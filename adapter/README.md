# Feather transfer adapter

`feather-transfer` is a private JSON Lines subprocess of Feather's Qt session
service. Qt owns the attached radio lease, confirmation prompts, recipient
selection and KJobs. This helper uses Owly's TLS, AirDrop protocol, mDNS,
archive preparation and sandboxed extraction; it never parses CLI output or
acquires a radio lease itself.

Build with cached dependencies:

```sh
cargo build --manifest-path adapter/Cargo.toml --release --offline --locked
cargo test --manifest-path adapter/Cargo.toml --offline --locked
CARGO_TARGET_DIR="$PWD/build/rust-library-tests" cargo test \
  --manifest-path adapter/vendor/owly-airdrop/Cargo.toml --offline --features mdns --lib
```

CMake installs the binary at `/usr/lib/feather/feather-transfer`. Required arguments
are `--interface IFACE --name NAME --directory DIR`. Qt must pass the leased link's
IPv6 interface, a display name and an absolute local receive directory. If Radio1
Link contains `announce: (uuu)`, also pass `--announce-count N
--announce-interval-us N --announce-pause-us N`, preserving its exact timing.
Omitting all three uses normal mDNS announcement behavior. Optional `--bus ADDRESS`
redirects only read-only Radio1 peer discovery to a private test bus; the default
is the system bus. It never acquires/releases/renews the radio.

The receiver listens on link-local IPv6 port 8771, as Owly's CLI does. A firewall
must allow this port on the leased interface. The helper does not change firewall
rules. `ready` means the listener and local mDNS registration exist, not that a
remote device has discovered us. Sender BLE wakeup failure emits a `warning`;
ordinary mDNS discovery still runs. File transfers only: URL offers are declined.

## IPC

One JSON object per line on stdin/stdout. stdout contains protocol objects only;
all sender names, file names and error strings are untrusted data. Qt must escape
these when rendering. Incoming IDs are `in-N`; Qt chooses outgoing `out-*` IDs.

Commands:

- `{"command":"accept","id":"in-1"}` or `decline` after a native user decision.
- `{"command":"cancel","id":"in-1"}` or an outgoing ID.
- `{"command":"send","id":"out-1","peer":"SERVICE-ID","paths":["/absolute/file"]}`.
- `{"command":"browse"}` refreshes discovery and repeats currently listed peers.
- `pause`, `resume`, `shutdown` use only the `command` key.

Events:

- `ready`; `warning`/`fatal` with `reason`; `stopped` after orderly cleanup.
- `peer` with `id`, `name`, IPv6 `address` and numeric `flags`; `peer-lost` with `id`.
- `request` with `id`, `sender`, `files` (display names); `withdrawn` with `id`.
- `started` with `id`, `direction` (`incoming`/`outgoing`), `name`.
- `progress` with `id`, `done`, and optional `total`.
- `completed` with `id` and, for incoming files, `paths` of committed local items.
- `failed` with `id`, `reason`; `cancelled` with `id`.

Incoming progress counts actual uncompressed file bytes written into staging;
there is no total. Outgoing progress counts compressed upload body bytes with an
accurate compressed total. Neither represents per-file progress. Outgoing
`started` precedes archive preparation; incoming `started` follows explicit
acceptance. Terminal events are emitted once per ID and subsequent progress is
suppressed. A receiver denial/timeout is a `failed` event with a distinct reason.

`pause` awaits the local mDNS goodbye, declines outstanding prompts and rejects
new connections/requests; existing uploads continue. The BLE advert is Owly's
sender-wakeup advert, not a local receiver advertisement, and stays available for
outgoing discovery. `resume` republishes the local service.

Cancellation during archive preparation checks between archive entries and 64KiB
read chunks, then joins the blocking preparation task before reporting cancelled.
Receiver cancellation waits for its connection/staging cleanup. Once a completed
upload's final filesystem commit starts, cancellation is refused and its natural
`completed`/`failed` event follows. Completed paths are never reported until
staging finishes. Shutdown and stdin EOF decline prompts, cancel ongoing work,
join connection/sender/preparation tasks, await mDNS teardown and unregister BLE.
Network writes have a 30-second deadline; slow filesystem calls or BlueZ teardown
can delay shutdown further. Qt should await `stopped`/process exit before releasing
its transfer protection or starting another helper.

## Source provenance and local extensions

`vendor/owly-airdrop/src` and its MIT LICENSE are copied from
https://github.com/t4t5/owly, checkout
`a38a6fe9bde3e35ea9930c6f4f0df45e8a5c6a8a` (0.4.2), from the user's
`/tmp/owly-upstream` fork. Only the crate is vendored. Cargo workspace package
metadata is resolved locally; integration-test workspace dependencies are omitted.
`src/bluez.rs` is the same checkout's `crates/owly-cli/src/bluez.rs`, under that
MIT license, with one added awaited `Registration::close` method. Interface lookup,
service-ID generation and the Radio1 AWDL-peer fallback follow Owly CLI's
`airdrop.rs`; modified EUI-64 mapping follows `owly-awdl/src/lib.rs`. No independent Bluetooth
protocol implementation is introduced.

`patches/0001-desktop-transfer-lifecycle.patch` records the changes against that
Owly tree: opt-in incoming byte progress, receiver control/pause/cancellation,
joined connection shutdown, cancellable sender preparation and network operation,
and awaited mDNS registration/browser teardown. It includes CLI compatibility for
the added progress event. The patch is provided for upstream review; it has not
been committed, pushed or published by the adapter implementation.

The adapter's tests verify strict command decoding and structured process errors
with closed stdin and a nonexistent interface, without accessing the radio.
Regression tests also cover cancellation refusal preserving a previously delivered
commit failure and successful cancellation emitting one terminal outcome.
The vendored crate's unit tests cover archive safety, staging, protocol behavior,
cooperative archive cancellation and receiver cleanup/commit cancellation policy.
A running receiver's successful EOF shutdown and actual interoperability need a
host allowing network sockets and hardware acceptance; a failed interface smoke
test does not establish those behaviors. Discovery combines public mDNS with Owly CLI-equivalent read-only Radio1
AWDL-peer discovery. The latter verifies Link.interface, reads up to256 current
Peers, derives scoped IPv6 addresses and probes port8770 with `Sender::discover`
and a3-second deadline. At most8 peers are attempted per round, with4 concurrent
probes and500ms between rounds; larger sets rotate. A listing expires when its
AWDL peer disappears or `/Discover` is unconfirmed for15seconds. mDNS and AWDL
results sharing a scoped IPv6 address keep one stable recipient ID and are removed
when neither source remains. Mapping, scope isolation and dual-source expiry are
covered by unit tests.
