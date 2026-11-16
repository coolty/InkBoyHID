#pragma once
#include "ble_gamepad.h"

extern bool (*fake_ble_connected)(void);
extern uint32_t fake_ble_raw;
extern ble_gamepad_profile_t fake_ble_profile;
extern bool fake_ble_paired;
extern esp_err_t fake_ble_start_error, fake_ble_stop_error;
extern unsigned fake_ble_starts, fake_ble_stops, fake_ble_forgets;
extern bool fake_ble_started;
extern unsigned fake_ble_found_count;
extern int fake_ble_pair_index;
extern bool fake_ble_pair_succeeds;
