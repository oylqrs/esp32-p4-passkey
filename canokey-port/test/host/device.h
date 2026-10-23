#pragma once
#include <stdint.h>
#define TOUCH_NO 0
#define TOUCH_SHORT 1
#define TOUCH_CANCEL 3
typedef enum {DEVICE_APPLET_SESSION_NONE, DEVICE_APPLET_SESSION_CCID, DEVICE_APPLET_SESSION_CTAPHID, DEVICE_APPLET_SESSION_WEBUSB} device_applet_session_owner_t;
uint32_t device_get_tick(void);
void device_applet_session_touch(device_applet_session_owner_t owner);
void device_applet_session_release(device_applet_session_owner_t owner);
int device_applet_session_acquire(device_applet_session_owner_t owner);
int device_applet_session_reset(device_applet_session_owner_t owner);
device_applet_session_owner_t device_applet_session_owner(void);
void stop_blinking(void);
void set_touch_result(uint8_t result);
uint8_t get_touch_result(void);
void set_nfc_state(uint8_t state);
