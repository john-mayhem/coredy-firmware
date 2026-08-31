#include "caps_lock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;

void caps_lock_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
}

// Both helpers tolerate being called before caps_lock_init() (they simply do
// nothing). That keeps early-boot capability initialisation from depending on
// init order -- at that point only one task exists, so there is nothing to
// race against anyway.
void caps_lock_take(void)
{
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void caps_lock_give(void)
{
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}
