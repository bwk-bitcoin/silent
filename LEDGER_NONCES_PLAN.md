# Ledger nonce sessions

## Behavior

- Target app-bitcoin commit `81b77861ba456f513c379494b1d0eaa4eb224fae`.
- Identify a Ledger's persistent cache by its master fingerprint, across networks.
- `MAX_INPUT = 8`; each session covers every owned MuSig key expression at eight absolute input positions.
- `MAX_LEDGER_NONCE_SESSIONS = 8`; manual fetch accepts one through eight sessions.
- Generate random 32-byte caller session IDs through `MUSIG2_SESSION_ID`.
- Every successful fetch appends sessions to the application cache. Existing entries, including other accounts and networks, are never cleared or evicted by fetching.
- Persist a monotonic fetch-order counter and allocate the selected policy's available sessions oldest first. The manual quantity limit is per fetch, not a cache-size limit.
- Persist transaction allocation before publishing public nonces. Never return allocated sessions to the available queue.
- Use cached round-one nonces without requiring a connected device. If the account has no available session, fetch exactly one for that signing attempt.
- Restore the allocated signer's session ID on its private round-two payload.
- After successful round-two signing, fetch one replacement if connected only when the allocated session came from cache. A session fetched for this signing attempt is consumed without replacement. Replacement appends and its failure does not lose the signature.
- Manual fetch four followed by one cached signing leaves four after replacement. Starting empty fetches one, consumes it, and ends empty.
- A failed or cancelled fetch leaves previous sessions untouched. A batch is appended atomically only after all its commands succeed.
- Firmware session loss or eviction uses the existing typed restart path; it does not introduce application cache eviction rules.
- Keep allocated transaction bindings separate from the available cache.

## Changes

1. Reuse bwk's public descriptor updater, indexed descriptor traversal, MuSig slot helpers, and existing Ledger signing transport. No dependency patches or new APDU.
2. Add encrypted, locked, crash-durable Rust storage for available sessions, the fetch counter, and allocated transaction bindings. Serialize operations per fingerprint.
3. Add a Nonces section to key details with MuSig account rows, cached counts, and a quantity/progress modal. Use asynchronous device and storage operations.
4. Integrate cache-backed round one, on-demand fetching, session restoration, and replacement into key signing flows.
5. Review append-only policy queues, provenance, FIFO, restart, slot remapping, cancellation, failed fetching, and device errors. Follow the project's check-execution restrictions.

## Protocol details

- Enumerate the internal key first, then each leaf's key expressions in policy order, counting foreign and ordinary expressions before ownership filtering.
- Map synthetic nonce responses by input position and expression occurrence; never rely on PSBT map order.
- Rebuild actual aggregate keys and leaf hashes when injecting cached nonce bytes into the real transaction.
- Existing firmware supports one owned participant per MuSig expression. Report unsupported policies explicitly rather than silently omitting participants.
- Replace a cached session only after its successful round two, never after nonce publication.
- Recheck account availability under the fingerprint lock before on-demand fetching. If another fetch already supplied a session, use that cache entry instead.
- An on-demand session records its intended transaction binding while still remaining an ordinary available entry. Its allocation durably records `Cached` or `FetchedForSigning`; another transaction taking the entry treats it as cached.
- Active allocation retries and round two restore that original provenance from disk, independently of current cache counts.

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
