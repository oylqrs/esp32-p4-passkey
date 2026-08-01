#include <stdint.h>

#include "esp_log.h"
#include "piv.h"

static const char *TAG = "piv_dispatch";

int __real_piv_process_apdu(const CAPDU *capdu, RAPDU *rapdu);
int __real_piv_process_apdu_message(RAPDU_CHAINING *rapdu_chaining, CAPDU *capdu, RAPDU *rapdu);

static const char *piv_dispatch_ins_name(uint8_t ins)
{
    switch (ins) {
    case PIV_INS_VERIFY:
        return "VERIFY";
    case PIV_INS_CHANGE_REFERENCE_DATA:
        return "CHANGE_REFERENCE_DATA";
    case PIV_INS_RESET_RETRY_COUNTER:
        return "RESET_RETRY_COUNTER";
    case PIV_INS_GENERATE_ASYMMETRIC_KEY_PAIR:
        return "GENERATE_ASYMMETRIC_KEY_PAIR";
    case PIV_INS_GENERAL_AUTHENTICATE:
        return "GENERAL_AUTHENTICATE";
    case PIV_INS_SELECT:
        return "SELECT";
    case PIV_INS_GET_DATA_RESPONSE:
        return "GET_DATA_RESPONSE";
    case PIV_INS_GET_DATA:
        return "GET_DATA";
    case PIV_INS_PUT_DATA:
        return "PUT_DATA";
    case PIV_INS_GET_METADATA:
        return "GET_METADATA";
    case PIV_INS_GET_SERIAL:
        return "GET_SERIAL";
    case PIV_INS_MOVE_DELETE_KEY:
        return "MOVE_DELETE_KEY";
    case PIV_INS_SET_PIN_RETRIES:
        return "SET_PIN_RETRIES";
    case PIV_INS_RESET:
        return "RESET";
    case PIV_INS_GET_VERSION:
        return "GET_VERSION";
    case PIV_INS_IMPORT_ASYMMETRIC_KEY:
        return "IMPORT_ASYMMETRIC_KEY";
    case PIV_INS_SET_MANAGEMENT_KEY:
        return "SET_MANAGEMENT_KEY";
    case PIV_INS_ALGORITHM_EXTENSION:
        return "ALGORITHM_EXTENSION";
    default:
        return "UNKNOWN";
    }
}

static void piv_dispatch_log_capdu(const CAPDU *capdu)
{
    if (!capdu) {
        ESP_LOGD(TAG, "APDU capdu=(null)");
        return;
    }

    ESP_LOGD(TAG,
             "APDU cla=0x%02x ins=0x%02x(%s) p1=0x%02x p2=0x%02x lc=%u le=%lu extended=%u",
             capdu->cla,
             capdu->ins,
             piv_dispatch_ins_name(capdu->ins),
             capdu->p1,
             capdu->p2,
             (unsigned)capdu->lc,
             (unsigned long)capdu->le,
             (unsigned)capdu->extended);
}

static void piv_dispatch_log_result(const RAPDU *rapdu, int ret)
{
    if (!rapdu) {
        ESP_LOGD(TAG, "APDU result rapdu=(null) ret=%d", ret);
        return;
    }

    ESP_LOGD(TAG, "APDU result ret=%d sw=0x%04x len=%u", ret, (unsigned)rapdu->sw, (unsigned)rapdu->len);
}

int canokey_piv_dispatch_process_apdu(const CAPDU *capdu, RAPDU *rapdu)
{
    piv_dispatch_log_capdu(capdu);
    int ret = __real_piv_process_apdu(capdu, rapdu);
    piv_dispatch_log_result(rapdu, ret);
    return ret;
}

int canokey_piv_dispatch_process_apdu_message(RAPDU_CHAINING *rapdu_chaining, CAPDU *capdu, RAPDU *rapdu)
{
    piv_dispatch_log_capdu(capdu);
    int ret = __real_piv_process_apdu_message(rapdu_chaining, capdu, rapdu);
    piv_dispatch_log_result(rapdu, ret);
    return ret;
}

int __wrap_piv_process_apdu(const CAPDU *capdu, RAPDU *rapdu)
{
    return canokey_piv_dispatch_process_apdu(capdu, rapdu);
}

int __wrap_piv_process_apdu_message(RAPDU_CHAINING *rapdu_chaining, CAPDU *capdu, RAPDU *rapdu)
{
    return canokey_piv_dispatch_process_apdu_message(rapdu_chaining, capdu, rapdu);
}
