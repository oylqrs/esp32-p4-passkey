#include "FIDO_Management.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "canokey_esp32p4.h"
#include "ctap-internal.h"
#include "esp_log.h"
#include "fs.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_status_bar.h"

#define FIDO_MANAGEMENT_WIDTH       480
#define FIDO_MANAGEMENT_MAX_RP_ROWS 24
#define FIDO_USER_IDS_TEXT_MAX      192
#define FIDO_USER_NAME_TEXT_MAX     160

static const char *TAG = "ui_fido_mgmt";

typedef struct {
    char rp_id[MAX_STORED_RPID_LENGTH + 1];
    uint8_t rp_id_hash[SHA256_DIGEST_LENGTH];
    uint32_t live_count;
    char user_ids[FIDO_USER_IDS_TEXT_MAX];
    char user_names[FIDO_USER_NAME_TEXT_MAX];
} fido_rp_item_t;

static fido_rp_item_t s_rp_items[FIDO_MANAGEMENT_MAX_RP_ROWS];
static size_t s_rp_item_count;

static bool bytes_are_printable_ascii(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x20 || data[i] > 0x7e) {
            return false;
        }
    }

    return true;
}

static void format_user_id(const uint8_t *user_id, size_t user_id_len, char *out, size_t out_len)
{
    if (out_len == 0) {
        return;
    }

    if (user_id_len == 0) {
        strlcpy(out, "(empty)", out_len);
        return;
    }

    if (bytes_are_printable_ascii(user_id, user_id_len)) {
        size_t copy_len = user_id_len < out_len - 1 ? user_id_len : out_len - 1;
        memcpy(out, user_id, copy_len);
        out[copy_len] = '\0';
        return;
    }

    size_t pos = 0;
    for (size_t i = 0; i < user_id_len && pos + 2 < out_len; i++) {
        int written = snprintf(out + pos, out_len - pos, "%02x", user_id[i]);
        if (written < 0) {
            break;
        }
        pos += (size_t)written;
    }
    out[pos < out_len ? pos : out_len - 1] = '\0';
}

static void append_user_id_text(fido_rp_item_t *item, const user_entity *user)
{
    char one_id[USER_ID_MAX_SIZE * 2 + 1];
    format_user_id(user->id, user->id_size, one_id, sizeof(one_id));

    size_t used = strlen(item->user_ids);
    if (used >= sizeof(item->user_ids) - 1) {
        return;
    }

    int written = snprintf(item->user_ids + used,
                           sizeof(item->user_ids) - used,
                           "%s%s",
                           used > 0 ? ", " : "",
                           one_id);
    if (written < 0) {
        item->user_ids[used] = '\0';
    }
}

static void append_user_name_text(fido_rp_item_t *item, const user_entity *user)
{
    const char *name = user->name[0] ? user->name : "(none)";
    size_t used = strlen(item->user_names);
    if (used >= sizeof(item->user_names) - 1) {
        return;
    }

    int written = snprintf(item->user_names + used,
                           sizeof(item->user_names) - used,
                           "%s%s",
                           used > 0 ? ", " : "",
                           name);
    if (written < 0) {
        item->user_names[used] = '\0';
    }
}

static void load_fido_user_ids(void)
{
    int dc_size = get_file_size(DC_FILE);
    if (dc_size <= 0) {
        ESP_LOGI(TAG, "no FIDO discoverable credentials: size=%d", dc_size);
        return;
    }

    size_t dc_count = (size_t)dc_size / sizeof(CTAP_discoverable_credential);
    ESP_LOGI(TAG, "load FIDO discoverable credentials size=%d count=%u",
             dc_size,
             (unsigned)dc_count);

    for (size_t i = 0; i < dc_count; i++) {
        CTAP_discoverable_credential dc = {0};
        int ret = read_file(DC_FILE,
                            &dc,
                            (lfs_soff_t)(i * sizeof(CTAP_discoverable_credential)),
                            sizeof(dc));
        if (ret != (int)sizeof(dc)) {
            ESP_LOGW(TAG, "read FIDO credential[%u] failed ret=%d", (unsigned)i, ret);
            continue;
        }

        if (dc.deleted) {
            continue;
        }

        for (size_t rp = 0; rp < s_rp_item_count; rp++) {
            if (memcmp(dc.credential_id.rp_id_hash,
                       s_rp_items[rp].rp_id_hash,
                       SHA256_DIGEST_LENGTH) == 0) {
                append_user_id_text(&s_rp_items[rp], &dc.user);
                append_user_name_text(&s_rp_items[rp], &dc.user);
                ESP_LOGI(TAG,
                         "FIDO credential[%u] rp=%s usr.id.len=%u usr.name=%s",
                         (unsigned)i,
                         s_rp_items[rp].rp_id,
                         (unsigned)dc.user.id_size,
                         dc.user.name);
                break;
            }
        }
    }
}

