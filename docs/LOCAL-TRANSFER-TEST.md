# Local transfer acceptance

These are required checks on the target Arch/Plasma/AX200 machine, not results
claimed by automated tests.

1. Stop the manually started `owly receive` process. Install the locally patched
   Owly daemon (`just install` in the fork checkout, with the radio off), build
   and install Feather with `makepkg -fsi`, and restart the session service while
   off (`pkill -x feather-session`). Open the applet without restarting Plasma.
2. Enable a five-minute session. Confirm the Wi-Fi interruption warning, check
   radio State is up and the subtitle becomes Ready to receive. Confirm the
   computer appears in the unlocked phone's share sheet. If it does not, inspect
   the applet's transfer error and firewall rules for TCP 8771 on awdl0. Feather
   deliberately does not change the firewall automatically.
3. Send a small file from the phone. Decline once, then accept another request.
   Closing/dismissing or allowing the prompt to time out must decline. Accepted
   transfer must appear as a Plasma job and in Feather's summary; check the file
   in Downloads and use Open folder.
4. Send multiple files from the phone. Check names/count in consent and byte
   progress. Cancel a larger transfer from Plasma's job UI and verify cleanup.
   A transfer already committing files can complete normally after Cancel.
5. Open Send files, choose local files and a discovered device, accept on the
   phone. Check native progress and successful delivery. Repeat through Dolphin's
   Share → Send with Feather action. Verify there is no duplicate Actions entry. Reject on the phone and inspect failure.
6. While a transfer is active, verify the off switch is disabled and the D-Bus
   Disable method cannot stop it. Add time at the same time as a heartbeat; the
   transfer must continue. Let discovery expire during a long transfer: stop
   advertising/new offers, retain the active job, and release after it completes.
7. Verify +5/+10/+30 keep the same lease and interface and adjust ring/tooltip.
   Until off removes timed controls. Expiry with no transfers must restore Wi-Fi.
8. Kill the helper during a transfer, restart owlyd and disconnect a peer. Each
   affected job must finish with an error and clear its active state; no automatic
   reacquisition. Retry by turning Feather off/on after cleanup.
9. Test suspend/resume, process crash and the KWallet-stored Wi-Fi secret case.
   Radio-down does not prove Wi-Fi/internet connectivity restored. If necessary,
   reconnect from Plasma Networks and report the exact case before release.

Feather observes only its own helper's transfers. A separate CLI receiver's jobs
will not appear in the applet or receive Feather's lifetime protection.

## Compact sharing flow

- Right-click Feather for Send files and Receive files. Drop a local file onto
  the tray icon; the drag must use Copy, never remove the original.
  For the popup, copy files in Dolphin, open Feather and use Paste files or Ctrl+V.
  Clipboard content and original files must remain unchanged; text, folders and
  remote URLs must not start a share.
- With radio off and the Wi-Fi warning enabled, cancel the warning once: no lease
  may be acquired and the warning preference must remain unchanged. Confirm the
  next attempt; it must acquire an unlimited lease without changing the ordinary
  five/ten-minute timeout preference.
- Start a five-minute session, then share through Dolphin, drop onto the tray or
  paste files. The recipient picker must open using the same lease and helper,
  without renewing the lease or changing the discovery countdown to unlimited.
  Send a file: transfer protection may renew the lease with a finite grace period,
  but the discovery countdown must stay unchanged. Repeat Receive files during a
  timed session; it must also preserve the countdown.
- Select a recipient, tick Remember this device and send. On a later share the
  rediscovered device should be selected, but Send must still be required. Untick
  the option or use Forget saved device to remove the preference. An absent device
  must not cause an automatic send or a match based only on its display name.
