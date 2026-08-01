#include "kdbx_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "kdbx_internal.h"
#include "ui/ui_password_manager.h"

#define KDBX_DATA_DIR       "/data"
#define KDBX_PATH_MAX       256
#define KDBX_PASSWORD_MAX   128
#define KDBX_SCAN_MAX_DEPTH 4

static const char *TAG = "kdbx";
static kdbx_entry_t s_entries[KDBX_MAX_ENTRIES];
static size_t s_entry_count;

static int ascii_tolower(int ch)
{
    return tolower((unsigned char)ch);
}

static bool ascii_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_tolower(*a) != ascii_tolower(*b)) {
            return false;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static bool ends_with_ci(const char *name, const char *suffix)
{
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);
    if (len < suffix_len) {
        return false;
    }

    return ascii_equal_ci(name + len - suffix_len, suffix);
}

static bool looks_like_kdbx_name(const char *name)
{
    return ends_with_ci(name, ".kdbx") || ends_with_ci(name, ".kdb");
}

static esp_err_t scan_kdbx_dir(const char *dir_path, int depth, char *out_path, size_t out_path_len)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open %s: errno=%d", dir_path, errno);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[KDBX_PATH_MAX];
        int written = snprintf(child_path, sizeof(child_path), "%s/%s", dir_path, entry->d_name);
        if (written < 0 || written >= (int)sizeof(child_path)) {
            ESP_LOGW(TAG, "Path too long under %s: %s", dir_path, entry->d_name);
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        struct stat st;
        if (stat(child_path, &st) != 0) {
            ESP_LOGW(TAG, "Failed to stat %s: errno=%d", child_path, errno);
            continue;
        }

#if KDBX_VERBOSE_LOG
        ESP_LOGI(TAG, "%*s%s %s size=%ld",
                 depth * 2,
                 "",
                 S_ISDIR(st.st_mode) ? "[DIR]" : "[FILE]",
                 child_path,
                 (long)st.st_size);
#endif

        if (!S_ISDIR(st.st_mode) && looks_like_kdbx_name(entry->d_name)) {
            strlcpy(out_path, child_path, out_path_len);
            ESP_LOGI(TAG, "Found KDBX database: %s size=%ld", out_path, (long)st.st_size);
            ret = ESP_OK;
            break;
        }

        if (S_ISDIR(st.st_mode) && depth < KDBX_SCAN_MAX_DEPTH) {
            ret = scan_kdbx_dir(child_path, depth + 1, out_path, out_path_len);
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_SIZE) {
                break;
            }
        }
    }

    closedir(dir);
    return ret;
}

static esp_err_t find_first_kdbx(char *out_path, size_t out_path_len)
{
    ESP_LOGI(TAG, "Scanning for .kdbx/.kdb under %s", KDBX_DATA_DIR);
    return scan_kdbx_dir(KDBX_DATA_DIR, 0, out_path, out_path_len);
}

static esp_err_t find_password_file(char *out_path, size_t out_path_len)
{
    DIR *dir = opendir(KDBX_DATA_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open %s for password search: errno=%d", KDBX_DATA_DIR, errno);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        bool is_password_config = ascii_equal_ci(entry->d_name, "config.txt") ||
                                  ascii_equal_ci(entry->d_name, "main_config") ||
                                  ascii_equal_ci(entry->d_name, "main_config.txt");
        if (!is_password_config) {
            continue;
        }

        int written = snprintf(out_path, out_path_len, "%s/%s", KDBX_DATA_DIR, entry->d_name);
        if (written < 0 || written >= (int)out_path_len) {
            ESP_LOGW(TAG, "Password path too long: %s", entry->d_name);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            ESP_LOGI(TAG, "Found password file: %s", out_path);
            ret = ESP_OK;
        }
        break;
    }

    closedir(dir);
    return ret;
}

static void trim_ascii(char *text)
{
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' ||
                       text[len - 1] == '\t' ||
                       text[len - 1] == '\r' ||
                       text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
}

