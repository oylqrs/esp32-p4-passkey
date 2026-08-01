#include "applets.h"
#include "apdu.h"
#include "canokey_esp32p4.h"
#include "device.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t s_canokey_task_handle;
static bool s_canokey_ready;
static bool s_ccid_session_active;

static const char *TAG = "canokey_port";

#define CTAP_CERT_FILE_NAME   "ctap_cert"
#define CTAP_PRIVATE_KEY_ATTR 0x00

int canokey_esp32p4_process_ccid_apdu(const uint8_t *command, uint16_t command_len,
                                      uint8_t *response, uint16_t response_size,
                                      uint16_t *response_len, uint16_t *status_word);

static void ccid_session_keepalive_poll(void)
{
    /* CanoKey core expires applet sessions after 2 seconds. Refresh from the
     * CanoKey task itself, not a second task, so device session state is touched
     * from the same context as device_loop(). */
    if (s_ccid_session_active) {
        device_applet_session_touch(DEVICE_APPLET_SESSION_CCID);
    }
}

static esp_err_t ensure_test_attestation_installed(void)
{
    static const uint8_t fido_private_key[32] = {
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    };
    static const uint8_t cert[] = {
        0x30, 0x03, 0x02, 0x01, 0x01,
    };

    uint8_t key[sizeof(fido_private_key)] = {0};
    int key_len = read_attr(CTAP_CERT_FILE_NAME,
                            CTAP_PRIVATE_KEY_ATTR,
                            key,
                            sizeof(key));
    int cert_len = get_file_size(CTAP_CERT_FILE_NAME);

    if (key_len == (int)sizeof(fido_private_key) && cert_len > 0) {
        ESP_LOGI(TAG, "CTAP attestation already installed: key_len=%d cert_len=%d", key_len, cert_len);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "CTAP attestation missing/incomplete: key_len=%d cert_len=%d; installing test attestation",
             key_len,
             cert_len);

    int ret = write_file(CTAP_CERT_FILE_NAME, cert, 0, sizeof(cert), 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "write test attestation certificate failed: %d", ret);
        return ESP_FAIL;
    }

    ret = write_attr(CTAP_CERT_FILE_NAME,
                     CTAP_PRIVATE_KEY_ATTR,
                     fido_private_key,
                     sizeof(fido_private_key));
    if (ret != 0) {
        ESP_LOGE(TAG, "write test attestation private key failed: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "CTAP test attestation installed");
    return ESP_OK;
}

int device_spinlock_lock(volatile uint32_t *lock, uint32_t blocking)
{
    do {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }

        if (!blocking) {
            return -1;
        }

        vTaskDelay(1);
    } while (true);
}

