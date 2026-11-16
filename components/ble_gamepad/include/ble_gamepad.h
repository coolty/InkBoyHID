#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "nvs.h"
/* INKBOY: the custom controller, raw bits as sent. HID: any BLE HID gamepad; raw holds the normalized
 * controls of hid_gamepad.h. */
typedef enum { BLE_GAMEPAD_INKBOY, BLE_GAMEPAD_HID } ble_gamepad_profile_t;
/* Start and stop are idempotent. Stop ends scanning and releases the
 * controller, returning ESP_ERR_TIMEOUT if either is still in progress. */
esp_err_t ble_gamepad_start(void);
esp_err_t ble_gamepad_stop(void);
bool ble_gamepad_connected(void);
uint32_t ble_gamepad_raw(void);
/* Kind of the controller connected last; meaningful while connected. */
ble_gamepad_profile_t ble_gamepad_profile(void);
uint32_t ble_gamepad_reports(void);

/* Pairing memory. A HID pad is paired explicitly: scan, then pair one of the results. Only the paired
 * pad (by address or name) reconnects by itself, so a neighbour's controller is never taken. Load before
 * ble_gamepad_start(). Forget unpairs it, drops the bond and disconnects. The InkBoy controller is
 * independent of all of this. */
void ble_gamepad_load(nvs_handle_t settings);
bool ble_gamepad_paired(void);
void ble_gamepad_forget(void);

/* Pairing menu. Begin a five second scan, poll until done, then read the results (strongest first) and
 * pair one. After pair, ble_gamepad_paired() turns true once the pad connected and was saved. All of it
 * runs on the connect task, so each call returns at once; the Bluetooth transport must be running. */
#define BLE_GAMEPAD_FOUND_MAX 8
void ble_gamepad_scan_begin(void);
bool ble_gamepad_scan_done(unsigned *count);
const char *ble_gamepad_scan_name(unsigned index);
int ble_gamepad_scan_rssi(unsigned index);
void ble_gamepad_pair(unsigned index);
