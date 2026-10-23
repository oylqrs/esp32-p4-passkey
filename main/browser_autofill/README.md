# Browser Autofill

This directory implements the KeePassHTTP subset used by ChromeKeePass.

## Browser configuration

Enable USB NCM on the device, then configure the extension:

- Host: `172.16.8.1`
- Port: `19455`

The extension sends JSON with `POST http://172.16.8.1:19455/`.

## Supported requests

- `associate`
- `test-associate`
- `get-logins`
- `get-logins-count`

`set-login` is intentionally rejected. The device is read-only from the
browser's perspective.

Association always requires confirmation on the device. Credential access
also requires confirmation unless **Trust Browser** is enabled. Resetting the
browser pairing removes the stored KeePassHTTP identifier and shared key.

Credential matching uses an exact, case-insensitive hostname match. At most
eight entries are returned per request.

## Security notes

KeePassHTTP uses its legacy AES-256-CBC protocol for compatibility. Pairing
keys are stored in NVS; production builds should enable NVS encryption.
Decrypted request data, copied credentials, and raw association requests are
cleared after use. Passwords are never written to logs.