static void parse_password_text(char *password)
{
    static const char key[] = "kdbx_password";
    char *value = NULL;
    char *cursor = password;

    while (*cursor) {
        char *line_end = strpbrk(cursor, "\r\n");
        if (line_end) {
            *line_end = '\0';
        }

        trim_ascii(cursor);
        if (strncmp(cursor, key, sizeof(key) - 1) == 0) {
            char *after_key = cursor + sizeof(key) - 1;
            while (*after_key == ' ' || *after_key == '\t') {
                after_key++;
            }

            if (*after_key == '=') {
                value = after_key + 1;
                break;
            }
        }

        if (!line_end) {
            break;
        }
        cursor = line_end + 1;
        while (*cursor == '\r' || *cursor == '\n') {
            cursor++;
        }
    }

    if (value) {
        trim_ascii(value);
        char *first_quote = strchr(value, '"');
        if (first_quote) {
            char *second_quote = strchr(first_quote + 1, '"');
            if (second_quote) {
                *second_quote = '\0';
                memmove(password, first_quote + 1, strlen(first_quote + 1) + 1);
                return;
            }
        }

        memmove(password, value, strlen(value) + 1);
    }

    trim_ascii(password);
}

static esp_err_t read_password(char *password, size_t password_len)
{
    int64_t start_us = kdbx_now_us();
    char password_path[KDBX_PATH_MAX];
    esp_err_t ret = find_password_file(password_path, sizeof(password_path));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Password config file config.txt not found in %s", KDBX_DATA_DIR);
        kdbx_log_elapsed("read password failed before open", start_us);
        return ret;
    }

    FILE *f = fopen(password_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open password file %s: errno=%d", password_path, errno);
        kdbx_log_elapsed("read password failed opening file", start_us);
        return ESP_ERR_NOT_FOUND;
    }

    size_t read_len = fread(password, 1, password_len - 1, f);
    bool read_error = ferror(f);
    fclose(f);

    if (read_error) {
        ESP_LOGW(TAG, "Failed to read password file %s", password_path);
        kdbx_log_elapsed("read password failed reading file", start_us);
        return ESP_FAIL;
    }

    password[read_len] = '\0';
    parse_password_text(password);
    read_len = strlen(password);

    if (read_len == 0) {
        ESP_LOGW(TAG, "Password file is empty: %s", password_path);
        kdbx_log_elapsed("read password empty file", start_us);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "KDBX master password source: %s", password_path);
    ESP_LOGI(TAG, "Loaded password from %s len=%u", password_path, (unsigned int)read_len);
    kdbx_log_elapsed("read password", start_us);
    return ESP_OK;
}

size_t kdbx_get_entry_count(void)
{
    return s_entry_count;
}

const kdbx_entry_t *kdbx_get_entry(size_t index)
{
    if (index >= s_entry_count) {
        return NULL;
    }

    return &s_entries[index];
}

esp_err_t kdbx_load_first_database_from_data(void)
{
    int64_t total_start_us = kdbx_now_us();
    char db_path[KDBX_PATH_MAX];
    char password[KDBX_PASSWORD_MAX];

    int64_t step_start_us = kdbx_now_us();
    esp_err_t ret = find_first_kdbx(db_path, sizeof(db_path));
    kdbx_log_elapsed("find first KDBX", step_start_us);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No .kdbx database found in %s", KDBX_DATA_DIR);
        kdbx_log_elapsed("load KDBX total failed finding database", total_start_us);
        return ret;
    }

    ret = read_password(password, sizeof(password));
    if (ret != ESP_OK) {
        kdbx_log_elapsed("load KDBX total failed reading password", total_start_us);
        return ret;
    }

    step_start_us = kdbx_now_us();
    ret = kdbx_parse_file(db_path, password, s_entries, &s_entry_count, KDBX_MAX_ENTRIES);
    kdbx_log_elapsed("parse/decrypt KDBX file", step_start_us);
    kdbx_log_elapsed(ret == ESP_OK ? "load KDBX total" : "load KDBX total failed parsing file", total_start_us);
    return ret;
}
