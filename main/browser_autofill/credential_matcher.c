#include "browser_autofill/credential_matcher.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "kdbx/kdbx_loader.h"
#include "ui/ui_password_manager.h"

#define HOST_MAX 128

static bool extract_host(const char *url, char *host, size_t host_size)
{
    if (!url || !url[0] || !host || host_size == 0) {
        return false;
    }

    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *end = start + strcspn(start, "/?#");
    const char *at = NULL;
    for (const char *cursor = start; cursor < end; cursor++) {
        if (*cursor == '@') {
            at = cursor;
        }
    }
    if (at) {
        start = at + 1;
    }

    if (start < end && *start == '[') {
        start++;
        const char *bracket = memchr(start, ']', (size_t)(end - start));
        if (!bracket) {
            return false;
        }
        end = bracket;
    } else {
        const char *colon = memchr(start, ':', (size_t)(end - start));
        if (colon) {
            end = colon;
        }
    }

    size_t length = (size_t)(end - start);
    while (length > 0 && start[length - 1] == '.') {
        length--;
    }
    if (length == 0 || length >= host_size) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        host[i] = (char)tolower((unsigned char)start[i]);
    }
    host[length] = '\0';
    return true;
}

static bool host_matches(const char *requested, const char *stored)
{
    return strcmp(requested, stored) == 0;
}

esp_err_t credential_matcher_find(const char *url,
                                  browser_credential_t *results,
                                  size_t capacity,
                                  size_t *count)
{
    if (!url || !results || capacity == 0 || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (ui_password_manager_is_loading()) {
        return ESP_ERR_INVALID_STATE;
    }

    char requested_host[HOST_MAX];
    if (!extract_host(url, requested_host, sizeof(requested_host))) {
        return ESP_ERR_INVALID_ARG;
    }

    kdbx_entry_t *entries = NULL;
    size_t entry_count = 0;
    esp_err_t ret = kdbx_create_entry_snapshot(&entries, &entry_count);
    if (ret != ESP_OK) {
        return ret;
    }
    for (size_t i = 0; i < entry_count && *count < capacity; i++) {
        const kdbx_entry_t *entry = &entries[i];
        if (!entry->url[0] || !entry->password[0] ||
            entry->password_protected) {
            continue;
        }

        char stored_host[HOST_MAX];
        if (!extract_host(entry->url, stored_host, sizeof(stored_host)) ||
            !host_matches(requested_host, stored_host)) {
            continue;
        }

        browser_credential_t *result = &results[(*count)++];
        memset(result, 0, sizeof(*result));
        snprintf(result->uuid, sizeof(result->uuid), "esp32p4-%08x",
                 (unsigned int)i);
        strlcpy(result->title, entry->title, sizeof(result->title));
        strlcpy(result->username, entry->username, sizeof(result->username));
        strlcpy(result->password, entry->password, sizeof(result->password));
    }
    kdbx_free_entry_snapshot(entries, entry_count);
    return ESP_OK;
}
