/*
 * Idle / power management for the 05_Spec_Analyzer example.
 *
 * Timeline, measured from the last touch on the panel:
 *
 *   0 s .. 10 s    active    : spectrum refreshed at ~30 fps
 *  10 s .. 70 s    screen off: brightness 0, analyzer and renderer skip work
 *  70 s and later  idle      : CPU dropped to 80 MHz; the panel stays dark
 *   any touch      -> panel on within ~10 ms
 *
 * Why this does not use esp_lv_adapter's auto-sleep framework:
 * its PAUSE mode deadlocks the LVGL worker on this board. The adapter pauses
 * itself from inside the worker task, and once entered the worker never
 * observes the wake request - measured on hardware, the touch INT fired 100+
 * times and the wake request was posted, yet "Auto sleep wake requested" never
 * appeared, so the exit callback never ran and the panel stayed dark.
 *
 * Blanking only needs a brightness write (the CO5300 keeps its GRAM while the
 * brightness is 0), so this module drives it directly and leaves LVGL alone.
 * Touch is detected by polling the controller's INT level: an edge interrupt
 * alone is unreliable because the CST9217 holds INT asserted until the host
 * reads it, which is exactly what stops happening once the panel is blanked.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the display.
 *
 * Thin wrapper over bsp_display_start(); kept so the application has a single
 * entry point for display + idle handling.
 */
lv_display_t *power_mgr_display_start(void);

/**
 * @brief Start the idle supervisor. Call after the UI has been created.
 */
esp_err_t power_mgr_start(void);

/**
 * @brief Whether the panel is currently lit.
 *
 * The analyzer and the canvas renderer skip their work while this is false.
 */
bool power_mgr_screen_is_on(void);

/**
 * @brief Whether the CPU has been dropped to the idle frequency.
 *
 * The battery estimator uses this to pick a lower discharge current.
 */
bool power_mgr_is_low_power(void);

#ifdef __cplusplus
}
#endif
