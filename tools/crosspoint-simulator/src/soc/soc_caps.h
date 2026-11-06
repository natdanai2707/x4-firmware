#pragma once

// No native wake-capability model: skip the ESP32-C3 battery-latch path.
#define SOC_PM_SUPPORT_EXT1_WAKEUP 1
