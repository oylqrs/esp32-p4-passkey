#!/usr/bin/env python3
"""Generate pre-encoded CBOR segments for authenticatorGetInfo.

The generated file contains the compact CanoKey GetInfo profile. Runtime code
adds the CTAP status byte and the current ``clientPin`` value.
"""

import argparse
import re
import struct
import sys


def parse_defines(header_paths, needed):
    pattern = re.compile(r'^\s*#define\s+(\w+)\s+(-?(?:0[xX][0-9a-fA-F]+|\d+)|\w+)\b')
    raw = {}
    for path in header_paths:
        with open(path) as f:
            for line in f:
                m = pattern.match(line)
                if m:
                    raw[m.group(1)] = m.group(2)

    def resolve(name, seen=None):
        if seen is None:
            seen = set()
        if name in seen or name not in raw:
            return None
        seen.add(name)
        val_str = raw[name]
        if re.match(r'-?(?:0[xX][0-9a-fA-F]+|\d+)$', val_str):
            return int(val_str, 0)
        return resolve(val_str, seen)

    found = {}
    for name in needed:
        value = resolve(name)
        if value is not None:
            found[name] = value

    missing = [k for k in needed if k not in found]
    if missing:
        print(f"ERROR: missing #define constants: {', '.join(missing)}", file=sys.stderr)
        print(f"  Searched in: {', '.join(header_paths)}", file=sys.stderr)
        sys.exit(1)

    return found


def encode_uint(n):
    if n < 0:
        raise ValueError(f"negative uint: {n}")
    if n <= 23:
        return bytes([n])
    if n <= 0xFF:
        return bytes([0x18, n])
    if n <= 0xFFFF:
        return bytes([0x19, (n >> 8) & 0xFF, n & 0xFF])
    if n <= 0xFFFFFFFF:
        return struct.pack('>BI', 0x1A, n)
    return struct.pack('>BQ', 0x1B, n)


def encode_int(n):
    if n >= 0:
        return encode_uint(n)
    value = -1 - n
    if value <= 23:
        return bytes([0x20 | value])
    if value <= 0xFF:
        return bytes([0x38, value])
    if value <= 0xFFFF:
        return bytes([0x39, (value >> 8) & 0xFF, value & 0xFF])
    if value <= 0xFFFFFFFF:
        return struct.pack('>BI', 0x3A, value)
    return struct.pack('>BQ', 0x3B, value)


def encode_text(s):
    b = s.encode('utf-8')
    if len(b) <= 23:
        return bytes([0x60 | len(b)]) + b
    if len(b) <= 0xFF:
        return bytes([0x78, len(b)]) + b
    return bytes([0x79, (len(b) >> 8) & 0xFF, len(b) & 0xFF]) + b


def encode_bytes(b):
    if len(b) <= 23:
        return bytes([0x40 | len(b)]) + b
    if len(b) <= 0xFF:
        return bytes([0x58, len(b)]) + b
    return bytes([0x59, (len(b) >> 8) & 0xFF, len(b) & 0xFF]) + b


def encode_bool(v):
    return bytes([0xF5 if v else 0xF4])


def encode_array_header(n):
    if n <= 23:
        return bytes([0x80 | n])
    return bytes([0x98, n])


def encode_map_header(n):
    if n <= 23:
        return bytes([0xA0 | n])
    return bytes([0xB8, n])


AAGUID = bytes([0x24, 0x4E, 0xB2, 0x9E, 0xE0, 0x90, 0x4E, 0x49,
                0x81, 0xFE, 0x1F, 0x20, 0xF8, 0xD3, 0xB8, 0xF4])


def split_at_placeholder(data, placeholder):
    pos = data.index(placeholder)
    before = data[:pos]
    after = data[pos + len(placeholder):]
    return before, after


def algorithm_entry(alg_id):
    return (encode_map_header(2) + encode_text("alg") + encode_int(alg_id) +
            encode_text("type") + encode_text("public-key"))