void device_spinlock_unlock(volatile uint32_t *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

int device_atomic_compare_and_swap(volatile uint32_t *var, uint32_t expect, uint32_t update)
{
    return __atomic_compare_exchange_n(var, &expect, update, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) ? 0 : -1;
}

static void canokey_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "CanoKey task starting");
    s_canokey_ready = false;
    ESP_LOGI(TAG, "CanoKey LittleFS mount begin");
    if (canokey_esp32p4_flash_init() != ESP_OK) {
        ESP_LOGE(TAG, "CanoKey task stopped: LittleFS unavailable");
        s_canokey_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CanoKey LittleFS mount done");
    ESP_LOGI(TAG, "CanoKey crypto port init begin");
    if (canokey_esp32p4_crypto_init() != ESP_OK) {
        ESP_LOGE(TAG, "CanoKey task stopped: crypto port unavailable");
        s_canokey_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (canokey_esp32p4_crypto_selftest() != ESP_OK) {
        ESP_LOGE(TAG, "crypto selftest failed");
        s_canokey_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "CanoKey crypto port init done");
    ESP_LOGI(TAG, "CanoKey APDU buffer init begin");
    init_apdu_buffer();
    ESP_LOGI(TAG, "CanoKey APDU buffer init done");
    ESP_LOGI(TAG, "CanoKey device_init begin");
    device_init();
    ESP_LOGI(TAG, "CanoKey device_init done");
    ESP_LOGI(TAG, "CanoKey applets_install begin");
    if (applets_install() != 0) {
        ESP_LOGE(TAG, "CanoKey task stopped: applets_install failed");
        s_canokey_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "CanoKey applets_install done");

    if (ensure_test_attestation_installed() != ESP_OK) {
        ESP_LOGE(TAG, "CanoKey task stopped: CTAP attestation unavailable");
        s_canokey_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_canokey_ready = true;
    ESP_LOGI(TAG, "CanoKey task ready; entering device loop");
    while (true) {
        ESP_LOGD(TAG, "CanoKey device_loop begin");
        /* Keep the host-owned CCID session alive before device_loop() checks
         * the 2-second CanoKey applet timeout. */
        ccid_session_keepalive_poll();
        device_loop();
        ESP_LOGD(TAG, "CanoKey device_loop done");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t canokey_esp32p4_start(void)
{
    if (s_canokey_task_handle) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(canokey_task,
                                "canokey",
                                12288,
                                NULL,
                                5,
                                &s_canokey_task_handle);
    if (ok != pdPASS) {
        s_canokey_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool canokey_esp32p4_is_running(void)
{
    return s_canokey_task_handle != NULL;
}

bool canokey_esp32p4_is_ready(void)
{
    return s_canokey_ready;
}

esp_err_t canokey_esp32p4_openpgp_probe(void)
{
    static const uint8_t select_openpgp[] = {
        0x00, 0xA4, 0x04, 0x00, 0x06, 0xD2, 0x76, 0x00, 0x01, 0x24, 0x01,
    };
    uint8_t response[256] = {0};
    uint16_t response_len = 0;
    uint16_t status_word = 0;

    int ret = canokey_esp32p4_process_ccid_apdu(select_openpgp,
                                                sizeof(select_openpgp),
                                                response,
                                                sizeof(response),
                                                &response_len,
                                                &status_word);
    ESP_LOGI(TAG, "OpenPGP probe SELECT ret=%d response_len=%u sw=%04x",
             ret,
             response_len,
             status_word);
    init_apdu_buffer();
    /* The boot probe is only a self-test, not a host-owned CCID session. Drop
     * it here so the real session starts cleanly at IccPowerOn. */
    device_applet_session_release(DEVICE_APPLET_SESSION_CCID);
    s_ccid_session_active = false;
    return ret == 0 && status_word == SW_NO_ERROR ? ESP_OK : ESP_FAIL;
}

esp_err_t canokey_esp32p4_ccid_power_on(void)
{
    if (!canokey_esp32p4_is_ready()) {
        ESP_LOGW(TAG, "CCID power-on rejected: CanoKey task is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ccid_session_active) {
        device_applet_session_touch(DEVICE_APPLET_SESSION_CCID);
        return ESP_OK;
    }

    /* CCID PowerOn is the card-session boundary. Reset stale volatile applet
     * state first, then hold the CCID session across following XfrBlock APDUs. */
    if (device_applet_session_reset(DEVICE_APPLET_SESSION_CCID) != 0) {
        ESP_LOGW(TAG, "CCID power-on failed: cannot reset applet session");
        return ESP_ERR_INVALID_STATE;
    }
    if (device_applet_session_acquire(DEVICE_APPLET_SESSION_CCID) != 0) {
        ESP_LOGW(TAG, "CCID power-on failed: cannot acquire applet session");
        return ESP_ERR_INVALID_STATE;
    }

    /* Mark active before the next idle gap; canokey_task will extend the
     * CanoKey 2-second applet timeout until host power-off/reset. */
    s_ccid_session_active = true;
    device_applet_session_touch(DEVICE_APPLET_SESSION_CCID);
    ESP_LOGI(TAG, "CCID applet session acquired");
    return ESP_OK;
}

void canokey_esp32p4_ccid_power_off(void)
{
    /* CCID PowerOff/USB reset is the only place we intentionally drop volatile
     * OpenPGP state such as PW1/PW3 validation and import chaining. */
    device_applet_session_release(DEVICE_APPLET_SESSION_CCID);
    s_ccid_session_active = false;
    ESP_LOGI(TAG, "CCID applet session released");
}

int canokey_esp32p4_process_ccid_apdu(const uint8_t *command, uint16_t command_len,
                                      uint8_t *response, uint16_t response_size,
                                      uint16_t *response_len, uint16_t *status_word)
{
    if (!command || !response || !response_len || !status_word || !canokey_esp32p4_is_running()) {
        ESP_LOGW(TAG, "CCID APDU rejected: command=%p response=%p response_len=%p status_word=%p running=%u ready=%u",
                 command,
                 response,
                 response_len,
                 status_word,
                 canokey_esp32p4_is_running(),
                 canokey_esp32p4_is_ready());
        return -1;
    }
    uint8_t command_data[APDU_INCOMING_DATA_SIZE] = {0};
    CAPDU capdu = {.data = command_data};
    RAPDU rapdu = {.data = response};
    if (build_capdu(&capdu, command, command_len) < 0) {
        ESP_LOGW(TAG, "CCID APDU parse failed: len=%u", command_len);
        return -2;
    }
    /* Each APDU only needs exclusive access to the shared APDU buffer. The
     * applet session itself is held from IccPowerOn to IccPowerOff so OpenPGP
     * keeps PW3/PW1 validation between commands. */
    if (!s_ccid_session_active && canokey_esp32p4_ccid_power_on() != ESP_OK) {
        return -4;
    }
    if (device_applet_session_acquire(DEVICE_APPLET_SESSION_CCID) != 0) {
        ESP_LOGW(TAG, "CCID APDU rejected: applet session busy");
        return -4;
    }
    if (acquire_apdu_buffer(BUFFER_OWNER_CCID) != 0) {
        ESP_LOGW(TAG, "CCID APDU rejected: APDU buffer busy");
        return -4;
    }
    ESP_LOGI(TAG,
             "CCID APDU dispatch: cla=%02x ins=%02x p1=%02x p2=%02x lc=%u le=%u",
             capdu.cla,
             capdu.ins,
             capdu.p1,
             capdu.p2,
             capdu.lc,
             capdu.le);
    process_apdu_from(&capdu, &rapdu, APDU_TRANSPORT_CCID);
    /* Release only the buffer here. Releasing the applet session would call
     * openpgp_poweroff() and immediately clear a successful VERIFY PW3. */
    release_apdu_buffer(BUFFER_OWNER_CCID);
    device_applet_session_touch(DEVICE_APPLET_SESSION_CCID);
    if (rapdu.len > response_size) {
        ESP_LOGW(TAG, "CCID APDU response too large: len=%u cap=%u", rapdu.len, response_size);
        return -3;
    }
    *response_len = rapdu.len;
    *status_word = rapdu.sw;
    ESP_LOGI(TAG, "CCID APDU done: response_len=%u sw=%04x", rapdu.len, rapdu.sw);
    return 0;
}
