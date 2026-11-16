#include "fake_ble_gamepad.h"
#include "host_fakes.h"

bool (*fake_ble_connected)(void);
uint32_t fake_ble_raw;
ble_gamepad_profile_t fake_ble_profile;
bool fake_ble_paired;
esp_err_t fake_ble_start_error, fake_ble_stop_error;
unsigned fake_ble_starts, fake_ble_stops, fake_ble_forgets;
bool fake_ble_started;

esp_err_t ble_gamepad_start(void)
{
    host_trace("ble_start");
    ++fake_ble_starts;
    if (fake_ble_start_error) return fake_ble_start_error;
    fake_ble_started = true;
    return ESP_OK;
}

esp_err_t ble_gamepad_stop(void)
{
    host_trace("ble_stop");
    ++fake_ble_stops;
    if (fake_ble_stop_error) return fake_ble_stop_error;
    fake_ble_started = false;
    return ESP_OK;
}

bool ble_gamepad_connected(void) { return fake_ble_started && (fake_ble_connected ? fake_ble_connected() : true); }
uint32_t ble_gamepad_raw(void) { return fake_ble_raw; }
ble_gamepad_profile_t ble_gamepad_profile(void) { return fake_ble_profile; }
uint32_t ble_gamepad_reports(void) { return 0; }
void ble_gamepad_load(nvs_handle_t settings) { (void)settings; }
bool ble_gamepad_paired(void) { return fake_ble_paired; }
void ble_gamepad_forget(void) { ++fake_ble_forgets; fake_ble_paired = false; }

unsigned fake_ble_found_count;
int fake_ble_pair_index = -1;
bool fake_ble_pair_succeeds = true;
void ble_gamepad_scan_begin(void) {}
bool ble_gamepad_scan_done(unsigned *count) { *count = fake_ble_found_count; return true; }
const char *ble_gamepad_scan_name(unsigned index) { return index ? "PAD TWO" : "PAD ONE"; }
int ble_gamepad_scan_rssi(unsigned index) { return -40 - 10 * (int)index; }
void ble_gamepad_pair(unsigned index) { fake_ble_pair_index = (int)index; fake_ble_paired = fake_ble_pair_succeeds; }
