#pragma once

#include <stdint.h>
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_err.h"

#define FIRMWARE_SHA256_SIZE 32
#define FIRMWARE_VERIFY_ERR_SAME_VERSION ((esp_err_t)0x7001)

typedef struct {
    size_t image_size;
    esp_app_desc_t app_desc;
    uint8_t sha256[FIRMWARE_SHA256_SIZE];
} firmware_image_info_t;

esp_err_t firmware_verify_file(FILE *file, size_t partition_size, firmware_image_info_t *info);
