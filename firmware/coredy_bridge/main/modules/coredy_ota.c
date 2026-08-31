#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

#include "coredy_ota.h"
#include "coredy_bridge.h"

static const char *TAG = "COREDY_OTA";

#define COREDY_OTA_VERSIONINFO_URL \
    "https://github.com/john-mayhem/coredy-firmware/releases/latest/download/versioninfo.json"

#define COREDY_OTA_SIGNATURE_SIZE       256  // RSA-2048 signature, appended as the last 256 bytes of the binary
#define COREDY_OTA_CHECK_INTERVAL_MS    30000 // check every 30s, continuously -- explicit project decision
                                               // 2026-08-31, not a rate-limit concern (unauthenticated GET
                                               // against a GitHub Releases CDN asset, not the REST API)
#define COREDY_OTA_HTTP_MAX_REDIRECTS   5
#define COREDY_OTA_BUF_SIZE             512
#define COREDY_OTA_VERSIONINFO_BUF_SIZE 1024
#define COREDY_OTA_SHA256_LEN           32

// How long a freshly-installed image gets to prove it can still reach the
// update server before we deliberately reboot into the bootloader's rollback.
// See the s_pending_verify comment below.
//
// 10 minutes = 20 attempts at the 30s poll interval. Normal boots mark valid
// at ~37s (assoc ~6s, cloud ~17s, first tick at 30s), so this is ~16x margin.
// Deliberately not tighter: the failure this guards against is permanent, but
// a transient outage that outlasts the deadline costs a rollback and then a
// re-download once connectivity returns -- i.e. flash writes. Generous is
// cheap; too tight risks cycling on a flaky link.
#define COREDY_OTA_VERIFY_DEADLINE_MS   (10 * 60 * 1000)

// True when the bootloader started us in PENDING_VERIFY, i.e. we are a
// just-installed OTA image on probation.
//
// This matters enormously now that the ESP32 is sealed inside the robot. The
// rollback guard only protects against an image that fails to BOOT. It does
// nothing about the far more likely failure: an image that boots fine, talks
// to SmartThings fine, and cannot fetch updates -- which is a permanent dead
// end reachable only by desoldering. That exact state is what the device sat
// in before 2026-08-31.
//
// So: do not mark ourselves valid merely because the cloud connected. Mark
// valid only after a versioninfo.json fetch has actually succeeded, and if
// that never happens, reboot so the bootloader reverts to the last image that
// could. Failing to update is recoverable; losing the ability to update is not.
static bool s_pending_verify = false;
static bool s_fetch_ok_once = false;
static int64_t s_boot_ms = 0;

extern const uint8_t coredy_ota_public_key_pem_start[] asm("_binary_coredy_ota_public_key_pem_start");
extern const uint8_t coredy_ota_public_key_pem_end[]   asm("_binary_coredy_ota_public_key_pem_end");

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    (void)evt;
    return ESP_OK;
}

static void http_client_close_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

// GitHub's releases/.../latest/download/<asset> URLs are 302 redirects to
// the real asset host, carrying ~1KB signed query strings. Two things are
// needed to actually reach them with the esp_http_client_open()+
// fetch_headers()+read() streaming pattern used throughout this file
// (matching the SDK's own ota_demo reference, not esp_http_client_perform()):
//   1. Follow the redirect by hand -- fetch_headers() alone does not.
//   2. A large enough header buffer (see _http_client_create) to even hold
//      the redirect's Location: header without truncating.
// Both found live 2026-08-31 testing against the real repo; without them
// every fetch against a GitHub release silently came back empty/404.
static bool open_following_redirects(esp_http_client_handle_t client, int *out_status)
{
    for (int hop = 0; hop < COREDY_OTA_HTTP_MAX_REDIRECTS; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGE(TAG, "http open failed");
            return false;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }

        *out_status = status;
        return true;
    }
    ESP_LOGE(TAG, "too many redirects");
    return false;
}

