# ESP32-P4 / Chameleon NFC FIDO bridge, version 1

The ESP32 implementation is in `port_nfc.c`. No CanoKey core source changes
are required. This is a **private firmware extension**, not a protocol
implemented by stock Chameleon firmware. The Chameleon side still needs to
implement the contract below before an NFC reader can use this device.

## Integration

`main/app_main.c` binds `chameleon_uart_nfc_transceive` after UART initialization.
The bridge starts from the CanoKey task after device unlock and FIDO startup,
not at boot while the device is locked. Disabling the FIDO menu keeps the
existing behavior of not starting CanoKey. Debug-monitor UART mode does not
bind the bridge because it occupies the same GPIO15/16 pins.

All new NFC protocol/session/APDU logic is under `canokey-port`. The main
component supplies only the UART adapter and the existing confirmation UI.
The port enables `ENABLE_NFC=1`, `NFC_CHIP=-1`, and the CanoKey Type-4 NDEF
applet for Android discovery; FM11 hardware is neither accessed nor required.

NFC owns the applet state for one RF session. A powered CCID card, active HID
operation, or partially received large HID request blocks RF session admission.
The frontend must retain FIELD_ON while ESP32 is busy, but replace it with
FIELD_OFF if the reader leaves. USB HID receives CHANNEL_BUSY while NFC is
active. CCID and CanoKey device-loop work share the port's recursive mutex.

## UART envelope

Use the existing Chameleon frame at 115200 baud, 8N1:

`11 EF CMD:u16 STATUS:u16 LENGTH:u16 HEADER_LRC DATA DATA_LRC`

All multi-byte fields in the envelope and payloads are big endian. LRC is the
two's complement of the byte sum. Request STATUS is zero; successful reply
STATUS is `0068`. The command in each reply must match the request. Replies
are synchronous; **do not send unsolicited UART APDU frames**, because the
ESP32 transport flushes RX before a new transaction. Keep events in a queue
until they are acknowledged by POLL.

Command range `7F00` through `7F03` is private to this integration. Each command
must return immediately after handling its request. POLL is nonblocking, and
WTX runs independently in Chameleon's RF task.

ESP32 polls every 100 ms while no RF session is active and every 20 ms after
FIELD_ON. UART failures start a 500 ms retry delay and, while inactive, back
off exponentially to five seconds. An active session remains subject to the
two-second link-loss timeout.

## Commands

| Command | ESP32 request DATA | Chameleon reply DATA |
| --- | --- | --- |
| `7F00 CONFIGURE` | `version:u8 enable:u8 max_capdu:u16 max_rapdu:u16` | Exact accepted configuration, six bytes |
| `7F01 POLL` | `ack_session:u32 ack_event:u32` | `event:u8 session:u32 event_id:u32 [CAPDU]` |
| `7F02 RESPONSE` | `session:u32 event_id:u32 RAPDU` | Matching `session:u32 event_id:u32`, eight bytes |
| `7F03 RESET` | Empty | Empty |

CONFIGURE v1 currently sends `01 01 01 29 0F F8`: maximum complete CAPDU is
297 bytes (288-byte parsed Lc plus a nine-byte extended APDU header/trailer),
maximum RAPDU is 4088 bytes **including SW1/SW2**. Larger FIDO CBOR commands
use ISO7816 command chaining; ISO-DEP block chaining does not bypass the CAPDU
limit. If APDU buffer constants change, derive limits from the configuration
payload instead of hardcoding these values.

CONFIGURE is a fresh bridge boundary: discard old events, cached RF responses,
and old ISO-DEP state, then enter FIDO ISO14443-A/ISO-DEP card emulation.
No FIDO AID selection should be handled locally on Chameleon. Enable `0` disables
the bridge and restores the previous tag/mode settings. RESET aborts the active
ISO-DEP exchange, clears the event queue and field state, disables the bridge,
and restores the previous mode. ESP32 reconfigures after a five-second delay
following a reset. Do not persist bridge mode into flash or slot configuration.

