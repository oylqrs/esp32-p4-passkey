#include <stddef.h>
#include <stdint.h>

#include "des.h"
#include "ecc.h"
#include "esp_log.h"
#include "key.h"
#include "rsa.h"

static const char *TAG = "piv_crypto";

int __real_rsa_generate_key(rsa_key_t *key, uint16_t nbits);
int __real_rsa_get_public_key(rsa_key_t *key, uint8_t *n);
int __real_rsa_private(const rsa_key_t *key, const uint8_t *input, uint8_t *output);
int __real_rsa_sign_pkcs_v15(const rsa_key_t *key, const uint8_t *data, size_t len, uint8_t *sig);
int __real_tdes_enc(const uint8_t *in, uint8_t *out, const uint8_t *key);
int __real_tdes_dec(const uint8_t *in, uint8_t *out, const uint8_t *key);
int __real_ecdh(key_type_t type, const uint8_t *priv_key, const uint8_t *receiver_pub_key, uint8_t *out);
int __real_ck_generate_key(ck_key_t *key);
int __real_ck_sign(const ck_key_t *key, const uint8_t *input, size_t input_len, uint8_t *sig);

static const char *piv_crypto_key_type_name(key_type_t type)
{
    switch (type) {
    case RSA2048:
        return "RSA2048";
    case RSA3072:
        return "RSA3072";
    case RSA4096:
        return "RSA4096";
    case SECP256R1:
        return "SECP256R1";
    case SECP384R1:
        return "SECP384R1";
    case SECP521R1:
        return "SECP521R1";
    case ED25519:
        return "ED25519";
    case X25519:
        return "X25519";
    case SECP256K1:
        return "SECP256K1";
    case SM2:
        return "SM2";
    default:
        return "UNKNOWN";
    }
}

int canokey_piv_crypto_rsa_generate_key(rsa_key_t *key, uint16_t nbits)
{
    int ret = __real_rsa_generate_key(key, nbits);
    ESP_LOGD(TAG, "RSA generate nbits=%u ret=%d", (unsigned)nbits, ret);
    return ret;
}

int canokey_piv_crypto_rsa_get_public_key(rsa_key_t *key, uint8_t *n)
{
    int ret = __real_rsa_get_public_key(key, n);
    ESP_LOGD(TAG, "RSA public nbits=%u ret=%d", key ? (unsigned)key->nbits : 0, ret);
    return ret;
}

int canokey_piv_crypto_rsa_private(const rsa_key_t *key, const uint8_t *input, uint8_t *output)
{
    int ret = __real_rsa_private(key, input, output);
    ESP_LOGD(TAG, "RSA private nbits=%u ret=%d", key ? (unsigned)key->nbits : 0, ret);
    return ret;
}

int canokey_piv_crypto_rsa_sign_pkcs_v15(const rsa_key_t *key, const uint8_t *data, size_t len, uint8_t *sig)
{
    int ret = __real_rsa_sign_pkcs_v15(key, data, len, sig);
    ESP_LOGD(TAG, "RSA PKCS#1 v1.5 sign nbits=%u len=%u ret=%d", key ? (unsigned)key->nbits : 0, (unsigned)len, ret);
    return ret;
}

int canokey_piv_crypto_tdes_enc(const uint8_t *in, uint8_t *out, const uint8_t *key)
{
    int ret = __real_tdes_enc(in, out, key);
    ESP_LOGD(TAG, "TDEA encrypt ret=%d", ret);
    return ret;
}

int canokey_piv_crypto_tdes_dec(const uint8_t *in, uint8_t *out, const uint8_t *key)
{
    int ret = __real_tdes_dec(in, out, key);
    ESP_LOGD(TAG, "TDEA decrypt ret=%d", ret);
    return ret;
}

int canokey_piv_crypto_ecdh(key_type_t type, const uint8_t *priv_key, const uint8_t *receiver_pub_key, uint8_t *out)
{
    int ret = __real_ecdh(type, priv_key, receiver_pub_key, out);
    ESP_LOGD(TAG, "ECDH type=%s ret=%d", piv_crypto_key_type_name(type), ret);
    return ret;
}

int canokey_piv_crypto_generate_key(ck_key_t *key)
{
    key_type_t type = key ? key->meta.type : KEY_TYPE_PKC_END;
    int ret = __real_ck_generate_key(key);
    ESP_LOGD(TAG, "CK generate type=%s ret=%d", piv_crypto_key_type_name(type), ret);
    return ret;
}

int canokey_piv_crypto_sign(const ck_key_t *key, const uint8_t *input, size_t input_len, uint8_t *sig)
{
    key_type_t type = key ? key->meta.type : KEY_TYPE_PKC_END;
    int ret = __real_ck_sign(key, input, input_len, sig);
    ESP_LOGD(TAG, "CK sign type=%s input_len=%u ret=%d", piv_crypto_key_type_name(type), (unsigned)input_len, ret);
    return ret;
}

int __wrap_rsa_generate_key(rsa_key_t *key, uint16_t nbits)
{
    return canokey_piv_crypto_rsa_generate_key(key, nbits);
}

int __wrap_rsa_get_public_key(rsa_key_t *key, uint8_t *n)
{
    return canokey_piv_crypto_rsa_get_public_key(key, n);
}

int __wrap_rsa_private(const rsa_key_t *key, const uint8_t *input, uint8_t *output)
{
    return canokey_piv_crypto_rsa_private(key, input, output);
}

int __wrap_rsa_sign_pkcs_v15(const rsa_key_t *key, const uint8_t *data, size_t len, uint8_t *sig)
{
    return canokey_piv_crypto_rsa_sign_pkcs_v15(key, data, len, sig);
}

int __wrap_tdes_enc(const uint8_t *in, uint8_t *out, const uint8_t *key)
{
    return canokey_piv_crypto_tdes_enc(in, out, key);
}

int __wrap_tdes_dec(const uint8_t *in, uint8_t *out, const uint8_t *key)
{
    return canokey_piv_crypto_tdes_dec(in, out, key);
}

int __wrap_ecdh(key_type_t type, const uint8_t *priv_key, const uint8_t *receiver_pub_key, uint8_t *out)
{
    return canokey_piv_crypto_ecdh(type, priv_key, receiver_pub_key, out);
}

int __wrap_ck_generate_key(ck_key_t *key)
{
    return canokey_piv_crypto_generate_key(key);
}

int __wrap_ck_sign(const ck_key_t *key, const uint8_t *input, size_t input_len, uint8_t *sig)
{
    return canokey_piv_crypto_sign(key, input, input_len, sig);
}
