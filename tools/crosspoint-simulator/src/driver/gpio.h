#pragma once

using gpio_num_t = int;

constexpr gpio_num_t GPIO_NUM_13 = 13;

enum gpio_mode_t { GPIO_MODE_OUTPUT };

inline void gpio_hold_dis(gpio_num_t /*pin*/) {}
inline void gpio_set_direction(gpio_num_t /*pin*/, gpio_mode_t /*mode*/) {}
inline void gpio_set_level(gpio_num_t /*pin*/, int /*level*/) {}