A non-success CONFIGURE reply or a successful reply with mismatched version or
limits disables the ESP32 bridge for this boot and logs one warning.
ESP32 also sends RESET to release any partially accepted configuration. Initial
UART timeouts retry CONFIGURE every five seconds, silently. USB FIDO remains
available. Use a non-success reply for unsupported commands/versions.

## Events and reliability

| Event | Value | Payload rules |
| --- | --- | --- |
| NONE | `00` | Exactly nine bytes; both IDs are zero |
| FIELD_ON | `01` | Exactly nine bytes; new nonzero session and event IDs |
| FIELD_OFF | `02` | Exactly nine bytes; identifies the session that ended |
| APDU | `03` | Nine-byte event header followed by one complete CAPDU |

Session IDs must change at each RF activation and not be reused while old
requests can still be in flight. Event IDs are nonzero and strictly increasing
within a session. Do not reuse an event ID for a different command. On wrap,
start a new session. Send FIELD_ON before any APDU in that session.

POLL first consumes the exact acknowledged event, then returns the oldest
unacknowledged event or NONE. Duplicate POLL acknowledgements are idempotent.
The first POLL has both acknowledgement IDs zero. A repeated event must contain
exactly the same IDs, type, and CAPDU bytes.

RESPONSE accepts a RAPDU only for the pending APDU with matching IDs. Retain
that APDU event until its subsequent POLL acknowledgement. A duplicate RESPONSE
must reply with the same IDs without transmitting the RAPDU over RF again.
ESP32 caches the most recent APDU and RAPDU and does not execute duplicate
APDUs twice. This protects FIDO credential creation and signature counters when
a UART acknowledgement is lost. Never advance to the next APDU before RESPONSE
acceptance and its POLL acknowledgement.

FIELD_OFF is the exception: it takes priority, discards pending APDUs, cancels
WTX and RF response transmission, and causes late RESPONSEs to return an error.
Keep FIELD_OFF available until acknowledged. If a new reader arrives before
that acknowledgement, queue its FIELD_ON afterwards. ESP32 drops CTAP pending
requests, response sources, APDU chaining, touch state, and volatile applet state
on FIELD_OFF, malformed events, or a lost link. An RF session is also reset if
the same RAPDU cannot be acknowledged for two seconds. CONFIGURE resets both
ends after a Chameleon reboot or a UART framing failure.

Chameleon must expire bridge ownership and restore normal mode if ESP32 stops
polling: allow at least 35 seconds while an APDU is being processed, since
ESP32's synchronous user-presence path can wait up to 30 seconds. While idle,
use a short watchdog to recover from an ESP32 reboot. This watchdog is required
to avoid leaving NFC FIDO active after ESP32 locks/restarts or disables FIDO.

## RF and FIDO responsibilities

Chameleon handles RF activation, UID/ATQA/SAK/ATS, ISO14443-4 CRC/block numbers,
I/R/S blocks, retransmission, ISO-DEP block chaining and deselection. Forward
only complete ISO7816 APDUs. Advertise ISO-DEP support in SAK and a valid ATS.
Send and handle S(WTX) before the negotiated frame waiting time expires, for
the whole pending APDU interval, including UART retries and ESP32 cryptography.
Do not assume a fixed 150 ms WTX period is valid for every advertised ATS.

ESP32 accepts SELECT for FIDO AID `A0 00 00 06 47 2F 00 01`, and also accepts a
first CTAP2/U2F command as an implicit FIDO selection when no applet is active.
For Android discovery it also routes the Type-4 NDEF AID
`D2 76 00 00 85 01 01`, SELECT FILE, READ BINARY, and UPDATE BINARY to CanoKey's
NDEF applet. NDEF selection never authorizes CTAP; the reader must subsequently
select the FIDO AID. Once FIDO is selected, accepted commands are U2F operations,
CTAP2 MSG (`80/90 10`), NFC GET_RESPONSE (`80 11`), and ISO GET RESPONSE
(`00/80 C0`). Other applet AIDs and administrative APDUs are rejected. The
existing WebAuthn and NDEF enable settings are still enforced by CanoKey. FIDO
SELECT response comes from CanoKey (`U2F_V2` in this core).
The port initializes an empty NDEF message instead of CanoKey's stock
`canokeys.org` URI and migrates only that stock value; user-written NDEF data
is preserved. This keeps Type-4 discovery available without opening a browser.

