#include "kdbx_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kdbx_internal.h"
#include "ui/ui_password_manager.h"

#define KDBX_DATA_DIR       "/data"
#define KDBX_DATABASE_DIR   KDBX_DATA_DIR "/KDBX"
#define KDBX_PATH_MAX       256
#define KDBX_PASSWORD_MAX   128
#define KDBX_SCAN_MAX_DEPTH 4

static const char *TAG = "kdbx";
static kdbx_entry_t *s_entries;
static atomic_size_t s_entry_count;
static kdbx_group_t s_groups[KDBX_MAX_GROUPS];
static atomic_size_t s_group_count;
static StaticSemaphore_t s_entries_mutex_storage;
static SemaphoreHandle_t s_entries_mutex;
static portMUX_TYPE s_entries_mutex_guard = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    KDBX_SCAN_NORMAL,
    KDBX_SCAN_EMERGENCY,
    KDBX_SCAN_DURESS,
} kdbx_scan_mode_t;

static SemaphoreHandle_t entries_mutex(void)
{
    taskENTER_CRITICAL(&s_entries_mutex_guard);
    if (!s_entries_mutex) {
        s_entries_mutex = xSemaphoreCreateMutexStatic(&s_entries_mutex_storage);
    }
    taskEXIT_CRITICAL(&s_entries_mutex_guard);
    return s_entries_mutex;
}

