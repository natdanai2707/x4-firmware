#pragma once

// Opaque field descriptor used only as an identity token by the eFuse API.
inline constexpr int CROSSPOINT_SIM_EFUSE_USER_DATA = 0;
#define ESP_EFUSE_USER_DATA (&CROSSPOINT_SIM_EFUSE_USER_DATA)
