#include "firmware_verify.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_image_format.h"
#include "mbedtls/sha256.h"

#define IMAGE_ALIGNMENT 16U
#define IMAGE_CHECKSUM_INITIAL 0xEFU
#define SHA256_SIZE 32U
#define VERIFY_BUFFER_SIZE 1024U

static bool version_is_valid(const char version[sizeof(((esp_app_desc_t *)0)->version)])
{
    return version[0] != '\0' && memchr(version, '\0', sizeof(((esp_app_desc_t *)0)->version)) != NULL;
}

static esp_err_t verify_image_body(FILE *file, const esp_image_header_t *header,
                                   size_t file_size, size_t *digest_offset)
{
    size_t offset = sizeof(*header);
    uint8_t checksum = IMAGE_CHECKSUM_INITIAL;
    uint8_t buffer[VERIFY_BUFFER_SIZE];

    for (uint8_t i = 0; i < header->segment_count; ++i) {
        esp_image_segment_header_t segment;
        if (offset > file_size - sizeof(segment) ||
            fseek(file, (long)offset, SEEK_SET) != 0 ||
            fread(&segment, 1, sizeof(segment), file) != sizeof(segment)) {
            return ESP_ERR_IMAGE_INVALID;
        }
        offset += sizeof(segment);
        if (segment.data_len > file_size - offset) return ESP_ERR_IMAGE_INVALID;

        size_t remaining = segment.data_len;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            if (fread(buffer, 1, chunk, file) != chunk) return ESP_ERR_IMAGE_INVALID;
            for (size_t j = 0; j < chunk; ++j) checksum ^= buffer[j];
            remaining -= chunk;
        }
        offset += segment.data_len;
    }

    if (offset > SIZE_MAX - IMAGE_ALIGNMENT) return ESP_ERR_IMAGE_INVALID;
    size_t image_end = (offset + IMAGE_ALIGNMENT) & ~(IMAGE_ALIGNMENT - 1U);
    if (image_end > file_size || fseek(file, (long)(image_end - 1U), SEEK_SET) != 0) {
        return ESP_ERR_IMAGE_INVALID;
    }
    uint8_t stored_checksum;
    if (fread(&stored_checksum, 1, 1, file) != 1 || stored_checksum != checksum) {
        return ESP_ERR_IMAGE_INVALID;
    }
    if (!header->hash_appended || image_end > file_size - SHA256_SIZE) {
        return ESP_ERR_IMAGE_INVALID;
    }
    *digest_offset = image_end;
    return ESP_OK;
}

static esp_err_t verify_sha256(FILE *file, size_t digest_offset,
                               uint8_t output[FIRMWARE_SHA256_SIZE])
{
    uint8_t expected[SHA256_SIZE];
    uint8_t calculated[SHA256_SIZE];
    uint8_t buffer[VERIFY_BUFFER_SIZE];
    mbedtls_sha256_context context;
    int result;

    if (fseek(file, (long)digest_offset, SEEK_SET) != 0 ||
        fread(expected, 1, sizeof(expected), file) != sizeof(expected) ||
        fseek(file, 0, SEEK_SET) != 0) {
        return ESP_ERR_IMAGE_INVALID;
    }

    mbedtls_sha256_init(&context);
    result = mbedtls_sha256_starts(&context, 0);
    size_t remaining = digest_offset;
    while (result == 0 && remaining > 0) {
        size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (fread(buffer, 1, chunk, file) != chunk) {
            result = -1;
            break;
        }
        result = mbedtls_sha256_update(&context, buffer, chunk);
        remaining -= chunk;
    }
    if (result == 0) result = mbedtls_sha256_finish(&context, calculated);
    mbedtls_sha256_free(&context);

    if (result != 0 || memcmp(calculated, expected, sizeof(expected)) != 0) {
        return ESP_ERR_IMAGE_INVALID;
    }
    memcpy(output, calculated, sizeof(calculated));
    return ESP_OK;
}

esp_err_t firmware_verify_file(FILE *file, size_t partition_size, firmware_image_info_t *info)
{
    if (!file || !info) return ESP_ERR_INVALID_ARG;
    if (fseek(file, 0, SEEK_END) != 0) return ESP_FAIL;
    long file_size_long = ftell(file);
    if (file_size_long <= 0 || (size_t)file_size_long > partition_size) return ESP_ERR_INVALID_SIZE;
    size_t file_size = (size_t)file_size_long;
    if (fseek(file, 0, SEEK_SET) != 0) return ESP_FAIL;

    esp_image_header_t image_header;
    esp_image_segment_header_t segment_header;
    if (fread(&image_header, 1, sizeof(image_header), file) != sizeof(image_header) ||
        fread(&segment_header, 1, sizeof(segment_header), file) != sizeof(segment_header) ||
        fread(&info->app_desc, 1, sizeof(info->app_desc), file) != sizeof(info->app_desc)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC || image_header.segment_count == 0 ||
        image_header.segment_count > ESP_IMAGE_MAX_SEGMENTS) return ESP_ERR_IMAGE_INVALID;
    if (image_header.chip_id != ESP_CHIP_ID_ESP32P4) return ESP_ERR_NOT_SUPPORTED;
    if (segment_header.data_len < sizeof(info->app_desc) ||
        info->app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !version_is_valid(info->app_desc.version)) return ESP_ERR_INVALID_VERSION;

    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(info->app_desc.project_name, running->project_name, sizeof(info->app_desc.project_name)) != 0) {
        return ESP_ERR_INVALID_VERSION;
    }

    size_t digest_offset;
    esp_err_t ret = verify_image_body(file, &image_header, file_size, &digest_offset);
    if (ret != ESP_OK) return ret;
    ret = verify_sha256(file, digest_offset, info->sha256);
    if (ret != ESP_OK) return ret;

    info->image_size = file_size;
    if (fseek(file, 0, SEEK_SET) != 0) return ESP_FAIL;

    if (strncmp(info->app_desc.version, running->version,
                sizeof(info->app_desc.version)) == 0) {
        return FIRMWARE_VERIFY_ERR_SAME_VERSION;
    }
    return ESP_OK;
}