static esp_err_t ensure_entry_storage(void)
{
    if (s_entries) {
        return ESP_OK;
    }

    s_entries = heap_caps_malloc(KDBX_MAX_ENTRIES * sizeof(*s_entries),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) {
        ESP_LOGW(TAG, "Failed to allocate %u KDBX entries in PSRAM (%u bytes)",
                 (unsigned)KDBX_MAX_ENTRIES,
                 (unsigned)(KDBX_MAX_ENTRIES * sizeof(*s_entries)));
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void clear_published_entries(void)
{
    size_t entry_count = atomic_exchange_explicit(&s_entry_count, 0,
                                                  memory_order_acq_rel);
    if (s_entries && entry_count > 0) {
        memset(s_entries, 0, entry_count * sizeof(*s_entries));
    }
}

static esp_err_t ensure_database_directory(void)
{
    struct stat st;
    if (stat(KDBX_DATABASE_DIR, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (errno != ENOENT) {
        ESP_LOGW(TAG, "Failed to stat KDBX directory %s: errno=%d",
                 KDBX_DATABASE_DIR, errno);
        return ESP_FAIL;
    }
    if (mkdir(KDBX_DATABASE_DIR, 0775) == 0 || errno == EEXIST) {
        ESP_LOGI(TAG, "Created KDBX directory: %s", KDBX_DATABASE_DIR);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Failed to create KDBX directory %s: errno=%d",
             KDBX_DATABASE_DIR, errno);
    return ESP_FAIL;
}

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

static bool looks_like_special_kdbx_name(const char *name, const char *prefix)
{
    char long_name[16];
    snprintf(long_name, sizeof(long_name), "%s.kdbx", prefix);
    if (ascii_equal_ci(name, long_name)) {
        return true;
    }
    snprintf(long_name, sizeof(long_name), "%s.kdb", prefix);
    if (ascii_equal_ci(name, long_name)) {
        return true;
    }

    return strlen(name) >= 6 &&
           ascii_tolower(name[0]) == ascii_tolower(prefix[0]) &&
           ascii_tolower(name[1]) == ascii_tolower(prefix[1]) &&
           ascii_tolower(name[2]) == ascii_tolower(prefix[2]) &&
           name[3] == '~' &&
           ends_with_ci(name, ".kdb");
}

static esp_err_t scan_kdbx_dir(const char *dir_path,
                               int depth,
                               kdbx_scan_mode_t scan_mode,
                               char *out_path,
                               size_t out_path_len)
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

        /* Runtime logs cannot contain a database. Avoid traversing log files,
         * including legacy FAT entries whose names are not valid UTF-8. */
        if (depth == 0 && ascii_equal_ci(entry->d_name, "log")) {
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

        bool is_emergency = looks_like_special_kdbx_name(entry->d_name, "emg");
        bool is_duress = looks_like_special_kdbx_name(entry->d_name, "drs");
        bool is_requested_database = false;
        if (looks_like_kdbx_name(entry->d_name)) {
            if (scan_mode == KDBX_SCAN_EMERGENCY) {
                is_requested_database = is_emergency;
            } else if (scan_mode == KDBX_SCAN_DURESS) {
                is_requested_database = is_duress;
            } else {
                is_requested_database = !is_emergency && !is_duress;
            }
        }
        if (!S_ISDIR(st.st_mode) && is_requested_database) {
            strlcpy(out_path, child_path, out_path_len);
            KDBX_LOGD(TAG, "Found KDBX database: %s size=%ld", out_path, (long)st.st_size);
            ret = ESP_OK;
            break;
        }

        if (S_ISDIR(st.st_mode) && depth < KDBX_SCAN_MAX_DEPTH) {
            ret = scan_kdbx_dir(child_path, depth + 1, scan_mode,
                                out_path, out_path_len);
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
    KDBX_LOGD(TAG, "Scanning for normal .kdbx/.kdb under %s", KDBX_DATA_DIR);
    return scan_kdbx_dir(KDBX_DATA_DIR, 0, KDBX_SCAN_NORMAL, out_path, out_path_len);
}

static esp_err_t find_special_kdbx(kdbx_scan_mode_t scan_mode,
                                   const char *name,
                                   char *out_path,
                                   size_t out_path_len)
{
    KDBX_LOGD(TAG, "Scanning for %s database under %s", name, KDBX_DATA_DIR);
    return scan_kdbx_dir(KDBX_DATA_DIR, 0, scan_mode, out_path, out_path_len);
}

static esp_err_t find_password_file(char *out_path, size_t out_path_len)
{
    DIR *dir = opendir(KDBX_DATABASE_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open %s for password search: errno=%d",
                 KDBX_DATABASE_DIR, errno);
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

        int written = snprintf(out_path, out_path_len, "%s/%s",
                               KDBX_DATABASE_DIR, entry->d_name);
        if (written < 0 || written >= (int)out_path_len) {
            ESP_LOGW(TAG, "Password path too long: %s", entry->d_name);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            KDBX_LOGD(TAG, "Found password file: %s", out_path);
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

static bool parse_password_text(char *password, const char *key)
{
    char *value = NULL;
    char *cursor = password;
    size_t key_length = strlen(key);

    while (*cursor) {
        char *line_end = strpbrk(cursor, "\r\n");
        if (line_end) {
            *line_end = '\0';
        }

        trim_ascii(cursor);
        if (strncmp(cursor, key, key_length) == 0) {
            char *after_key = cursor + key_length;
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
                return true;
            }
        }

        memmove(password, value, strlen(value) + 1);
        trim_ascii(password);
        return true;
    }

    trim_ascii(password);
    return false;
}

static esp_err_t read_password(const char *key,
                               bool require_key,
                               char *password,
                               size_t password_len)
{
    int64_t start_us = kdbx_now_us();
    char password_path[KDBX_PATH_MAX];
    esp_err_t ret = find_password_file(password_path, sizeof(password_path));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Password config file config.txt not found in %s",
                 KDBX_DATABASE_DIR);
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
    bool key_found = parse_password_text(password, key);
    if (require_key && !key_found) {
        ESP_LOGW(TAG, "Password key %s not found in %s", key, password_path);
        return ESP_ERR_NOT_FOUND;
    }
    read_len = strlen(password);

    if (read_len == 0) {
        ESP_LOGW(TAG, "Password file is empty: %s", password_path);
        kdbx_log_elapsed("read password empty file", start_us);
        return ESP_ERR_INVALID_ARG;
    }

    KDBX_LOGD(TAG, "KDBX password key %s loaded from %s len=%u",
             key, password_path, (unsigned int)read_len);
    kdbx_log_elapsed("read password", start_us);
    return ESP_OK;
}

size_t kdbx_get_entry_count(void)
{
    return atomic_load_explicit(&s_entry_count, memory_order_acquire);
}

const kdbx_entry_t *kdbx_get_entry(size_t index)
{
    if (!s_entries || index >= kdbx_get_entry_count()) {
        return NULL;
    }

    return &s_entries[index];
}

esp_err_t kdbx_create_entry_snapshot(kdbx_entry_t **entries, size_t *count)
{
    if (!entries || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *entries = NULL;
    *count = 0;

    SemaphoreHandle_t mutex = entries_mutex();
    if (!mutex) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    size_t snapshot_count = kdbx_get_entry_count();
    kdbx_entry_t *snapshot = snapshot_count
                                 ? malloc(snapshot_count * sizeof(*snapshot))
                                 : NULL;
    if (snapshot_count && !snapshot) {
        xSemaphoreGive(mutex);
        return ESP_ERR_NO_MEM;
    }
    if (snapshot_count) {
        memcpy(snapshot, s_entries, snapshot_count * sizeof(*snapshot));
    }
    xSemaphoreGive(mutex);

    *entries = snapshot;
    *count = snapshot_count;
    return ESP_OK;
}

void kdbx_free_entry_snapshot(kdbx_entry_t *entries, size_t count)
{
    if (!entries) {
        return;
    }
    memset(entries, 0, count * sizeof(*entries));
    free(entries);
}

size_t kdbx_get_group_count(void)
{
    return atomic_load_explicit(&s_group_count, memory_order_acquire);
}

const kdbx_group_t *kdbx_get_group(size_t index)
{
    return index < kdbx_get_group_count() ? &s_groups[index] : NULL;
}

void kdbx_reset_groups(void)
{
    atomic_store_explicit(&s_group_count, 0, memory_order_release);
}

size_t kdbx_add_group(const char *name, uint8_t depth, size_t parent_index)
{
    size_t group_count = atomic_load_explicit(&s_group_count, memory_order_relaxed);
    if (!name || group_count >= KDBX_MAX_GROUPS) {
        return SIZE_MAX;
    }

    size_t index = group_count;
    memset(&s_groups[index], 0, sizeof(s_groups[index]));
    strlcpy(s_groups[index].name, name, sizeof(s_groups[index].name));
    s_groups[index].depth = depth;
    s_groups[index].parent_index = parent_index;
    atomic_store_explicit(&s_group_count, group_count + 1, memory_order_release);
    ui_password_manager_notify_load_progress();
    return index;
}

bool kdbx_try_increment_group_entry_count(size_t index)
{
    if (index == SIZE_MAX) {
        return true;
    }
    if (index >= kdbx_get_group_count() ||
        s_groups[index].entry_count >= KDBX_MAX_ENTRIES_PER_GROUP) {
        return false;
    }
    s_groups[index].entry_count++;
    return true;
}

void kdbx_publish_entry_count(size_t count)
{
    atomic_store_explicit(&s_entry_count, count, memory_order_release);
    ui_password_manager_notify_load_progress();
}

esp_err_t kdbx_load_first_database_from_data(void)
{
    int64_t total_start_us = kdbx_now_us();
    char db_path[KDBX_PATH_MAX];
    char password[KDBX_PASSWORD_MAX];
    SemaphoreHandle_t mutex = entries_mutex();
    if (!mutex) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t ret = ensure_entry_storage();
    if (ret != ESP_OK) {
        xSemaphoreGive(mutex);
        return ret;
    }
    kdbx_reset_groups();
    clear_published_entries();
    xSemaphoreGive(mutex);

    ret = ensure_database_directory();
    if (ret != ESP_OK) {
        kdbx_log_elapsed("load KDBX total failed preparing database directory", total_start_us);
        return ret;
    }

    int64_t step_start_us = kdbx_now_us();
    ret = find_first_kdbx(db_path, sizeof(db_path));
    kdbx_log_elapsed("find first KDBX", step_start_us);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No .kdbx database found in %s", KDBX_DATA_DIR);
        kdbx_log_elapsed("load KDBX total failed finding database", total_start_us);
        return ret == ESP_ERR_NOT_FOUND ? KDBX_ERR_DATABASE_NOT_FOUND : ret;
    }

    ret = read_password("kdbx_password", false, password, sizeof(password));
    if (ret != ESP_OK) {
        kdbx_log_elapsed("load KDBX total failed reading password", total_start_us);
        return ret;
    }

    step_start_us = kdbx_now_us();
    size_t parsed_entry_count = 0;
    ret = kdbx_parse_file(db_path, password, s_entries,
                          &parsed_entry_count, KDBX_MAX_ENTRIES);
    if (ret != ESP_OK) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        clear_published_entries();
        xSemaphoreGive(mutex);
        kdbx_reset_groups();
    } else {
        atomic_store_explicit(&s_entry_count, parsed_entry_count, memory_order_release);
    }
    kdbx_log_elapsed("parse/decrypt KDBX file", step_start_us);
    kdbx_log_elapsed(ret == ESP_OK ? "load KDBX total" : "load KDBX total failed parsing file", total_start_us);
    return ret;
}

static esp_err_t load_special_database(kdbx_scan_mode_t scan_mode,
                                       const char *database_name,
                                       const char *password_key)
{
    int64_t total_start_us = kdbx_now_us();
    char db_path[KDBX_PATH_MAX];
    char password[KDBX_PASSWORD_MAX];

    SemaphoreHandle_t mutex = entries_mutex();
    if (!mutex) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t ret = ensure_entry_storage();
    if (ret != ESP_OK) {
        xSemaphoreGive(mutex);
        return ret;
    }
    kdbx_reset_groups();
    clear_published_entries();
    xSemaphoreGive(mutex);

    ret = ensure_database_directory();
    if (ret != ESP_OK) {
        kdbx_log_elapsed("load special KDBX failed preparing database directory", total_start_us);
        return ret;
    }

    ret = find_special_kdbx(scan_mode, database_name, db_path, sizeof(db_path));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s database not found under %s", database_name, KDBX_DATA_DIR);
        return ret == ESP_ERR_NOT_FOUND ? KDBX_ERR_DATABASE_NOT_FOUND : ret;
    }

    ret = read_password(password_key, true, password, sizeof(password));
    if (ret != ESP_OK) {
        kdbx_log_elapsed("load special KDBX failed reading password", total_start_us);
        return ret;
    }

    size_t parsed_entry_count = 0;
    ret = kdbx_parse_file(db_path, password, s_entries,
                          &parsed_entry_count, KDBX_MAX_ENTRIES);
    if (ret != ESP_OK) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        clear_published_entries();
        xSemaphoreGive(mutex);
        kdbx_reset_groups();
    } else {
        atomic_store_explicit(&s_entry_count, parsed_entry_count, memory_order_release);
    }
    kdbx_log_elapsed(ret == ESP_OK ? "load special KDBX total" :
                                      "load special KDBX failed parsing file",
                     total_start_us);
    return ret;
}

esp_err_t kdbx_load_emergency_database_from_data(void)
{
    return load_special_database(KDBX_SCAN_EMERGENCY, "emg.kdbx", "emg_kdbx");
}

esp_err_t kdbx_load_duress_database_from_data(void)
{
    return load_special_database(KDBX_SCAN_DURESS, "drs.kdbx", "drs_kdbx");
}
