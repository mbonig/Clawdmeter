#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "hx8394/esp_lcd_hx8394.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// First non-QSPI/SPI display in this project: a 720x1280 MIPI-DSI panel,
// driven directly against ESP-IDF's esp_lcd_mipi_dsi/esp_lcd_panel_ops APIs
// (called from Arduino framework code) via the vendored HX8394 driver in
// hx8394/ — GFX Library for Arduino has no MIPI-DSI panel class at all, so
// there's no Arduino_GFX-style bus/driver object here like every other
// board. The DPI panel created by esp_lcd_new_panel_dpi() (inside
// esp_lcd_new_panel_hx8394) already implements draw_bitmap against its
// auto-refreshing framebuffer — no CPU-side rotation or partial-flush
// bookkeeping needed. See board.h and openspec/changes/add-esp32-p4-core-board/
// for why this board exists without BLE.

static esp_lcd_panel_handle_t     panel = nullptr;
static esp_lcd_panel_io_handle_t  panel_io = nullptr;
static esp_lcd_dsi_bus_handle_t   dsi_bus = nullptr;
static esp_ldo_channel_handle_t   dsi_phy_ldo = nullptr;

// esp_lcd_panel_draw_bitmap() on a DPI panel is async (queues a DMA2D copy
// into the framebuffer, returns immediately) and gates re-entry with its own
// internal non-blocking semaphore check (esp_lcd_panel_dpi.c:
// `xSemaphoreTake(dpi_panel->draw_sem, 0)`) — if the previous copy hasn't
// finished, it returns ESP_ERR_INVALID_STATE rather than blocking. An earlier
// attempt at gating this from our side with a *separate* semaphore mirrored
// off the on_color_trans_done callback compiled and flashed fine but hung
// setup() completely with a black screen on real hardware — most likely a
// degenerate zero-size LVGL flush region (or some other edge case) skips
// firing the completion callback while still consuming a wait, permanently
// desyncing our semaphore from the driver's real internal state. Retrying
// directly on the driver's own authoritative return code avoids that
// desync risk entirely.
static void wait_and_draw(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const uint16_t* pixels) {
    esp_err_t err;
    do {
        err = esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels);
        if (err == ESP_ERR_INVALID_STATE) vTaskDelay(1);
    } while (err == ESP_ERR_INVALID_STATE);
}