static esp_http_client_handle_t http_client_create(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_handler,
        // GitHub's redirect targets carry ~1KB signed query strings; the
        // default 512-byte header buffers can't hold them.
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return NULL;
    if (esp_http_client_get_transport_type(client) != HTTP_TRANSPORT_OVER_SSL) {
        ESP_LOGE(TAG, "refusing non-HTTPS OTA URL");
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static bool fetch_url_to_buffer(const char *url, char *buf, size_t buf_size, size_t *out_len)
{
    esp_http_client_handle_t client = http_client_create(url);
    if (!client) return false;

    bool ok = false;
    int status = 0;
    if (open_following_redirects(client, &status)) {
        size_t total = 0;
        while (total < buf_size - 1) {
            int n = esp_http_client_read(client, buf + total, buf_size - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        buf[total] = '\0';
        *out_len = total;
        ok = (total > 0) && (status == 200);
        if (!ok) ESP_LOGW(TAG, "fetch %s failed, http status=%d", url, status);
    }
    http_client_close_cleanup(client);
    return ok;
}

static bool parse_versioninfo(const char *json, char *version_out, size_t version_out_len,
                               char *url_out, size_t url_out_len)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;
    cJSON *v = cJSON_GetObjectItem(root, "version");
    cJSON *u = cJSON_GetObjectItem(root, "url");
    if (cJSON_IsString(v) && cJSON_IsString(u)) {
        strncpy(version_out, v->valuestring, version_out_len - 1);
        version_out[version_out_len - 1] = '\0';
        strncpy(url_out, u->valuestring, url_out_len - 1);
        url_out[url_out_len - 1] = '\0';
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// Parses "1.2.3" (a leading 'v' is tolerated). Returns false on anything that
// isn't three dotted integers.
static bool parse_semver(const char *s, int out[3])
{
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

// Is `available` strictly newer than `current`?
//
// FAILS OPEN on purpose: if either string doesn't parse as semver we return
// true, so an unparseable version can only ever cause an unnecessary update,
// never block a needed one. For a sealed device, wrongly refusing an update is
// unrecoverable while wrongly taking one is not -- and the pre-1.0 images in
// the field report git-describe strings like "v2.3.2-dirty" that will never
// parse, yet must still be upgradeable.
static bool version_is_newer(const char *available, const char *current)
{
    int a[3], c[3];
    if (!parse_semver(available, a) || !parse_semver(current, c)) {
        ESP_LOGW(TAG, "unparseable version (available='%s' current='%s') -- treating as newer",
                 available ? available : "(null)", current ? current : "(null)");
        return true;
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] > c[i]) return true;
        if (a[i] < c[i]) return false;
    }
    return false; // identical
}

static bool verify_signature(const unsigned char *sha256, const unsigned char *sig, size_t sig_len)
{
    if (sig_len != COREDY_OTA_SIGNATURE_SIZE) {
        ESP_LOGE(TAG, "bad signature length %u (expected %u)",
                 (unsigned)sig_len, (unsigned)COREDY_OTA_SIGNATURE_SIZE);
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    const unsigned char *pub = coredy_ota_public_key_pem_start;
    size_t pub_len = coredy_ota_public_key_pem_end - coredy_ota_public_key_pem_start;

    bool ok = false;
    char err[100];
    int ret = mbedtls_pk_parse_public_key(&pk, pub, pub_len);
    if (ret != 0) {
        mbedtls_strerror(ret, err, sizeof(err));
        ESP_LOGE(TAG, "pk_parse_public_key failed: -0x%04x %s", -ret, err);
        goto done;
    }
    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_RSA)) {
        ESP_LOGE(TAG, "embedded key is not RSA");
        goto done;
    }
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, sha256, COREDY_OTA_SHA256_LEN, sig, COREDY_OTA_SIGNATURE_SIZE);
    if (ret != 0) {
        mbedtls_strerror(ret, err, sizeof(err));
        ESP_LOGE(TAG, "SIGNATURE VERIFY FAILED: -0x%04x %s -- refusing this image", -ret, err);
        goto done;
    }
    ok = true;

done:
    mbedtls_pk_free(&pk);
    return ok;
}

// Streams `url` into the inactive OTA partition, holding back the trailing
// COREDY_OTA_SIGNATURE_SIZE bytes as the signature, hashing everything else
// as it goes. Only stages the new partition as bootable if the signature
// verifies -- an unsigned/corrupt/short download is aborted and the current
// partition stays active.
static bool download_and_install(const char *url)
{
    esp_http_client_handle_t client = http_client_create(url);
    if (!client) return false;

    bool success = false;
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;
    char *buf = NULL;
    unsigned char sig[COREDY_OTA_SIGNATURE_SIZE];
    size_t sig_len = 0;
    mbedtls_sha256_context sha_ctx;
    bool sha_started = false;
    unsigned int firmware_len = 0;
    unsigned int total_read = 0;

    int status = 0;
    if (!open_following_redirects(client, &status)) {
        goto cleanup; // open_following_redirects already logged why
    }
    if (status != 200) {
        ESP_LOGE(TAG, "unexpected http status %d fetching firmware", status);
        goto cleanup;
    }

    int content_len = esp_http_client_get_content_length(client);
    if (content_len <= (int)COREDY_OTA_SIGNATURE_SIZE) {
        ESP_LOGE(TAG, "firmware content too small (%d)", content_len);
        goto cleanup;
    }
    firmware_len = (unsigned int)content_len - COREDY_OTA_SIGNATURE_SIZE;

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "no OTA update partition available");
        goto cleanup;
    }
    ESP_LOGI(TAG, "writing to partition subtype %d at 0x%x, firmware_len=%u",
             update_partition->subtype, (unsigned)update_partition->address, firmware_len);

    if (esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        update_handle = 0;
        goto cleanup;
    }

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    sha_started = true;

    buf = malloc(COREDY_OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating read buffer");
        goto cleanup;
    }

    while (1) {
        int n = esp_http_client_read(client, buf, COREDY_OTA_BUF_SIZE);
        if (n == 0) break;
        if (n < 0) {
            ESP_LOGE(TAG, "read error");
            goto cleanup;
        }

        int write_len = n;
        if (total_read + (unsigned)n > firmware_len) {
            unsigned int fw_remaining = firmware_len - total_read;
            unsigned int tail_len = (unsigned)n - fw_remaining;
            if (sig_len + tail_len > COREDY_OTA_SIGNATURE_SIZE) {
                ESP_LOGE(TAG, "signature buffer overflow (%u+%u)", (unsigned)sig_len, tail_len);
                goto cleanup;
            }
            memcpy(sig + sig_len, buf + fw_remaining, tail_len);
            sig_len += tail_len;
            write_len = (int)fw_remaining;
        }

        if (write_len > 0) {
            mbedtls_sha256_update(&sha_ctx, (const unsigned char *)buf, write_len);
            if (esp_ota_write(update_handle, buf, write_len) != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed");
                goto cleanup;
            }
            total_read += write_len;
        }
    }

    if (total_read != firmware_len) {
        ESP_LOGE(TAG, "incomplete download: got %u, expected %u", total_read, firmware_len);
        goto cleanup;
    }
    if (sig_len != COREDY_OTA_SIGNATURE_SIZE) {
        ESP_LOGE(TAG, "incomplete signature: got %u bytes", (unsigned)sig_len);
        goto cleanup;
    }

    unsigned char hash[COREDY_OTA_SHA256_LEN];
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    sha_started = false;

    if (!verify_signature(hash, sig, sig_len)) {
        goto cleanup; // verify_signature already logged why
    }

    if (esp_ota_end(update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (image invalid)");
        update_handle = 0;
        goto cleanup;
    }
    update_handle = 0;

    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        goto cleanup;
    }

    ESP_LOGW(TAG, "update verified and staged -- rebooting into it");
    success = true;

cleanup:
    if (sha_started) mbedtls_sha256_free(&sha_ctx);
    if (buf) free(buf);
    if (update_handle) esp_ota_abort(update_handle);
    http_client_close_cleanup(client);
    return success;
}

