#include "esp_random.h"
#include "host_fakes.h"

/* A test sets this to choose what the "hardware" random number generator returns. */
uint32_t host_random_value;

uint32_t esp_random(void) { return host_random_value; }
