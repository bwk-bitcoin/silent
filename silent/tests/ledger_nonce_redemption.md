# Ledger nonce-session redemption tests

These ignored integration tests communicate with the real Bitcoin application in
Speculos over TCP, using the pinned bwk client. No Bitcoin node is needed. The
synthetic regtest-style coins only need valid witness UTXOs and descriptor metadata.
Every successful redemption is checked by bwk's public finalizer, which verifies
each participant's partial signature and the resulting Schnorr signature.

## Firmware and simulator

Use app-bitcoin commit
`81b77861ba456f513c379494b1d0eaa4eb224fae` (`musig2_improvements`).
The exact CI binary is `nanos2/bin/app.elf` from the
`compiled_app_binaries` artifact of
[LedgerHQ/app-bitcoin Actions run 39282263854](https://github.com/LedgerHQ/app-bitcoin/actions/runs/39282263854).
The older 2.4.6 fixture does not implement the external session ID used here.

Download the artifact into an absolute directory of your choice:

```sh
export FIXTURE_DIR=/absolute/path/to/ledger-nonce-fixture
mkdir -p "$FIXTURE_DIR"
gh run download 39282263854 --repo LedgerHQ/app-bitcoin --name compiled_app_binaries --dir "$FIXTURE_DIR"
export SPECULOS_ELF="$FIXTURE_DIR/nanos2/bin/app.elf"
```

Start the pinned Speculos image with model `nanosp` and the seed whose master
fingerprint the tests assert is `b8688df1`:

```sh
docker run --rm --name ledger-nonce-speculos \
  -p 127.0.0.1:9999:9999 -p 127.0.0.1:5000:5000 \
  -v "$SPECULOS_ELF:/app/app.elf:ro" \
  ghcr.io/ledgerhq/speculos@sha256:028d1e368244631578537e7ca50b6f13ca2932b5f3fd220627289b559c07a886 \
  --model nanosp --display headless --api-port 5000 --apdu-port 9999 \
  --seed 'legal winner thank year wave sausage worth useful legal winner thank yellow' \
  /app/app.elf
```

In another terminal at the repository root, approve registration and transaction
prompts with the existing clicker (set `FIXTURE_DIR` there too):

```sh
python3 -I tests/e2e/devices/drivers/ledger_clicker.py --api http://127.0.0.1:5000 --log "$FIXTURE_DIR/ledger-screens.log"
```

The clicker approves every review on this simulator. Stop it after the tests.
Run the tests serially with no other client using that device. The tests connect
to APDU TCP port `127.0.0.1:9999` by default; `LEDGER_SPECULOS_ADDR` overrides it.

From the repository root:

```sh
LEDGER_SPECULOS_ADDR=127.0.0.1:9999 cargo test --manifest-path silent/Cargo.toml --test ledger_nonce_redemption -- --ignored --test-threads=1 --nocapture
```

Compile without contacting the device:

```sh
cargo test --manifest-path silent/Cargo.toml --no-run --test ledger_nonce_redemption
```

## Cases and assertions

Each sequence uses a fresh random 32-byte global proprietary `MUSIG2_SESSION_ID`
(subtype 0, empty keydata). It registers one policy with three Ledger-owned
expressions: one key-path MuSig aggregate and two aggregates in an `and_v` leaf,
with separate role keys and derivation pairs. Every round-1 and signing request
includes all three owned expressions for every requested input, using this same
registered policy.

- Repeat the identical round-2 PSBT after a successful redemption. Expect the
  firmware's `SW_BAD_STATE`, exposed by bwk as exactly `Error::MusigSession`.
- Generate round 1 again with the same external ID. Assert different public
  nonces, then verify another successful redemption and another refused replay.
- Change an output amount after a successful redemption. Expect the same refusal.
- Test all nine ordered pairs of first/replay input counts from `1`, `2`, `8`.
  Prefetch the larger count before the first redemption, including unused positions
  when the first request is smaller.

Before any replay assertion, the first command must return exactly the expected
Ledger partial-signature count and pass cryptographic verification with the hot
cosigner. Every rejected replay must leave the entire PSBT unchanged and contain
no partial signatures. Failures before first-redemption success are fixture or
protocol failures, not evidence that replay was prevented. The high-level client
does not expose the firmware's extended error code in `Error::MusigSession`.

## Observed result

All three ignored tests passed in 417.40 seconds against app-bitcoin commit
`81b77861ba456f513c379494b1d0eaa4eb224fae` using the pinned Speculos image.
Every signing request included all three owned expressions. Identical and changed
transaction replays, including all nine input-count pairs, were refused after
successful first redemption. Fresh round 1 with the same ID generated different
nonces and allowed signing again.

This agrees with
[`musigsession_round2_initialize` in the tested firmware](https://github.com/LedgerHQ/app-bitcoin/blob/81b77861ba456f513c379494b1d0eaa4eb224fae/src/musig/musig_sessions.c#L180-L193):
the first round-2 request pops the entire stored session, while subsequent inputs
and expressions within that command share the volatile round-2 state.
