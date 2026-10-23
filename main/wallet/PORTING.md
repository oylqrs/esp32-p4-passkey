# Jade wallet port: stages 1-4

Source reviewed: `E:/oyl/2026/202606/Jade` (local checkout). This stage does
not copy Jade source or create a usable wallet.

## Stage 1: scope

The first deliverable is a Wallet menu entry that opens a separate LVGL screen
and returns to the main menu. It must report unavailable until wallet behavior
exists. No seed entry, generation, PIN, address, QR, signing, persistence, or
transport is enabled by this deliverable.

Proposed functional first release, to be approved before implementation:
Bitcoin testnet first; create/restore BIP39 seed, separate wallet PIN and
lock/unlock, receive address with on-device confirmation, QR-based PSBT import
and signed PSBT export. Do not silently reuse the device/KDBX PIN or storage.
Liquid, BLE, Jade companion protocol over USB/serial, PIN server, and Jade OTA
are outside that proposed first release. Network, backup, and recovery choices
must be confirmed before any key material is generated.

## Stage 2: compatibility and security gates

- Jade `main/process/dashboard.c` owns the uninitialized/locked/active state
  machine and message loop. `main/gui.c`, `display.c`, and input code own a
  separate renderer, task, and input/event model. This project already owns
  LVGL, FT6336 touch, display, default event loop, and navigation. Port the
  workflow into LVGL; do not start Jade's `app_main`, GUI task, or display
  driver alongside them.
- Jade's reviewed configuration and dependency set targets ESP32/ESP32-S3
  (IDF 5.5.4); this firmware targets ESP32-P4. Check every selected crypto,
  QR, camera, and transport dependency for P4 support before importing it.
- This device has 32 MB flash, PSRAM, two 0x770000 OTA app slots, shared NVS,
  and a FAT USB store. Measure final binary, internal heap, stack, and PSRAM
  use. Reserve isolated encrypted wallet storage; do not put seeds in the FAT
  volume, logs, or the existing KDBX database.
- Audit entropy, seed lifecycle, zeroization, PIN rate limiting, secure boot,
  flash encryption, physical extraction threat model, and recovery behavior.
  Do not claim parity with Jade hardware security merely from using its UI.
- Existing TinyUSB HID/CCID/MSC/NCM modes can conflict with a Jade USB
  protocol. The proposed QR-only first release avoids that integration;
  camera availability and QR decoder support remain unverified.
- Jade README says the collection is GPL3 and individual components can have
  separate licenses. Audit each file and dependency before copying source,
  and document attribution/distribution obligations.

These are review gates, not certifications of compatibility or security.

## Stage 3: ownership boundary

`wallet/wallet_service.h` is the UI-facing boundary. It reports uninitialized,
locked, and ready states and exposes explicit create, unlock, lock, PIN change,
delete, and address operations.
The service must own sensitive state and storage; UI code must only handle
short-lived presentation data. No wallet operation should depend on the main
menu's LVGL objects or call LVGL from a worker task.

## Stage 4: navigation acceptance

`main_item_ui.c` routes Wallet (menu index 6) through `ui_nav_push` to
`jade_wallet_ui_show`. The LVGL page uses the shared status bar and shows an
unavailable state. The back button and right swipe return to the menu. The
page creates no background task or timer. Verify repeated enter/back cycles,
language switching, menu visibility settings, and other menu destinations on
hardware before treating stage 4 as complete.

## Stage 5: LVGL workflow adapter

Implemented on 2026-09-21 under `main/wallet`. `jade_ui_model.c` ports the
dynamic home-state/menu rules from Jade's `main/process/dashboard.c`, covering
uninitialized, initialized/locked, and active states. Optional SeedQR, QR Mode,
and Scan QR entries are only present when the service reports QR scan support.
`jade_wallet_ui.c` adapts Jade's selected/next panel and footer composition to
this project's LVGL 9, 480x640 touchscreen, status bar, and navigation stack.
It calls only `wallet_service.h`; setup presentation data is explicitly wiped
when the flow exits. Deletion cleanup covers the right-swipe navigation path.
English and Chinese labels are included.

The setup flow supports 12/24-word generation, BIP39 import and validation,
recovery-word display, PIN confirmation, lock/unlock, PIN change, wallet
deletion, and BIP84 address display for Bitcoin and testnet.

Receive-address management follows Jade's caller-controlled path model. The
current external-chain index is persisted separately for Bitcoin mainnet and
testnet in a versioned NVS record. Opening or redrawing the page never consumes
an address: only the explicit New Address action advances and commits the
index. Previous and next controls browse already allocated indexes, and the UI
shows the complete BIP84 derivation path. Address strings are derived on demand
and are not duplicated in storage. Deleting a wallet also deletes this address
metadata.

## Stage 6: backend and transport gate status

Implemented through local encrypted persistence on 2026-09-21. Jade's pinned libwally revision is
vendored as `main/wallet/libwally-core` and builds as an independent ESP-IDF
component on ESP32-P4. The port resolves its CCAN SHA symbol overlap with the
existing CanaKey crypto port by namespacing libwally's private SHA symbols.

`wallet_crypto.c` now provides real BIP39 validation and 12/24-word generation
from `esp_fill_random`, BIP84 mainnet/testnet address derivation, account XPUB
export, strict PSBT parsing, transaction review, and BIP84 PSBT signing. Its
runtime self-test derives `m/84'/0'/0'/0/0` from the standard 12-word test
mnemonic and checks the expected BIP84 address. XPUB derivation and PSBT
signing are deliberately excluded from the synchronous UI-entry self-test
because they are heavyweight operations. Sensitive seed, extended-key,
entropy, and address buffers are explicitly zeroed. The service exposes wallet
capabilities only after the lightweight self-test passes.

