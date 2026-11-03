#pragma once

#include "lvgl.h"

/* BSP display and touch setup, with a reusable internal DMA transfer buffer. */
lv_display_t *display_port_start(void);
