# Owly upstream proposal

The first patch adds radio lease renewal. It is based on Owly 0.4.2, commit
`0f2ca4087e6d938d823e76c856fcd04d0a0162b6`; the latest upstream revision could not
be fetched from the restricted development session. Rebase on current upstream
before opening a PR. This patch does not implement transfer tracking or grace-time
policy in Feather.

The prepared checkout is `/tmp/owly-upstream`, branch `radio-lease-renewal`.
A durable copy is `0001-radio-lease-renewal.patch`. Apply it with `git am` to an
Owly branch if the temporary checkout is gone. `PR.md` is the proposed PR body.

Run on a normal host before submission:

```sh
cd /tmp/owly-upstream
cargo fmt --all --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --locked -p owlyd
```

The development session passed formatting, package Clippy and the library test.
The contributor subsequently ran `cargo test --locked -p owlyd` on a normal host
and reported all 15 service integration tests passing. The branch has been pushed
to `obegron/owly`. Full upstream CI remains required.

From the prepared checkout, use your working GitHub CLI to create the fork:

```sh
gh repo fork --remote --remote-name origin
git fetch upstream
git rebase upstream/main
# Re-run checks after rebasing.
git push -u origin radio-lease-renewal
```

Do not create a PR until Feather receive/send and radio lifecycle have passed local hardware acceptance. Do not bundle
Qt, KDE, or Feather changes into it. Separate follow-up proposals should address
Owly's reusable session Wi-Fi restoration helper, incoming progress events and
coordinated per-transfer cancellation.
