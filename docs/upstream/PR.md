A client holding a timed radio lease currently has to release and reacquire it to change the timeout. That interrupts the link, including any transfer still using it.

Add `Renew(t lease, u duration)` to Radio1 and its Rust proxy. It resets the timeout from the call's handling time without changing the lease number or restarting the backend; zero removes the timeout, as with Acquire. It uses Release's ownership checks, including the existing root exception for detached leases. An already expired lease cannot be revived. Owner-loss and link-loss cleanup remain unchanged.

Fake-radio D-Bus tests cover renewing past the original deadline, expiry at the new deadline, finite/unlimited transitions, wrong-owner and stale-lease rejection, detached leases, and cleanup when the owner disconnects.

Validation: `cargo fmt --all --check`, `cargo clippy --offline --locked -p owlyd --all-targets -- -D warnings`, and the library test passed in development. The contributor also ran `cargo test --locked -p owlyd` on a normal host: all 15 service integration tests passed, with no failures; doc tests passed. Full workspace CI remains to be run.