static void check_for_update(void)
{
    ESP_LOGI(TAG, "free heap before check: %u bytes (largest block %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    char *info_buf = malloc(COREDY_OTA_VERSIONINFO_BUF_SIZE);
    if (!info_buf) return;

    size_t info_len = 0;
    if (!fetch_url_to_buffer(COREDY_OTA_VERSIONINFO_URL, info_buf, COREDY_OTA_VERSIONINFO_BUF_SIZE, &info_len)) {
        free(info_buf);
        return;
    }

    // We just proved this image can still reach the update server -- the only
    // property that makes it safe to keep. Now it may cancel rollback.
    if (!s_fetch_ok_once) {
        s_fetch_ok_once = true;
        ESP_LOGI(TAG, "update server reachable -- image is self-updatable");
        coredy_ota_mark_valid();
    }

    char version[32] = {0};
    char url[512] = {0};
    bool parsed = parse_versioninfo(info_buf, version, sizeof(version), url, sizeof(url));
    free(info_buf);

    if (!parsed) {
        ESP_LOGW(TAG, "malformed versioninfo.json");
        return;
    }

    const char *current_version = esp_app_get_description()->version;
    ESP_LOGI(TAG, "current=%s available=%s", current_version, version);

    if (strcmp(version, current_version) == 0) {
        return; // already up to date
    }
    // Differing is not enough -- it must be NEWER. Without this, republishing
    // an older tag as "latest" silently downgrades every device, which is
    // exactly how the field unit ended up booting a pre-1.0.0 image out of
    // ota_1 and stranded itself there.
    if (!version_is_newer(version, current_version)) {
        ESP_LOGW(TAG, "published version %s is not newer than %s -- refusing to downgrade",
                 version, current_version);
        return;
    }

    ESP_LOGW(TAG, "new version %s available, downloading", version);
    if (download_and_install(url)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

static void ota_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(COREDY_OTA_CHECK_INTERVAL_MS));

        // A probationary image that cannot reach the update server is worse
        // than useless in a sealed robot -- reboot and let the bootloader put
        // the previous, known-updatable image back. Only ever fires for images
        // the bootloader itself flagged PENDING_VERIFY, so a hand-flashed
        // build with no internet will never reboot-loop on this.
        if (s_pending_verify && !s_fetch_ok_once &&
            (esp_timer_get_time() / 1000 - s_boot_ms) > COREDY_OTA_VERIFY_DEADLINE_MS) {
            ESP_LOGE(TAG, "no successful update check within %d s of boot while on probation -- "
                          "restarting to trigger rollback to the previous image",
                     COREDY_OTA_VERIFY_DEADLINE_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(500)); // let the log drain to UART/SSE first
            esp_restart();
        }

        // Skip this tick entirely (not just the network call) if the cloud
        // connection isn't currently settled -- see coredy_bridge.h for why.
        if (!coredy_bridge_is_cloud_connected()) {
            continue;
        }
        check_for_update();
    }
}

void coredy_ota_start(void)
{
    s_boot_ms = esp_timer_get_time() / 1000;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        ESP_LOGW(TAG, "image is PENDING_VERIFY -- it must reach the update server within %d s "
                      "or we roll back", COREDY_OTA_VERIFY_DEADLINE_MS / 1000);
    }

    xTaskCreate(ota_task, "coredy_ota", 8192, NULL, 5, NULL);
}

void coredy_ota_mark_valid(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "app marked valid, rollback cancelled");
    } else {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %d", err);
    }
}
