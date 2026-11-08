#pragma once
#include "esp_err.h"
static inline esp_err_t esp_lv_adapter_lock(int ms) { (void)ms; return ESP_OK; }
static inline void esp_lv_adapter_unlock(void) {}