`wallet_storage.c` stores one fixed-size authenticated blob in NVS. A random
16-byte salt and 100,000-round PBKDF2-HMAC-SHA256 derive an AES-256 key from a
6-to-12 digit wallet PIN. AES-GCM uses a random 12-byte nonce and authenticates
the blob header. The plaintext mnemonic is never written directly to NVS.
The in-memory mnemonic is zeroed when the wallet locks or the Wallet UI exits.

The signing policy intentionally accepts only single-signature native SegWit
BIP84 inputs for the selected network and current account. Every wallet input
must match its master fingerprint, exact derivation path, compressed public
key, and P2WPKH UTXO script. Taproot, multisig, Liquid/PSET, unsupported
sighashes, excessive output counts, and unknown output scripts are rejected.
The UI reviews non-change destinations, amounts, change, and fee before an
explicit confirmation, then returns a Base64 PSBT and optionally a static QR.

Jade PIN-server authentication, device-bound eFuse keys, PIN rate limiting,
camera integration, QR decoding, and animated BC-UR transport are not yet
ported. Base64 text entry is the current PSBT import path; PSBTs too large for
a static QR must be transported outside the QR screen.

Current security configuration is unsuitable for persistent wallet secrets:

- `CONFIG_SECURE_BOOT` is disabled.
- `CONFIG_FLASH_ENCRYPTION_ENABLED` is disabled.
- `CONFIG_NVS_ENCRYPTION` is disabled.
- The existing FAT USB store is host-readable and must never contain a seed.

The seed is persisted only inside the application-encrypted blob. Because a
numeric PIN has limited entropy and this build has no device-bound secret,
an attacker who extracts flash can attempt offline PIN guesses. This stage is
appropriate for functional integration testing, not real-fund deployment.
QR import additionally requires the actual camera hardware, driver, and
decoder to be specified.

## Stage 7: verification record

Automated checks for this increment:

- ESP-IDF full configure, compile, link, image generation, and partition-size
  check pass for ESP32-P4 with IDF 5.4.4.
- Jade's pinned libwally compiles and links on ESP32-P4. The on-device runtime
  self-test remains to be confirmed from serial logs after flashing.
- Image size after this increment must remain below both 0x770000 OTA slots.
- Service operations are denied unless the Wallet session is entered and the
  state required by that operation is active.

Hardware verification remains required: create both mnemonic sizes, reboot,
unlock with the correct and an incorrect PIN, compare the displayed BIP84
address with an independent wallet, change the PIN, test import, lock, and
delete, and repeat navigation in both languages. Real-fund testing remains
prohibited because hardware-backed key protection, rate limiting, and
production transport are not implemented yet.

Latest ESP32-P4 build after linking XPUB export and PSBT signing is 3,743,968
bytes (`0x3920e0`), leaving 52% of the smaller OTA application slot free.
Cryptographic primitives, validation/derivation, persisted receive-index
management, transaction review, and constrained signing are now present.

## Bitcoin send, QR export, and USB Jade protocol

The Wallet now presents the existing PSBT review/signing workflow as **Send
Bitcoin**. This is intentionally an offline hardware-wallet flow: the device
does not discover UTXOs, calculate a spend from a local balance, contact a
Bitcoin node, or broadcast transactions. A host constructs a PSBT, the device
validates and reviews its destinations/change/fee, and the host broadcasts the
returned signed PSBT. The same restricted BIP84 signing policy documented
above applies.

Receive addresses can be exported from the address-management page using a
large static QR overlay. XPUB and signed-PSBT static QR export remain
available. Animated BC-UR and camera-based QR import are still not included,
so large PSBTs must use USB or Base64 text input.

`jade_usb_protocol.c` adds an opt-in TinyUSB CDC ACM transport alongside the
existing keyboard and CTAPHID interfaces. It follows Jade's concatenated CBOR
RPC framing and implements this explicit compatibility subset:

- `ping`
- `get_version_info`
- `auth_user`
- `get_xpub`
- `sign_psbt`
- `logout`

`auth_user` never accepts a PIN over USB; it succeeds only after the Wallet
has already been unlocked on the touchscreen. `get_xpub` accepts only the
selected network's current `m/84'/coin'/account'` path. `sign_psbt` accepts a
binary PSBT up to 6144 bytes, applies the local BIP84 policy, and always waits
for explicit on-device approval. Requests are limited to one transaction
confirmation at a time and time out after two minutes. Jade extended-data
chunking, multisig, Liquid/PSET, pinserver authentication, OTA, entropy calls,
and the rest of Jade's RPC surface are not implemented.

The Wallet option toggles a dedicated HID+CDC descriptor (VID `0x303A`, PID
`0x4014`). Leaving Wallet stops the RPC worker, locks the in-memory seed, and
restores the normal HID/FIDO descriptor. Smartcard, USB storage, NCM, and Jade
CDC modes remain mutually exclusive because they reuse endpoint numbers.

Hardware verification is required for Windows/Linux CDC enumeration, Jade
Python `get_version_info`, locked/unlocked authentication, XPUB path/network
rejection, PSBT approve/cancel/timeout, malformed CBOR recovery, reconnect,
and restoration of FIDO operation after leaving Wallet.