def build_segments(c, attestation_format="packed"):
    """Build the compact GetInfo response used by USB and NFC.

    Keep the CanoKey capabilities required by current iOS/WebAuthn clients,
    while keeping the complete response below the short NFC APDU limit.
    ``clientPin`` remains dynamic, while the other advertised capabilities
    describe the authenticator implementation.

    ``attestation_format`` is retained for callers from older port builds; it
    no longer changes GetInfo because attestation formats are omitted here.
    """
    del attestation_format

    segments = {}
    versions = (encode_array_header(3) + encode_text("U2F_V2") +
                encode_text("FIDO_2_0") + encode_text("FIDO_2_1"))

    # Thirteen top-level fields: versions, extensions, aaguid, options,
    # maxMsgSize, pinUvAuthProtocols, maxCredentialCountInList,
    # maxCredentialIdLength, transports, algorithms, large-blob capacity,
    # firmwareVersion, and credBlob capacity.
    segments["cbor_gi_prefix_before_versions"] = encode_map_header(13) + encode_uint(c['GI_RESP_VERSIONS'])
    segments["cbor_gi_versions"] = versions

    after_versions = bytearray()
    after_versions += encode_uint(c['GI_RESP_EXTENSIONS']) + encode_array_header(4)
    for ext in ["credBlob", "credProtect", "hmac-secret", "largeBlobKey"]:
        after_versions += encode_text(ext)
    after_versions += encode_uint(c['GI_RESP_AAGUID']) + encode_bytes(AAGUID)
    after_versions += encode_uint(c['GI_RESP_OPTIONS'])
    segments["cbor_gi_after_versions_before_options"] = bytes(after_versions)
    segments["cbor_gi_options_before_up"] = encode_text("rk") + encode_bool(True)
    segments["cbor_gi_up_option"] = encode_text("up") + encode_bool(True)

    options_after_up = bytearray()
    options_after_up += encode_text("credMgmt") + encode_bool(True)
    options_after_up += encode_text("clientPin")
    client_pin_marker = b'\xF0CLIENTPIN'
    options_after_up += client_pin_marker
    options_after_up += encode_text("largeBlobs") + encode_bool(True)
    options_after_up += encode_text("pinUvAuthToken") + encode_bool(True)
    options_after_up += encode_text("makeCredUvNotRqd") + encode_bool(True)
    options_after_up += encode_uint(c['GI_RESP_MAX_MSG_SIZE']) + encode_uint(c['CTAP_GET_INFO_MAX_MSG_SIZE'])
    options_after_up += encode_uint(c['GI_RESP_PIN_UV_AUTH_PROTOCOLS'])
    options_after_up += encode_array_header(2) + encode_uint(1) + encode_uint(2)
    options_after_up += encode_uint(c['GI_RESP_MAX_CREDENTIAL_COUNT_IN_LIST']) + encode_uint(c['MAX_CREDENTIAL_COUNT_IN_LIST'])
    options_after_up += encode_uint(c['GI_RESP_MAX_CREDENTIAL_ID_LENGTH']) + encode_uint(c['CREDENTIAL_ID_SIZE'])
    options_after_up += encode_uint(c['GI_RESP_TRANSPORTS'])
    options_after_up += encode_array_header(2) + encode_text("nfc") + encode_text("usb")
    options_after_up += encode_uint(c['GI_RESP_ALGORITHMS']) + encode_array_header(2)
    options_after_up += algorithm_entry(c['COSE_ALG_ES256'])
    options_after_up += algorithm_entry(c['COSE_ALG_EDDSA'])
    options_after_up += encode_uint(c['GI_RESP_MAX_SERIALIZED_LARGE_BLOB_ARRAY'])
    options_after_up += encode_uint(c['LARGE_BLOB_SIZE_LIMIT'])
    options_after_up += encode_uint(c['GI_RESP_FIRMWARE_VERSION']) + encode_uint(c['FIRMWARE_VERSION'])
    options_after_up += encode_uint(c['GI_RESP_MAX_CRED_BLOB_LENGTH'])
    options_after_up += encode_uint(c['MAX_CRED_BLOB_LENGTH'])

    before, after = split_at_placeholder(bytes(options_after_up), client_pin_marker)
    segments["cbor_gi_options_after_up_before_client_pin"] = before
    segments["client_pin_placeholder"] = b""
    segments["cbor_gi_after_client_pin"] = after

    return segments


def format_c_array(data):
    if not data:
        return ""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def write_array(f, name, data):
    f.write(f"static const uint8_t {name}[{len(data)}] = {{\n")
    body = format_c_array(data)
    if body:
        f.write(body + "\n")
    f.write("};\n\n")


def main():
    parser = argparse.ArgumentParser(description="Generate const CBOR segments for CTAP GetInfo")
    parser.add_argument("--headers", nargs="+", required=True)
    parser.add_argument("--credential-id-size", type=int, required=True)
    parser.add_argument("--attestation-format", choices=("packed", "none"), default="packed")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    needed = {
        'GI_RESP_VERSIONS': None,
        'GI_RESP_EXTENSIONS': None,
        'GI_RESP_AAGUID': None,
        'GI_RESP_OPTIONS': None,
        'GI_RESP_MAX_MSG_SIZE': None,
        'CTAP_GET_INFO_MAX_MSG_SIZE': None,
        'GI_RESP_PIN_UV_AUTH_PROTOCOLS': None,
        'GI_RESP_MAX_CREDENTIAL_COUNT_IN_LIST': None,
        'GI_RESP_MAX_CREDENTIAL_ID_LENGTH': None,
        'GI_RESP_TRANSPORTS': None,
        'GI_RESP_ALGORITHMS': None,
        'GI_RESP_MAX_SERIALIZED_LARGE_BLOB_ARRAY': None,
        'GI_RESP_FIRMWARE_VERSION': None,
        'GI_RESP_MAX_CRED_BLOB_LENGTH': None,
        'FIRMWARE_VERSION': None,
        'MAX_CREDENTIAL_COUNT_IN_LIST': None,
        'LARGE_BLOB_SIZE_LIMIT': None,
        'MAX_CRED_BLOB_LENGTH': None,
        'COSE_ALG_ES256': None,
        'COSE_ALG_EDDSA': None,
    }
    consts = parse_defines(args.headers, needed)
    consts['CREDENTIAL_ID_SIZE'] = args.credential_id_size

    segments = build_segments(consts, args.attestation_format)
    with open(args.output, 'w') as f:
        f.write("// Auto-generated by scripts/gen_ctap_get_info.py. DO NOT EDIT.\n")
        f.write("// Static CTAP GetInfo CBOR segments; dynamic fields are inserted at runtime.\n\n")
        for key in sorted(consts):
            f.write(f"// {key} = {consts[key]}\n")
        f.write("\n")
        for name, data in segments.items():
            if name.endswith("_placeholder"):
                continue
            write_array(f, name, data)

    total_static = sum(len(v) for k, v in segments.items() if not k.endswith("_placeholder"))
    # Runtime contributes the options-map header and the clientPin boolean.
    if total_static + 2 > 256:
        raise ValueError(f"compact GetInfo CBOR exceeds 256 bytes: {total_static + 2}")
    print(f"Generated {args.output}: {total_static} static bytes in {len([k for k in segments if not k.endswith('_placeholder')])} segments")


if __name__ == "__main__":
    main()
