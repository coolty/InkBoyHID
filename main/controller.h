#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"

typedef enum { CONTROLLER_BLUETOOTH, CONTROLLER_USB, CONTROLLER_TRANSPORT_COUNT } controller_transport_t;

/* Loads the saved transport; an unknown stored value means Bluetooth. Also loads the paired Bluetooth gamepad. */
void controller_load(nvs_handle_t settings);
controller_transport_t controller_transport(void);
const char *controller_transport_name(controller_transport_t transport);

/* Starts the saved transport. A USB failure falls back to Bluetooth for this
 * boot without changing the saved choice. */
esp_err_t controller_start(void);

/* Switches transport and saves the choice. On failure the previous transport
 * is restarted; controller_running() reports whether that worked. */
esp_err_t controller_select(controller_transport_t transport);
bool controller_running(void);

bool controller_connected(void);
/* Controller bits that are pressed: the 8 raw bits of the InkBoy controller, or the normalized
 * controls of a Bluetooth HID gamepad (see hid_gamepad.h). controller_profile() says which. */
uint32_t controller_raw(void);
typedef enum { CONTROLLER_PROFILE_INKBOY, CONTROLLER_PROFILE_HID } controller_profile_t;
controller_profile_t controller_profile(void);
uint32_t controller_reports(void);

/* Bluetooth pairing memory for HID gamepads. Once one is paired no other gamepad is accepted until
 * it is forgotten. The InkBoy controller is never part of this. */
bool controller_pad_paired(void);
void controller_forget_pad(void);
/* Pairing menu: scan (about five seconds, poll controller_scan_done), then pair one result. Pairing is
 * complete when controller_pad_paired() turns true. */
#define CONTROLLER_SCAN_MAX 8u
void controller_scan_begin(void);
bool controller_scan_done(unsigned *count);
const char *controller_scan_name(unsigned index);
int controller_scan_rssi(unsigned index);
void controller_scan_pair(unsigned index);

/* Startup serial bypass and runtime USB controller/serial toggle. Neither saves NVS. */
void controller_request_console(void);
esp_err_t controller_toggle_usb(void);

/* Snapshot used by menu badges; selected transport and live connection are separate. */
typedef enum { CONTROLLER_BLE_OFF, CONTROLLER_BLE_WAIT, CONTROLLER_BLE_CONNECTED,
               CONTROLLER_USB_OFF, CONTROLLER_USB_WAIT, CONTROLLER_USB_CONNECTED,
               CONTROLLER_USB_PC } controller_status_t;
controller_status_t controller_status(void);
