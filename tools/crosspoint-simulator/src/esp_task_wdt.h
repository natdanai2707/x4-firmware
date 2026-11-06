#pragma once

#include "esp_err.h"

inline esp_err_t esp_task_wdt_reset() { return ESP_OK; }
inline esp_err_t esp_task_wdt_init(int, bool) { return ESP_OK; }
inline esp_err_t esp_task_wdt_add(void *) { return ESP_OK; }
inline esp_err_t esp_task_wdt_delete(void *) { return ESP_OK; }
inline esp_err_t esp_task_wdt_status(void *) { return ESP_OK; }
