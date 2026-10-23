#pragma once
#define ESP_LOG_INFO 3

static inline void esp_log_stub(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

static inline void esp_log_buffer_hexdump_stub(const char *tag, const void *buffer,
                                                unsigned length, int level)
{
    (void)tag;
    (void)buffer;
    (void)length;
    (void)level;
}

#define ESP_LOGI(...) esp_log_stub(__VA_ARGS__)
#define ESP_LOGW(...) esp_log_stub(__VA_ARGS__)
#define ESP_LOG_BUFFER_HEXDUMP(...) esp_log_buffer_hexdump_stub(__VA_ARGS__)
