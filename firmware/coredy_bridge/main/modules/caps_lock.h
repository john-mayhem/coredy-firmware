#ifndef _CAPS_LOCK_H_
#define _CAPS_LOCK_H_

// One global recursive mutex guarding every capability's value storage.
//
// Why this exists: the STDK-generated caps_*.c pattern stores string
// attributes as a heap pointer and updates them with free()+strdup(). That is
// safe in STDK's own sample apps, which only ever write a capability from one
// place. This app writes from TWO tasks:
//
//   * tuya_uart_rx  -- a DP report from the STM32 lands in coredy_bridge's
//                      on_dp() and calls set_*()/send() directly.
//   * STDK's command task -- an app tap lands in a caps_*_cmd_*_cb() which
//                      calls the very same set_*()/send() on the same object.
//
// Without a lock, a tap arriving while a DP report is mid-send frees the
// string the other task is reading -> use-after-free. Found by inspection
// 2026-08-31; the RTC crash counter in main.c exists to catch exactly this
// class of fault.
//
// Recursive so a caller may hold it across a set+send pair (keeping the two
// atomic) without deadlocking against the lock taken inside each function.
//
// Lock ordering, must not be violated: caps_lock -> tuya_uart's TX lock.
// Never the reverse. on_dp() takes caps_lock and never transmits; the command
// path takes caps_lock then transmits. Nothing takes caps_lock while holding
// the UART lock.

void caps_lock_init(void);
void caps_lock_take(void);
void caps_lock_give(void);

#endif
