# Jade UI port notice

The wallet dashboard model and LVGL presentation in this directory are adapted
from the Blockstream Jade firmware checkout at:

`E:/oyl/2026/202606/Jade`

Source revision reviewed: `9c097297f58339c15fb9b8df4c7fe105efefb902`.

The vendored `libwally-core` source is copied from Jade's pinned submodule at
revision `3bf543cd06a67fdd877688a6304808f270351aee`. Its upstream `LICENSE` and the
individual CCAN licenses remain in `libwally-core/upstream`.

Relevant upstream files reviewed for this stage:

- `main/process/dashboard.c`: uninitialized, locked, and active home states;
  state-dependent menu entries and status labels.
- `main/ui/dashboard.c`: selected/next panel composition and footer layout.
- `main/gui.c`, `main/display.c`, and `main/input.c`: reviewed to identify the
  renderer and hardware code that must not be copied into this LVGL project.

The upstream collection declares GPLv3 licensing in its README. The adapted
files `jade_ui_model.c`, `jade_ui_model.h`, and `jade_wallet_ui.c` are marked
`GPL-3.0-or-later`. `COPYING.JADE` contains the upstream GPLv3 text. Review the
license obligations for the complete distributed firmware before release.

This port intentionally does not include Jade's renderer, display driver,
button/wheel input, dashboard process loop, keychain persistence, PIN server,
or communication transports. Those subsystems conflict with this project's
LVGL/touch/event ownership or belong to later backend stages. Jade's pinned
libwally component is included as the Bitcoin cryptography and PSBT primitive
provider.