void display_hal_init(void) {
    // ESP32-P4's MIPI DPHY needs a dedicated 2.5V rail from the chip's
    // internal adjustable LDO before the DSI bus can be created.
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN;
    ldo_cfg.voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV;
    if (esp_ldo_acquire_channel(&ldo_cfg, &dsi_phy_ldo) != ESP_OK) {
        Serial.println("DSI PHY LDO power-on failed");
        return;
    }

    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = LCD_MIPI_DSI_LANE_NUM;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_BITRATE_MBPS;
    if (esp_lcd_new_dsi_bus(&bus_config, &dsi_bus) != ESP_OK) {
        Serial.println("DSI bus init failed");
        return;
    }

    esp_lcd_dbi_io_config_t dbi_config = {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &panel_io) != ESP_OK) {
        Serial.println("Panel IO init failed");
        return;
    }

    // Same values as Waveshare's own official BSP macro for this exact panel
    // (HX8394_720_1280_PANEL_30HZ_DPI_CONFIG in hx8394/esp_lcd_hx8394.h) —
    // written out manually because that macro's nested `.flags.use_dma2d =`
    // designator is a C-only construct that doesn't compile as C++.
    static esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 58;
    dpi_config.virtual_channel = 0;
    dpi_config.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    // Single framebuffer, matching Waveshare's raw macro default (their BSP
    // bumps this to 3 for tearing-avoidance, but that requires cooperating
    // buffer-rotation bookkeeping via their esp_lv_adapter component, which
    // this HAL doesn't have — with num_fbs=3 and no such bookkeeping, our
    // draw_bitmap calls land in whichever buffer the driver rotates to
    // internally, which isn't necessarily the one actually being scanned
    // out: backlight and the DSI stream both worked, but nothing ever
    // rendered on the physical panel (confirmed on hardware). With a single
    // buffer there's no rotation ambiguity — every draw targets the one
    // buffer that's always on screen. The earlier "previous draw operation
    // is not finished" error this was meant to fix is already handled by
    // wait_and_draw()'s retry loop below, independent of buffer count.
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = LCD_WIDTH;
    dpi_config.video_timing.v_size = LCD_HEIGHT;
    dpi_config.video_timing.hsync_back_porch = 20;
    dpi_config.video_timing.hsync_pulse_width = 20;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 10;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_front_porch = 24;
    // DMA2D (async) was the initial choice, but it only queues the copy and
    // returns immediately — completion happens later via a callback. LVGL
    // calls lv_display_flush_ready() as soon as display_hal_draw_bitmap()
    // returns, believing the source buffer is free to reuse; with only 2
    // LVGL render buffers ping-ponging, a lagging DMA2D copy could still be
    // reading a buffer LVGL has already started overwriting with the next
    // region. Confirmed on hardware: a raw solid-color fill_screen() (no
    // LVGL involved) rendered correctly, but real LVGL/splash content never
    // appeared — exactly the signature of this race, not a display-driver
    // bug. The CPU-copy path (use_dma2d=false) is fully synchronous: by the
    // time esp_lcd_panel_draw_bitmap() returns, the memcpy has already
    // completed, so there's no race, at the cost of using the CPU instead of
    // the DMA2D peripheral for the copy — irrelevant at this UI's update rate.
    dpi_config.flags.use_dma2d = false;

    static hx8394_vendor_config_t vendor_config = {};
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.mipi_config.lane_num = LCD_MIPI_DSI_LANE_NUM;
    // vendor_config.init_cmds left null — falls back to the driver's own
    // built-in default HX8394 init sequence (vendor_specific_init_code_default
    // in hx8394/esp_lcd_hx8394.c), the same one Waveshare's own demo uses.

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_RESET_GPIO;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    panel_config.flags.reset_active_high = 1;

    if (esp_lcd_new_panel_hx8394(panel_io, &panel_config, &panel) != ESP_OK) {
        Serial.println("HX8394 panel create failed");
        panel = nullptr;
    }
}

void display_hal_begin(void) {
    if (!panel) return;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_disp_on_off(panel, true);

    ledcAttach(LCD_BACKLIGHT_GPIO, LCD_BACKLIGHT_LEDC_FREQ_HZ, LCD_BACKLIGHT_LEDC_RES_BITS);
    display_hal_fill_screen(0x0000);
    display_hal_set_brightness(200);
}

void display_hal_set_brightness(uint8_t level) {
    uint32_t max_duty = (1u << LCD_BACKLIGHT_LEDC_RES_BITS) - 1;
    ledcWrite(LCD_BACKLIGHT_GPIO, (uint32_t)level * max_duty / 255);
}

void display_hal_fill_screen(uint16_t color) {
    if (!panel) return;
    const int32_t w = LCD_WIDTH;
    const int32_t chunk_h = 40;
    static uint16_t* buf = nullptr;
    if (!buf) buf = (uint16_t*)heap_caps_malloc(w * chunk_h * 2, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    for (int32_t i = 0; i < w * chunk_h; i++) buf[i] = color;
    for (int32_t y = 0; y < LCD_HEIGHT; y += chunk_h) {
        int32_t h = (y + chunk_h <= LCD_HEIGHT) ? chunk_h : (LCD_HEIGHT - y);
        wait_and_draw(0, y, w, y + h, buf);
    }
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!panel) return;
    wait_and_draw(x, y, x + w, y + h, pixels);
}

void display_hal_tick(void) {
    // No rotation handling on this board.
}

void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    // No even-alignment requirement for this panel's framebuffer copy.
    (void)x1; (void)y1; (void)x2; (void)y2;
}
