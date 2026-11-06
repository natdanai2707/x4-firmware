#pragma once

#include <cstddef>

#include "esp_err.h"

inline esp_err_t esp_efuse_read_field_blob(const void * /*fieldBlob*/,
                                           void * /*dst*/,
                                           size_t /*bitCount*/) {
  // A desktop host has no ESP eFuse storage. Returning ESP_FAIL makes firmware
  // use its existing unavailable/unknown serial-number fallback.
  return ESP_FAIL;
}
