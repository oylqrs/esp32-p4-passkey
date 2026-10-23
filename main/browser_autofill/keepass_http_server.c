#include "browser_autofill/keepass_http_server.h"

#include <stdlib.h>

#include "browser_autofill/keepass_http_protocol.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

#define KEEPASS_HTTP_PORT 19455
#define KEEPASS_HTTP_CONTROL_PORT 19456
#define KEEPASS_HTTP_MAX_REQUEST 4096

static const char *TAG = "keepass_server";
static httpd_handle_t s_server;

static esp_err_t post_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 ||
        request->content_len > KEEPASS_HTTP_MAX_REQUEST) {
        httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE,
                            "Request too large");
        return ESP_FAIL;
    }

    char *body = malloc((size_t)request->content_len + 1);
    if (!body) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int chunk = httpd_req_recv(request, body + received,
                                   (size_t)request->content_len - received);
        if (chunk <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    char *response = NULL;
    esp_err_t ret = keepass_http_protocol_handle(body, &response);
    mbedtls_platform_zeroize(body, received);
    free(body);
    if (ret != ESP_OK || !response) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Protocol failure");
        free(response);
        return ret;
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    ret = httpd_resp_sendstr(request, response);
    free(response);
    return ret;
}

esp_err_t keepass_http_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = KEEPASS_HTTP_PORT;
    config.ctrl_port = KEEPASS_HTTP_CONTROL_PORT;
    config.stack_size = 8192;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TCP/%d: %s",
                 KEEPASS_HTTP_PORT, esp_err_to_name(ret));
        s_server = NULL;
        return ret;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_POST,
        .handler = post_handler,
    };
    ret = httpd_register_uri_handler(s_server, &root);
    if (ret != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "KeePassHTTP listening on TCP/%d", KEEPASS_HTTP_PORT);
    return ESP_OK;
}

void keepass_http_server_stop(void)
{
    if (!s_server) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "KeePassHTTP stopped");
}
