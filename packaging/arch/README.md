# Arch Linux package

The `packaging/arch/PKGBUILD` packages this local checkout as `feather-plasma`. It copies
source inputs into makepkg's build directory; no Git remote or release archive
is required. This recipe is for local installation, not AUR publication.

Install Arch's build tools first if needed:

```sh
sudo pacman -S --needed base-devel
```

Install the separate `owly` package first (Owly documents `yay -S owly` for Arch).
`makepkg -s` installs repository dependencies but cannot fetch AUR dependencies.
Then run from the Feather checkout:

```sh
cd packaging/arch
makepkg -si
```

The check phase runs lifecycle tests on a private D-Bus without touching the live
radio. Build on a normal host permitting local sockets. To deliberately package
without executing tests in a restricted container, use `makepkg --nocheck -si`.
This skips runtime validation and does not establish hardware compatibility.

The package installs the session executable, D-Bus activation entry, native QML
module and tray applet together under `/usr`. After installation, enable Feather
in System Tray settings, or add it through Plasma's widget chooser. If the
current Plasma process does not discover the new module, log out and back in.
Owly's system service must be running (see `systemctl status owlyd`); start it
with `sudo systemctl start owlyd` if needed. The frontend never enables the radio
at login. Its session service starts on demand through D-Bus.

This remains a radio-control developer preview. Transfers are not implemented;
AX200/KWallet Wi-Fi reconnection requires manual acceptance testing. Use Plasma
Networks if Wi-Fi fails to reconnect after stopping the radio.

Remove the frontend with `sudo pacman -R feather-plasma` after turning its radio
off. Owly is independently packaged and managed.
