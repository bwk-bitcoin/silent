# Ledger nonce sessions

## Behavior

- Target app-bitcoin commit `81b77861ba456f513c379494b1d0eaa4eb224fae`.
- Identify a Ledger's persistent cache by its master fingerprint, across networks.
- `MAX_INPUT = 8`; each session covers every owned MuSig key expression at eight absolute input positions.
- `MAX_LEDGER_NONCE_SESSIONS = 8`; manual fetch accepts one through eight sessions.
- Generate random 32-byte caller session IDs through `MUSIG2_SESSION_ID`.
- Any fetch replaces all available sessions for the fingerprint, including other accounts and networks.
- Persist a monotonic fetch-order counter and allocate available sessions oldest first.
- Persist transaction allocation before publishing public nonces. Never return allocated sessions to the available queue.
- Use cached round-one nonces without requiring a connected device. Without available nonces, fetch for signing.
- Restore the allocated signer's session ID on its private round-two payload.
- After successful device signing, fetch one replacement if connected. This discards remaining available sessions; replacement failure does not lose the signature.
- Keep allocated transaction bindings separate from the available cache.

## Changes

1. Reuse bwk's public descriptor updater, indexed descriptor traversal, MuSig slot helpers, and existing Ledger signing transport. No dependency patches or new APDU.
2. Add encrypted, locked, crash-durable Rust storage for available sessions, the fetch counter, and allocated transaction bindings. Serialize operations per fingerprint.
3. Add a Nonces section to key details with MuSig account rows, cached counts, and a quantity/progress modal. Use asynchronous device and storage operations.
4. Integrate cache-backed round one, on-demand fetching, session restoration, and replacement into key signing flows.
5. Review FIFO, restart, invalidation, slot remapping, cancellation, failed fetching, and device errors. Follow the project's check-execution restrictions.

## Protocol details

- Enumerate the internal key first, then each leaf's key expressions in policy order, counting foreign and ordinary expressions before ownership filtering.
- Map synthetic nonce responses by input position and expression occurrence; never rely on PSBT map order.
- Rebuild actual aggregate keys and leaf hashes when injecting cached nonce bytes into the real transaction.
- Existing firmware supports one owned participant per MuSig expression. Report unsupported policies explicitly rather than silently omitting participants.
- Replenish after round two, not after round-one publication, to avoid evicting the session needed for signing.

## Allocation lifecycle and restart

- Persist active, consumed, and invalid allocation states. Success consumes only the allocation whose transaction and published own nonces match the signed request. Device session loss invalidates that same attempt.
- Explicit restart retires the old transaction attempt for every Ledger fingerprint in its descriptor before returning a nonce-free PSBT with a fresh application attempt ID. Durable retirement also fences requests from the old attempt that have not allocated yet.
- Initial account preparation also creates a fresh application attempt ID, so preparing the same unsigned transaction again cannot reuse an earlier session.
- Keep retries within an active attempt idempotent. Never revive a consumed or invalid attempt, and never return its session to availability.
- Report session loss separately from invalid input and storage failures, including missing allocations and changed peer nonce transcripts.
- Compute offline round-one capability from compatible available sessions or an active allocation for this transaction. Available counts exclude allocations. Compute completion across every requested registered policy.
- The account signer modal loads nonce snapshots asynchronously and uses the existing signing plan through transaction-aware key entries. Offline cache capability never enables round two.
- Merge and consume successful device results on a worker. Restart also runs on a worker and blocks signing until retirement succeeds; late answers cannot change the restarted transaction.
- A failed retirement exposes Try Again without requiring a connected signing device. Ordinary signing and broadcast stay blocked until retirement succeeds.
- Manual fetch uses a validated numeric input from one through `MAX_LEDGER_NONCE_SESSIONS`.