static size_t load_fido_rp_items(void)
{
    memset(s_rp_items, 0, sizeof(s_rp_items));
    s_rp_item_count = 0;

    esp_err_t flash_ret = canokey_esp32p4_flash_init();
    if (flash_ret != ESP_OK) {
        ESP_LOGW(TAG, "cannot load FIDO credentials: flash init failed %s", esp_err_to_name(flash_ret));
        return 0;
    }

    int meta_size = get_file_size(DC_META_FILE);
    if (meta_size <= 0) {
        ESP_LOGI(TAG, "no FIDO rp metadata: size=%d", meta_size);
        return 0;
    }

    size_t meta_count = (size_t)meta_size / sizeof(CTAP_rp_meta);
    ESP_LOGI(TAG, "load FIDO rp metadata size=%d count=%u",
             meta_size,
             (unsigned)meta_count);

    for (size_t i = 0; i < meta_count && s_rp_item_count < FIDO_MANAGEMENT_MAX_RP_ROWS; i++) {
        CTAP_rp_meta meta = {0};
        int ret = read_file(DC_META_FILE,
                            &meta,
                            (lfs_soff_t)(i * sizeof(CTAP_rp_meta)),
                            sizeof(meta));
        if (ret != (int)sizeof(meta)) {
            ESP_LOGW(TAG, "read FIDO rp metadata[%u] failed ret=%d", (unsigned)i, ret);
            continue;
        }

        if (meta.deleted || meta.live_count == 0 || meta.rp_id_len == 0) {
            continue;
        }

        size_t rp_len = meta.rp_id_len;
        if (rp_len > MAX_STORED_RPID_LENGTH) {
            rp_len = MAX_STORED_RPID_LENGTH;
        }

        memcpy(s_rp_items[s_rp_item_count].rp_id, meta.rp_id, rp_len);
        s_rp_items[s_rp_item_count].rp_id[rp_len] = '\0';
        memcpy(s_rp_items[s_rp_item_count].rp_id_hash, meta.rp_id_hash, sizeof(meta.rp_id_hash));
        s_rp_items[s_rp_item_count].live_count = meta.live_count;
        ESP_LOGI(TAG,
                 "FIDO rp[%u] id=%s live=%lu",
                 (unsigned)s_rp_item_count,
                 s_rp_items[s_rp_item_count].rp_id,
                 (unsigned long)meta.live_count);
        s_rp_item_count++;
    }

    load_fido_user_ids();
    return s_rp_item_count;
}

static void add_rp_row(lv_obj_t *parent, const fido_rp_item_t *item)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, FIDO_MANAGEMENT_WIDTH - 36, 210);//136
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 58, 58);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xB8A8FF), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_box, 6, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_center(icon);

    lv_obj_t *rp_label = lv_label_create(row);
    lv_label_set_text(rp_label, item->rp_id);
    lv_obj_set_width(rp_label, FIDO_MANAGEMENT_WIDTH - 150);
    lv_label_set_long_mode(rp_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(rp_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(rp_label, &lv_font_montserrat_28, 0);
    lv_obj_align(rp_label, LV_ALIGN_TOP_LEFT, 88, 18);

    char count_text[32];
    snprintf(count_text, sizeof(count_text), ui_i18n_get(UI_STR_CREDENTIALS), (unsigned long)item->live_count);
    lv_obj_t *count_label = lv_label_create(row);
    lv_label_set_text(count_label, count_text);
    lv_obj_set_width(count_label, FIDO_MANAGEMENT_WIDTH - 150);
    lv_label_set_long_mode(count_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(count_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(count_label, ui_i18n_font_16(), 0);
    lv_obj_align(count_label, LV_ALIGN_TOP_LEFT, 88, 56);

    char user_ids_text[FIDO_USER_IDS_TEXT_MAX + 16];
    snprintf(user_ids_text,
             sizeof(user_ids_text),
             ui_i18n_get(UI_STR_USER_ID),
             item->user_ids[0] ? item->user_ids : "(none)");
    lv_obj_t *user_ids_label = lv_label_create(row);
    lv_label_set_text(user_ids_label, user_ids_text);
    lv_obj_set_width(user_ids_label, FIDO_MANAGEMENT_WIDTH - 150);
    lv_label_set_long_mode(user_ids_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(user_ids_label, lv_color_hex(0xC5C9D1), 0);
    lv_obj_set_style_text_font(user_ids_label, ui_i18n_font_16(), 0);
    lv_obj_align(user_ids_label, LV_ALIGN_TOP_LEFT, 88, 80);

    char user_names_text[FIDO_USER_NAME_TEXT_MAX + 16];
    snprintf(user_names_text,
             sizeof(user_names_text),
             ui_i18n_get(UI_STR_USER_NAME),
             item->user_names[0] ? item->user_names : "(none)");
    lv_obj_t *user_names_label = lv_label_create(row);
    lv_label_set_text(user_names_label, user_names_text);
    lv_obj_set_width(user_names_label, FIDO_MANAGEMENT_WIDTH - 150);
    lv_label_set_long_mode(user_names_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(user_names_label, lv_color_hex(0xC5C9D1), 0);
    lv_obj_set_style_text_font(user_names_label, ui_i18n_font_16(), 0);
    lv_obj_align(user_names_label, LV_ALIGN_TOP_LEFT, 88, 150);
}

static void add_empty_state(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, FIDO_MANAGEMENT_WIDTH - 36, 150);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 4, 0);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, ui_i18n_get(UI_STR_NO_FIDO_KEYS));
    lv_obj_set_width(label, FIDO_MANAGEMENT_WIDTH - 72);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(0xD7DAE0), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, ui_i18n_get(UI_STR_FIDO_KEYS_HINT));
    lv_obj_set_width(hint, FIDO_MANAGEMENT_WIDTH - 72);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(hint, ui_i18n_font_16(), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 26);
}

void FIDO_Management_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, FIDO_MANAGEMENT_WIDTH);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, FIDO_MANAGEMENT_WIDTH, 600);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_pad_left(list, 18, 0);
    lv_obj_set_style_pad_right(list, 18, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    size_t count = load_fido_rp_items();
    if (count == 0) {
        add_empty_state(list);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        add_rp_row(list, &s_rp_items[i]);
    }
}
