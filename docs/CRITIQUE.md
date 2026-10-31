# Independent plan critique

Reviewed 2026-10-31 by the requested critique agent.

Initial UX score: **7.5/10**. Revised plan UX score: **8.5/10**. Architecture: **8.5/10**.

The revised score evaluates the design of a developer radio preview, not a tested product. It is conditional on preserving the release gates in PLAN.md. No hardware or rendered UI validation has occurred as part of this critique.

## What improved

The first proposal omitted a concrete resolution for Owly's user-session Wi-Fi restoration workaround. `../owly/crates/owly-cli/src/wifi.rs` explains that root reconnection can fail for secrets stored in KDE KWallet. A direct Qt Radio1 client therefore cannot promise equivalent reconnection behavior.

The revised plan explicitly holds reliable cleanup and production readiness behind upstream helper reuse and AX200/KWallet testing. It provides a network settings recovery action and distinguishes the end of a radio lease from restored connectivity. It also requires service-enforced warning consent, daemon reconciliation, no automatic reacquisition, approximate countdown semantics, compact tray status, and correct behavior when the popup or session service disappears.

Native themed controls, asynchronous state transitions, attached leases, persistent warning reset, accurate scope labels, and the refusal to invent device identities or progress all support a strong Plasma experience.

## Implementation acceptance criteria

- Activation warning describes the reconnection limitation; cancelling does not acquire or persist suppression.
- The service enforces confirmation whenever warnings are enabled.
- The UI presents this as a radio preview and never claims it can receive files.
- Countdown and ownership reconcile after daemon changes and suspend; a failed stop does not silently discard ownership.
- Closing the popup leaves the selected lease running. Lost service is unavailable, not off.
- Network recovery is accessible without a terminal; the implementation does not copy Owly's NetworkManager recovery routines.
- Manual hardware tests remain required before any claim of reliable Wi-Fi restoration.

One remaining copy refinement: use definite Wi-Fi interruption language only for the known AX200/exclusive mode; unknown adapters should say Wi-Fi may disconnect. Keep “AirDrop for Plasma” in metadata so the working name Feather remains discoverable.
