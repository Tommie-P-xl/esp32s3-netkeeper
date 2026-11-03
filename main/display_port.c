#include "display_port.h"

#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"

static const char *TAG = "display_port";
static uint8_t *s_tx_buf;
static size_t s_tx_size;

static esp_err_t draw_bitmap(lv_display_t *disp, esp_lcd_panel_handle_t panel,
                             int x1, int y1, int x2, int y2,
                             const void *pixels, void *ctx)
{
    (void)disp;
    (void)ctx;
    if (x2 <= x1 || y2 <= y1) return ESP_ERR_INVALID_ARG;
    const size_t bytes = (size_t)(x2 - x1) * (y2 - y1) * sizeof(uint16_t);
    if (bytes > s_tx_size) return ESP_ERR_INVALID_SIZE;

    /* Adapter has already packed the rows and swapped RGB565 bytes. LVGL
     * waits for the previous transfer's done ISR before the next flush, so
     * this buffer can be reused without changing it while DMA is reading it.
     * Internal DMA memory avoids SPI's per-flush PSRAM bounce allocation. */
    memcpy(s_tx_buf, pixels, bytes);
    return esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, s_tx_buf);
}

lv_display_t *display_port_start(void)
{
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) return NULL;

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    s_tx_size = lv_display_get_draw_buf_size(disp);
    s_tx_buf = heap_caps_malloc(s_tx_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_tx_buf == NULL) {
        ESP_LOGE(TAG, "cannot reserve %u-byte display DMA buffer", (unsigned)s_tx_size);
        esp_lv_adapter_unlock();
        return NULL;
    }
    const esp_lv_adapter_draw_bitmap_callbacks_t cbs = {
        .custom_draw_bitmap = draw_bitmap,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_set_draw_bitmap_callbacks(disp, &cbs, NULL));
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "reserved %u-byte display DMA buffer", (unsigned)s_tx_size);
    return disp;
}
