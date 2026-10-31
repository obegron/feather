# Feather — AirDrop for Plasma

Implementation status: the 0.2 preview now includes the Rust adapter, native receive consent, incoming/outgoing KJobs, peer discovery, Dolphin service menu, and transfer-aware lease protection. See README.md and LOCAL-TRANSFER-TEST.md for current behavior and unverified acceptance checks. The radio-only sections below record the initial delivery plan.

Working name selected after the architecture: Feather. Descriptor: “AirDrop for Plasma, powered by Owly”. Package feather-plasma; independent of KDE itself. Name availability has not been established.

## Architecture and scope

1. Plasma 6 tray applet with native themed controls and a small C++ QML D-Bus bridge.
2. Qt 6 session service owns owlyd's attached system-bus lease and KConfig settings; exports a session D-Bus API. Owly owns radio, Bluetooth discovery, NetworkManager and recovery. No sudo subprocesses or radio scripts.
3. Later Dolphin service menu passes local URLs to a recipient picker in the session service. Purpose can follow.

First slice is explicitly radio control, not working file sharing. Never label radio-up as visible or ready to receive until the transfer receiver is running. Ship no pretend device list or transfer buttons.

## Verified radio contract

Local source: ../owly/crates/owlyd/src/api.rs and owly-cli/src/radio.rs. System name io.github.t4t5.Owly, path /io/github/t4t5/Owly, interface io.github.t4t5.Owly.Radio1. Acquire(u seconds) returns (t lease, a{sv} link); 0 means until disabled. Release(t) waits for recovery. StateChanged(t,s,s), State, Lease, Link, Peers, LastEnd exist. Probe() returns JSON. Attached lease expires on bus-owner loss, duration or link loss. Do not use root-only AcquireDetached. Peers are AWDL MAC/RSSI, not recipient identities.

Use asynchronous calls and daemon owner monitoring. Service states: unavailable, off, starting, on, stopping, error, externally owned. Only release our lease. Handle daemon restart, acquisition failure and lost link. Start countdown after successful acquisition; owlyd owns timeout enforcement. Use 600/1800/0 seconds. Changing duration affects next activation, clearly labeled. Failed release retains ownership information until reconciliation. Do not claim Wi-Fi restored solely from UI state; investigate backend guarantees and document limits.

## User experience

Applet title Feather, subtitle AirDrop radio. One Turn on / Turn off action, current state and countdown. During transition show activity and disable duplicate actions. Native colors, icons, focus navigation, accessible names, wrapping text, no fixed light palette. Show persistent “Wi-Fi is unavailable while AirDrop radio is on” reminder for this AX200 target. Warning before acquire: “Turn on AirDrop radio? Your Intel Wi-Fi connection will disconnect while it is on. Owly will attempt to restore it when you turn it off.” Cancel default; “Turn on” action; “Don't show again” persisted only upon confirmation. Provide reset preference. Conservative warning on unknown hardware. No radio activation on applet construction or login.

Radio slice includes explicit “File sharing is not available in this version.” Duration label “Turn off radio after”, not “Stay visible”. Missing service shows actionable install/start instructions, authorization failures allow retry, foreign lease explains another application is using it. Technical details are expandable, not the primary message.

## Transfer phase

Investigate actual Rust library callbacks before committing to API. Prefer a narrowly scoped Rust adapter using owly-airdrop behind the Qt service, with framed structured messages and transfer IDs; do not parse CLI prose. Adapter is private implementation, not a fourth desktop service. Owly CLI owns BlueZ glue currently; avoid reimplementing it and identify minimal upstream extraction if needed.

Qt creates one cancellable KJob per transfer, registered through KIO's job tracker, in both directions. Accurate aggregate bytes when supplied; indeterminate otherwise; never fake per-file progress. Native incoming consent shows sender, file names/count and destination; reject on dismissal/timeout, never auto-accept. Sender names are untrusted. Map denial, disconnect, cancellation and extraction failure distinctly. Completed files open through KIO. Discovery expiry stops accepting new work; decide transfer-aware lease policy before shipping transfers (no surprise mid-transfer deadline). While any send/receive job is active, disable ordinary radio-off in the UI and enforce the same guard in the session service. Offer explicit transfer cancellation through native jobs. Before transfers ship, extend owlyd with an owner-authorized lease renewal operation so both automatic transfer grace time and a manual add-time action update the real daemon deadline. Do not reset the radio or merely extend the displayed timer. Stop accepting new transfers at discovery expiry; retain the link until existing jobs finish, then release it. Bound grace time and crash cleanup must remain daemon-enforced. Applet shows summary and opens Plasma job UI rather than custom progress dialogs.

## Delivery and validation

First deliver buildable CMake Qt/KF6 service, QML bridge and packaged tray applet, activation metadata and build/install instructions. Independently packaged with Owly runtime dependency. Test with isolated D-Bus fake owlyd: default timeout, warning suppression/reset, failed acquisition, external lease, expiry, release and daemon loss. Compile against installed KDE headers, lint QML and stage installation. No live radio disruption in automated tests. Manual AX200 acceptance must cover timeout, process crash, suspend, daemon restart and Wi-Fi reconnection before calling the slice hardware verified.

Then consent/receive + native jobs, then Dolphin/send + jobs, then packaging/polish. No KDE, BlueDevil or KDE Connect patches.

## Critique revisions and release gates

This first slice is a developer radio preview. Reliable cleanup is not complete until Owly's CLI user-session restoration helper is extracted/reused upstream and AX200/KWallet reconnection passes. Do not duplicate its NetworkManager routines. Explain the limitation in the activation warning and provide “Open network settings” recovery action. Off means radio lease ended, not proof of restored connectivity.

Session service enforces warning confirmation: requesting enable cannot bypass a required warning; a separate confirmed activation records suppression only with explicit consent. Re-enable warning from applet settings. Reconcile state on owlyd owner change and never automatically reacquire. Display deadline as approximate, based on successful acquisition; after suspend re-read daemon state and treat its timeout as authoritative. Compact tray tooltip includes state and remaining time; active radio uses active tray status. Popup closure never changes the lease. Session service loss shows unavailable, never off.

## Omdrop reuse assessment

Reviewed https://github.com/netmojo-systems/omdrop-plugin and its core/Cargo.toml (2026-10-31). It has separate identity, protocol, receiver, sender, advertising, radio and CLI crates, with Omarchy QML and backend hooks. These are useful prior art, but integrating the complete application would bring a second protocol implementation and platform-specific lifecycle. Prefer Owly's public crates; evaluate individual MIT-licensed consent/identity ideas when transfers begin. No claim that Omdrop provides a ready-made KDE/KJob bridge. Unknown adapter warning says Wi-Fi may disconnect; the AX200-specific warning says it will.