On NFC, authenticatorGetInfo uses the same compact capability profile as USB:
`versions: ["FIDO_2_0", "FIDO_2_1", "U2F_V2"]`, the common
`credBlob`/`credProtect`/`hmac-secret`/`largeBlobKey` extensions, the device
AAGUID, resident-key/user-presence options, `maxMsgSize: 1024`,
`pinUvAuthProtocols: [1, 2]`, and `transports: ["nfc", "usb"]`. The standard
`pinUvAuthToken` option remains true; `clientPin` reflects the current PIN
state. The complete response is below the 256-byte CBOR limit, so it is
returned with `9000` without an intermediate `61xx`; Chameleon only handles
ISO-DEP block framing.

Keep `61xx` response chaining visible to the reader for short APDUs. For every
extended FIDO APDU, including CTAP2 and native U2F, ESP32 drains the core's
internal `61xx` chunks into one RAPDU, up to the negotiated RAPDU limit.
Chameleon fragments that RAPDU into ISO-DEP blocks. These two kinds of chaining
are separate.

CTAP2 MSG with P1 bit 7 set supports NFC user-presence polling: return the
core's keepalive RAPDU ending in `9100`, let the reader send only `80 11` while presence is pending, and later
return the completed result. Re-selects and unrelated probes are rejected until that poll completes. Never turn `9100` into `9000` or poll CTAP locally
on behalf of the reader. Reader cancellation uses `80 11 11 00 00`. The ESP32
confirmation UI uses the same CTAP cancellation path for its Cancel button.
Expired or disconnected NFC dialogs cannot approve a later RF session.

## Bridge state-machine tests

`test/test_nfc_bridge.c` links the actual port implementation to a fake UART
and applet engine. It checks session admission, APDU retry caching, cancellation,
response aggregation, NDEF discovery and FIDO switching, field loss, and
unsupported firmware. It does not test real FIDO cryptography or RF timing. The
runner uses the ESP RISC-V compiler and Unicorn, with Python dependencies in
`test/requirements.txt`.

```powershell
python -m pip install --target canokey-port/test/build/deps -r canokey-port/test/requirements.txt
powershell -NoProfile -ExecutionPolicy Bypass -File canokey-port/test/run_nfc_bridge.ps1
```

Pass `-CompileOnly` to compile the harness without installing or running the
emulator. Use the ESP-IDF Python environment (which includes pyelftools), or
specify its path with the runner's `-Python` argument.

## Hardware acceptance checks

1. Stock Chameleon firmware: one unsupported-bridge warning; USB FIDO still works.
2. Android discovery: SELECT `00 A4 04 00 07 D2 76 00 00 85 01 01 00`, expect
   `90 00`, then verify CC file `E103` and NDEF file `0001` can be selected/read.
3. FIDO selection: SELECT `00 A4 04 00 08 A0 00 00 06 47 2F 00 01 00`,
   expect `55 32 46 5F 56 32 90 00` (`U2F_V2`, SW `9000`).
4. Extended CTAP GetInfo `80 10 00 00 00 00 01 04 00 00`; expect one complete
   RAPDU ending in `9000`, without an intermediate `61xx` visible to the phone.
5. MakeCredential and GetAssertion with NFC polling: verify confirmation,
   cancellation (`2D 90 00`), signatures, counters, and WTX while crypto runs.
6. Drop a RESPONSE UART acknowledgement: same event reappears, byte-identical
   RAPDU is returned, and the command is executed only once.
7. Remove RF during confirmation/response chaining, then tap again: require a
   fresh SELECT/session; no old dialog, pending response, or touch grants survive.
8. Exercise USB HID and powered CCID alongside NFC, UART corruption, Chameleon
   reboot, and ESP32 reboot to check arbitration and recovery.

Real RF/phone validation requires the matching Chameleon firmware and hardware;
the ESP32 build alone cannot verify ISO-DEP timing or frontend WTX behavior.
