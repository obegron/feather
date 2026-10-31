# Feather — AirDrop for Plasma

A native Plasma 6 tray applet powered by Owly. Enable the radio to receive files,
right-click the icon or popup for Send/Receive shortcuts, or drop files onto the tray icon.
Dolphin also provides **Send with Feather**.
Incoming offers ask for native Accept/Decline consent. Both directions appear in
Plasma's transfer job UI, where they can be cancelled. Files are saved to Downloads.

This is a development preview requiring local phone testing. URL offers and
folder sending are not supported. Sender/device names are unverified display names.

## Install on Arch

Install a compatible `owlyd` with its system D-Bus configuration and Polkit policy.
A source installation works; the Arch `owly` package is an optional package
dependency. Timed transfers and adding time require the local Owly fork's
`Radio1.Renew` method. Without it, use **Until off** for transfers.

```sh
cd packaging/arch
makepkg -fsi
```

See [Arch instructions](packaging/arch/README.md). Add Feather through System Tray
settings. No radio is enabled automatically at login. To open it separately:

```sh
plasmawindowed io.github.obegron.feather
```

Before upgrading, finish transfers, turn the radio off and close Feather. After
installing, run `pkill -x feather-session` and reopen it to load the new service.
**Stop any manually started `owly receive` process first**: both receivers use
TCP 8771 on the same AWDL interface. Feather does not observe external CLI transfers.

## Radio and transfers

Choose 5 or 10 minutes, or **∞** (Until off). While timed radio is active, add
5 or 10 minutes without restarting the link, or switch to **∞**. The tray ring
shrinks with the timer; hover shows the remaining time. Unlimited sessions have a
steady ring. A missing renewal acknowledgement marks the timer unknown rather
than inventing a deadline.

The off switch is disabled during incoming approval and active transfers. The
service enforces this too. Before accepting or sending, Feather obtains a finite
transfer grace period from Owly and refreshes it while work is active. When the
original discovery timeout expires it stops advertising and accepting new work,
lets current transfers finish, then releases the radio. If a transfer finishes
before the discovery deadline, the original deadline is restored. Cancellation
waits for cleanup; a final filesystem commit already underway finishes naturally.

Outgoing progress is compressed upload bytes with a total. Incoming progress is
actual extracted file bytes without a known total; native jobs show indeterminate
progress rather than a fabricated percentage. Completion offers **Open folder**.

On Intel AX200, radio operation interrupts normal Wi-Fi. Confirmation includes a
persistent Don't ask again preference, resettable under Settings. Owly attempts
restoration; the private CLI user-secret reconnect workaround is not yet reused.
KWallet-backed networks may need manual reconnection through Network settings.
Feather does not implement NetworkManager recovery or a Bluetooth stack.

## Build and test

Requires CMake, C++20, Rust/Cargo, Qt6 Core/DBus/Qml/Widgets/Test, KF6 Config, KIO,
JobWidgets and Notifications, plus Plasma and Kirigami runtime QML modules. Fetch
Rust dependencies for the host once, then build offline:

```sh
cargo fetch --manifest-path adapter/Cargo.toml --locked --target "$(rustc -vV | sed -n 's/^host: //p')"
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build -j
ctest --test-dir build --output-on-failure
qmllint -I build -I /usr/lib/qt6/qml applet/contents/ui/main.qml
DESTDIR="$PWD/build/stage" cmake --install build
```

D-Bus lifecycle and transfer-pipeline tests use a private bus and fake radio/helper;
they never enable live hardware. They require local socket creation. Rust adapter
and vendored-library tests also run from CTest. Hardware acceptance, native Plasma
rendering, actual firewall setup and AX200/KWallet reconnection remain to be checked
on a real host. See [local transfer acceptance](docs/LOCAL-TRANSFER-TEST.md).

The three desktop components are the Plasma applet, the Qt session service with
its private Rust transfer subprocess, and Dolphin's service menu. The adapter uses
Owly's public APIs and locally patched lifecycle callbacks rather than parsing CLI
output. See [adapter provenance, IPC and upstream patches](adapter/README.md).
No upstream PR will be created until local functionality is validated.

## License

Feather's original code is [MIT licensed](LICENSE). Vendored Owly code and BLE glue
retain their MIT notices; compiled Rust dependency notices are bundled in
`adapter/THIRD_PARTY_LICENSES`. Qt/KDE dependencies retain their own licenses.


Copy files in Dolphin, open Feather and choose **Paste files** to share from the
clipboard. The popup closes on focus changes, so it uses paste instead of a drop
target; dropping onto the tray icon remains available. Plain text, remote URLs and
folders are not accepted from the clipboard. The clipboard is left unchanged.

Dropping files or using a sharing shortcut enables the radio until turned off,
with Wi-Fi confirmation when required. It then searches for recipients and opens
a device picker. The **Remember this device** option only preselects a rediscovered
device; it never sends automatically or bypasses incoming consent. Forget the
selection in Settings. Device identifiers can change; if a saved device cannot be
matched, choose it again. Drop-to-share does not change the normal radio timeout
preference. Thirty-minute sessions remain accepted by the service for older clients
but are no longer offered in the compact UI.


The application, applet, QML module and session D-Bus service use
`io.github.obegron.feather`. Feather is an independent project, not an official
KDE application. On upgrading from the former `org.kde.feather` applet, finish
transfers, turn the radio off, remove the old tray widget, close Feather and stop
the old session service (`pkill -x feather-session`). After upgrading, add Feather
again from System Tray settings. KConfig preferences remain in `featherrc`.
