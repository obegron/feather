# Owly source investigation

Inspected the local Owly source checkout at commit `0f2ca4087e6d938d823e76c856fcd04d0a0162b6`, including its `AGENTS.md`. Paths below are relative to that checkout. Findings describe this revision, not hypothetical APIs.

## Radio lifecycle: usable now

`crates/owlyd/src/api.rs` exposes the system D-Bus service `io.github.t4t5.Owly`, object `/io/github/t4t5/Owly`, interface `io.github.t4t5.Owly.Radio1`:

| API | Behavior |
| --- | --- |
| `Probe() → s` | Read-only JSON capability report; no authorization required. |
| `Acquire(u seconds) → (t lease, a{sv} link)` | Polkit-authorized acquisition; zero means until released. |
| `Release(t lease)` | Releases the caller's lease and waits for radio teardown. |
| `StateChanged(t lease, s state, s reason)` | Lifecycle notification. |
| `State`, `Lease`, `Link`, `Peers`, `LastEnd` | Observable state, active lease/link, AWDL peers, last termination reason. |

States are `idle`, `starting`, `up`, `stopping`. Link fields include interface, index, MAC, IPv6 address and optional mDNS announcement timing. Errors include Busy, NotNow, NoRadio, StaleLease and NotAuthorized. The polkit action is `io.github.t4t5.Owly.acquire`.

A lease ends on expiry, explicit release, owner bus disconnect or link loss. Do not use the root-only detached lease API. `crates/owlyd/src/service.rs` recovers the backend before serving and handles owner disappearance and shutdown. The frontend must preserve this ownership model, track its own lease separately from global state, and never automatically reacquire after a restart.

## Wi-Fi restoration blocker

`crates/owly-radio/src/sys.rs` contains NetworkManager restoration primitives, and the radio journal records changes for recovery. However, `crates/owly-cli/src/wifi.rs` explicitly documents an additional KDE-relevant problem: root's reconnection may fail when credentials belong to a user secret agent such as KWallet.

The CLI snapshots active Wi-Fi UUID/device pairs before acquisition and retries missing connections as the user after lease termination. `crates/owly-cli/src/radio.rs` invokes this helper. It is private binary code, not an existing public session API.

Consequently, a direct Qt Radio1 client does not reproduce the complete CLI recovery path. Do not claim that successful Release proves internet connectivity restored. Do not duplicate NetworkManager recovery in Qt. Propose a minimal upstream extraction of the existing user-session recovery helper into a reusable Owly library/helper API, including a reconnect result. Until integrated, AX200/KWallet restoration is a hardware acceptance blocker, explicitly documented rather than hidden.

## Discovery and usable libraries

`crates/owly-airdrop/src/lib.rs` exports a public sans-I/O protocol core. The `tokio` feature exposes driver APIs and the `mdns` feature enables discovery. `owlyd` also exports its API as a library. These are Rust interfaces; no C ABI or desktop transfer D-Bus API was found.

`crates/owly-airdrop/src/driver/mdns.rs` provides `Mdns::new(scope_id)`, `browse()` with a browse-event receiver, and service registration. `driver/sender.rs` provides `Sender::discover(SocketAddrV6) → io::Result<Option<Receiver>>` for receiver validation and identity. Owlyd's `Peers` property contains AWDL MAC/RSSI records, not named AirDrop recipients.

`crates/owly-cli/src/airdrop.rs` combines BLE advertising, mDNS and receiver discovery. Its output is human-oriented text. BLE integration lives in CLI code and should be extracted/reused through Owly, not independently reimplemented. Holding a radio lease alone does not advertise a receiving AirDrop service.

## Consent, progress and cancellation

`crates/owly-airdrop/src/driver/receiver.rs` exposes `Request { id, peer, ask }` with consuming `accept()` and `decline()` methods. Dropping a request declines it. Events are Discovered, Request, Withdrawn, Urls, Received and Failed. Withdrawn covers disappearance or timeout and must dismiss pending native prompts. Receiver binding and running are public APIs.

There are no incoming byte-progress events and no per-transfer public receiver cancellation handle. The server spawns connection tasks; stopping the listening task alone is not a sufficient coordinated shutdown contract.

`crates/owly-airdrop/src/driver/sender.rs` exposes `Sender::send(addr, payload, receiver_flags, events)` returning `io::Result<Outcome>`. Events report acceptance and compressed archive bytes sent/total. This is aggregate wire-body progress, not per-file or unpacked-byte progress. Archive preparation runs in a blocking task. Cancelling a network future does not establish cancellation of that blocking work.

Minimal upstream enhancements needed for complete desktop transfers:

- Incoming upload-byte progress with an optional truthful total.
- Per-transfer cancellation handles and coordinated task shutdown.
- Explicit cancellation during archive preparation, and structured terminal outcomes.
- Shared Owly session/BLE orchestration instead of copying private CLI code.
- Per-file archive/extraction callbacks only if actual per-file reporting is required; aggregate KJob progress must not invent them.

## Adapter decision and vertical slice

Use a Qt session service as the Radio1 lease client, owning persistent preferences and warning policy. A small native Qt QML client exposes that service to the Plasma applet. The daemon duration provides cleanup even if the UI disappears. Show global versus locally owned sessions distinctly and treat daemon loss as unknown status, not verified off.

For later transfers, use a narrowly scoped Rust helper linked against pinned Owly crates, communicating through framed structured IPC. Qt owns KJob, KNotifications and the KDE-facing API; Owly owns transport, discovery and radio management. Avoid parsing human-readable CLI progress. Incoming KJob initially needs indeterminate progress until the upstream callback exists; never fabricate percentages.

Validate acquisition/release, failures, duration, concurrent requests, foreign leases, backend disappearance, user-session disappearance and preference persistence against a mock D-Bus service. Hardware validation must separately cover exclusive-mode Wi-Fi interruption, actual recovery, timeout and KWallet credentials.

## Existing omdrop infrastructure

The local tree includes `crates/omdrop-owl`, legacy Python helpers and discoverability hooks. `AGENTS.md` identifies the legacy implementation as `omdrop-owl` 0.3.0 and older. These are not evidence of a reusable KDE frontend. The external omdrop-plugin repository was not examined in this source investigation; its reuse and licensing require a separate review.
