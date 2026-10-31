# Arch Linux package

The `packaging/arch/PKGBUILD` packages this local checkout as `feather-plasma`. It copies
source inputs into makepkg's build directory; no Git remote or release archive
is required. This recipe is for local installation, not AUR publication.

Install Arch's build tools first if needed:

```sh
sudo pacman -S --needed base-devel
```

Owly is required for radio operation but is an optional package dependency so
a source-installed `owlyd` is supported. If needed, install the separate `owly`
package (`yay -S owly`). `makepkg -s` cannot fetch AUR dependencies.
A compatible source installation must include owlyd’s system D-Bus configuration
and Polkit policy, not just its executable.
Then run from the Feather checkout:

```sh
cd packaging/arch
makepkg -fsi
```

The check phase runs lifecycle tests on a private D-Bus without touching the live
radio. Build on a normal host permitting local sockets. To deliberately package
without executing tests in a restricted container, use `makepkg --nocheck -si`.
This skips runtime validation and does not establish hardware compatibility.

The package installs the session executable, D-Bus activation entry, native QML
module, Rust transfer helper, notifications, Dolphin service menu and tray applet together under `/usr`. After installation, enable Feather
in System Tray settings, or add it through Plasma's widget chooser. If the
current Plasma process does not discover the new module, log out and back in.
Owly's system service must be running (see `systemctl status owlyd`); start it
with `sudo systemctl start owlyd` if needed. The frontend never enables the radio
at login. Its session service starts on demand through D-Bus.

This is a transfer preview. Native receive/send and Dolphin sharing are included;
AX200/KWallet Wi-Fi reconnection still requires manual acceptance testing. Use Plasma
Networks if Wi-Fi fails to reconnect after stopping the radio.

Remove the frontend with `sudo pacman -R feather-plasma` after turning its radio
off. Owly is independently packaged and managed.

The recipe disables separate debug packages. If an earlier attempt installed only
`feather-plasma-debug`, remove it with `sudo pacman -R feather-plasma-debug`.
Rebuild with `makepkg -fsi` after recipe changes so an old archive is not reused.

Timed transfers require the locally patched Owly daemon with `Radio1.Renew`;
Until off supports transfer operation with an older daemon. Stop any separate
`owly receive` process before enabling Feather. The package check phase also runs
Rust protocol/library tests and a fake-helper Qt transfer pipeline test.
