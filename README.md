# Feather — AirDrop radio developer preview

A native Plasma 6 tray applet, powered by Owly. This preview controls the
AirDrop radio only. **File sharing, receiving, recipient discovery and Dolphin
integration are not implemented.** No radio is enabled at login or when opening
the applet.

Wi-Fi is unavailable while the Intel AX200 runs the AirDrop radio. Other adapters
may also disconnect. Owly attempts restoration, but this preview does not yet
reuse Owly CLI's user-session reconnection helper. KWallet-backed connections may
need manual reconnection using **Open network settings**. An ended lease does not
prove Wi-Fi or internet connectivity was restored.

## Build and stage

Dependencies: CMake, C++20, Qt 6.6+ Core/DBus/Qml/Test, KF6 Config, Plasma 6 and
Kirigami runtime QML modules. A separately installed, compatible `owlyd` provides
`io.github.t4t5.Owly.Radio1` on the system bus and its Polkit policy. See
[the source investigation](docs/INVESTIGATION.md).

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j
ctest --test-dir build --output-on-failure
qmllint -I build -I /usr/lib/qt6/qml applet/contents/ui/main.qml
DESTDIR="$PWD/build/stage" cmake --install build
```

The QML import root defaults to `${CMAKE_INSTALL_LIBDIR}/qt6/qml`; override
`FEATHER_QML_INSTALL_DIR` for the target distribution. Staging does not modify the
running desktop. For a real package install, include the session executable,
session D-Bus activation file, QML module and Plasma package together. D-Bus starts
`feather-session` on demand. Add **Feather** in Plasma's widget chooser or System
Tray settings after installing the package. No login autostart is required.

## Lifecycle and settings

The session service owns an attached lease; closing the popup changes nothing.
Only its own successful `Acquire` result can be released. Foreign leases are
read-only. Daemon disappearance invalidates ownership; restart never reacquires.
All remote calls are asynchronous. A failed release keeps ownership until daemon
reconciliation proves that lease ended. `owlyd` enforces 10-minute, 30-minute or
unlimited duration. The displayed deadline is approximate and starts after
successful acquisition. A two-second status refresh reconciles after suspend.
Changing duration applies to the next activation.

The session service enforces the warning: `Enable` emits `WarningRequired`
without acquiring unless the user previously suppressed it. `ConfirmEnable` is
the explicit confirmation action; only this action can persist suppression.
**Show activation warning again** resets it. Preferences use KConfig's
`featherrc`, group `Radio`. No NetworkManager or BLE logic is duplicated.

## Validation and release gate

`radio-test` uses a fake Radio1 on an isolated `dbus-run-session` bus, without
connecting to the real system daemon. It covers warning/cancellation,
suppression/reset, duration settings, duplicate activation, acquire denial,
failed release retaining ownership, foreign lease refusal, expiry and daemon
loss/reappearance. It also checks persisted settings. Run it on a host permitting
local sockets; a container denying `bind(2)` cannot start the private test bus.

This is **not hardware verified**. Before release, extract/reuse Owly's existing
user-session Wi-Fi recovery helper upstream and verify AX200/KWallet reconnection.
Manual acceptance must cover explicit off, expiry, service process crash,
suspend/resume, daemon restart, and actual Wi-Fi reconnection. Automated tests
must never enable the live radio. UI layout and tray behavior also require a real
Plasma session acceptance pass.

## Install on Arch Linux

A local-checkout [PKGBUILD](packaging/arch/PKGBUILD) is included. Install `owly` separately (a compatible source installation also works),
then run `cd packaging/arch && makepkg -si` to build, test and install Feather.
See [Arch package instructions](packaging/arch/README.md) for prerequisites,
tray setup and restricted-container test limitations.

## License

Feather's original code is licensed under the [MIT License](LICENSE).
Owly and the Qt/KDE dependencies retain their own licenses. Preserve upstream
copyright and license notices when incorporating third-party source code.

The tray icon shows a shrinking countdown ring and rounded-up minutes badge for
timed sessions; its tooltip shows an approximate `mm:ss` countdown. Unlimited
sessions use a steady active ring. The session duration is captured on activation;
changing the next timeout does not change the current lease or its progress ring.
Adding time and protecting active transfers require Owly lease renewal and transfer
integration; these are planned, not available in this radio-only preview.
