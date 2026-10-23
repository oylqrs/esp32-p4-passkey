#include "wallpaper.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/unlock_pin_ui.h"
#include "ui_clock_desktop.h"
#include "ui_nav.h"

#define WALLPAPER_WIDTH  480
#define WALLPAPER_HEIGHT 640
#define WALLPAPER_PATH   "/data/wallpaper.bin"
#define WALLPAPER_STRIDE (WALLPAPER_WIDTH * sizeof(uint16_t))
#define WALLPAPER_SIZE   (WALLPAPER_STRIDE * WALLPAPER_HEIGHT)
#define WALLPAPER_FILE_SIZE (WALLPAPER_SIZE + sizeof(lv_image_header_t))
#define WALLPAPER_OPEN_RETRIES 20
#define WALLPAPER_OPEN_RETRY_MS 100

static const char *TAG = "wallpaper";
static uint8_t *s_wallpaper_data;
static lv_image_dsc_t s_wallpaper_dsc;
static bool s_load_attempted;
static bool s_wallpaper_ready;

extern const uint8_t wallpaper_bin_start[]
    asm("_binary_wallpaper_bin_start");
extern const uint8_t wallpaper_bin_end[]
    asm("_binary_wallpaper_bin_end");

static bool wallpaper_header_is_valid(const lv_image_header_t *header)
{
    return header->magic == LV_IMAGE_HEADER_MAGIC &&
           header->cf == LV_COLOR_FORMAT_RGB565 &&
           (header->flags & LV_IMAGE_FLAGS_COMPRESSED) == 0 &&
           header->w == WALLPAPER_WIDTH &&
           header->h == WALLPAPER_HEIGHT &&
           header->stride == WALLPAPER_STRIDE;
}

static void set_wallpaper_descriptor(const uint8_t *pixels)
{
    s_wallpaper_dsc = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .flags = 0,
            .w = WALLPAPER_WIDTH,
            .h = WALLPAPER_HEIGHT,
            .stride = WALLPAPER_STRIDE,
            .reserved_2 = 0,
        },
        .data_size = WALLPAPER_SIZE,
        .data = pixels,
        .reserved = NULL,
    };
}

static bool use_embedded_wallpaper(void)
{
    size_t embedded_size = (size_t)(wallpaper_bin_end - wallpaper_bin_start);
    if (embedded_size != WALLPAPER_FILE_SIZE) {
        ESP_LOGE(TAG, "Embedded wallpaper size is invalid: %u", (unsigned)embedded_size);
        return false;
    }

    lv_image_header_t header;
    memcpy(&header, wallpaper_bin_start, sizeof(header));
    if (!wallpaper_header_is_valid(&header)) {
        ESP_LOGE(TAG, "Embedded wallpaper header is invalid");
        return false;
    }

    set_wallpaper_descriptor(wallpaper_bin_start + sizeof(header));
    s_wallpaper_ready = true;
    ESP_LOGI(TAG, "Using embedded wallpaper");
    return true;
}

static bool load_wallpaper(void)
{
    if (s_wallpaper_ready) {
        return true;
    }
    if (s_load_attempted) {
        return false;
    }
    s_load_attempted = true;

    struct stat file_info = {0};
    int stat_error = 0;
    for (int attempt = 0; attempt < WALLPAPER_OPEN_RETRIES; attempt++) {
        if (stat(WALLPAPER_PATH, &file_info) == 0) {
            stat_error = 0;
            break;
        }
        stat_error = errno;
        vTaskDelay(pdMS_TO_TICKS(WALLPAPER_OPEN_RETRY_MS));
    }
    if (stat_error != 0) {
        ESP_LOGW(TAG, "Wallpaper not available from USB storage: %s (errno=%d)",
                 WALLPAPER_PATH, stat_error);
        return use_embedded_wallpaper();
    }

    bool has_lvgl_header = file_info.st_size >= 0 &&
                           (size_t)file_info.st_size ==
                               WALLPAPER_FILE_SIZE;
    if (file_info.st_size < 0 ||
        ((size_t)file_info.st_size != WALLPAPER_SIZE &&
         !has_lvgl_header)) {
        ESP_LOGW(TAG,
                 "Wallpaper size must be %u bytes (raw) or %u bytes (LVGL), got %ld: %s",
                 (unsigned)WALLPAPER_SIZE,
                 (unsigned)WALLPAPER_FILE_SIZE,
                 (long)file_info.st_size, WALLPAPER_PATH);
        return use_embedded_wallpaper();
    }

    FILE *file = fopen(WALLPAPER_PATH, "rb");
    if (!file) {
        ESP_LOGW(TAG, "Failed to open wallpaper: %s (errno=%d)",
                 WALLPAPER_PATH, errno);
        return use_embedded_wallpaper();
    }

    if (has_lvgl_header) {
        lv_image_header_t header;
        if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
            !wallpaper_header_is_valid(&header)) {
            fclose(file);
            ESP_LOGW(TAG, "Invalid LVGL wallpaper header: %s", WALLPAPER_PATH);
            return use_embedded_wallpaper();
        }
    }

    uint8_t *data = heap_caps_malloc(WALLPAPER_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        fclose(file);
        ESP_LOGW(TAG, "Not enough PSRAM for wallpaper (%u bytes)",
                 (unsigned)WALLPAPER_SIZE);
        return use_embedded_wallpaper();
    }

    size_t bytes_read = fread(data, 1, WALLPAPER_SIZE, file);
    int read_error = ferror(file);
    fclose(file);
    if (bytes_read != WALLPAPER_SIZE || read_error) {
        heap_caps_free(data);
        ESP_LOGW(TAG, "Failed to read wallpaper: %u/%u bytes",
                 (unsigned)bytes_read, (unsigned)WALLPAPER_SIZE);
        return use_embedded_wallpaper();
    }

    s_wallpaper_data = data;
    set_wallpaper_descriptor(s_wallpaper_data);
    s_wallpaper_ready = true;
    ESP_LOGI(TAG, "Loaded wallpaper (%s): %s",
             has_lvgl_header ? "LVGL BIN" : "raw RGB565", WALLPAPER_PATH);
    return true;
}

static void wallpaper_gesture_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT ||
        dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM) {
        lv_obj_t *screen = lv_event_get_current_target(event);
        if (screen) {
            lv_obj_remove_event_cb(screen, wallpaper_gesture_event_cb);
        }
        ui_nav_replace(unlock_pin_ui_show);
    }
}

void wallpaper_show(void)
{
    if (!load_wallpaper()) {
        ESP_LOGW(TAG, "Falling back to clock screen");
        ui_clock_desktop_show();
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, wallpaper_gesture_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *wallpaper = lv_image_create(screen);
    lv_image_set_src(wallpaper, &s_wallpaper_dsc);
    lv_obj_center(wallpaper);
    lv_obj_add_flag(wallpaper, LV_OBJ_FLAG_GESTURE_BUBBLE);
}
