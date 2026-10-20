# MuSig session caching for hot and hardware keys

## Shared behavior

- Persist sessions in one signer-neutral cache, keyed by signer, fingerprint,
  policy, and caller-supplied session ID. Hot and hardware routes sharing a key
  must not consume each other's nonce material.
- Each session covers eight absolute input positions and every owned MuSig key
  expression. Account rows keep the label `Musig sessions cache` and the `Set`
  action, including while a hardware device is disconnected.
- Generate random 32-byte session IDs. Creating an existing ID generates fresh
  nonce material and replaces that session, invalidating its old allocations.
  This matches Ledger round-one replacement; it is not an idempotent lookup.
- FIFO selects an available session. Its order comes from a durable monotonic
  counter, separate from its session ID.
- Different IDs append without clearing other accounts. The manual quantity
  bound remains eight sessions per request.
- At the nonce step, use an available session or create one for the current
  signing attempt. Persist allocation before publishing public nonces.
- At the signing step, consume the whole session. Only cached-origin sessions
  receive one replacement after successful signing. A session created on demand
  is consumed without replacement. Replacement failure preserves the signature.
- Keep active, consumed, and invalid attempts distinct. Restarts never return
  allocated material to the available queue.

## bwk API

1. Add transaction-independent hot session generation with explicit session IDs,
   a fresh random root, descriptor and signer binding, and bounded input coverage.
2. Support explicit secret-state export and validated restoration for encrypted
   application persistence. Secret-bearing values are not printed or ordinarily
   cloned and erase their secret material when dropped.
3. Expose nonce publication and consuming signing operations through the hot
   signer. Keep ownership checks, slot mapping, sighashes, key aggregation, and
   partial-signature generation in bwk, rather than copying them into Silent.
4. Make the shared indexed-slot metadata available where both hot and Ledger
   adapters need it. Preserve expression occurrence indexes across spend paths.
5. Keep persistence ownership in the application: it durably burns secret state
   before invoking the consuming signing operation or releasing signatures.

## Silent integration

1. Pin bwk dependencies to published commit
   `1af01b30cc813cf9f231d1b2c26d17981372fab3` on `signing_rework`, keeping
   Cargo and Nix source revisions consistent.
2. Separate generic storage and allocation from the Ledger protocol adapter.
   Persist hot secret payloads inside the existing encrypted application state.
   Preserve previously stored Ledger sessions when moving to the common cache.
3. Expose the same account cache controls for hot keys. Generate hot sessions
   after the existing password prompt, with blocking work on worker threads.
4. Publish cached hot nonces without asking for the key password. Actual signing
   continues to require the password. Create on-demand sessions and cached-origin
   replacements while the signer is already unlocked for the requested action.
5. Preserve Ledger's caller-ID handling against app-bitcoin commit
   `81b77861ba456f513c379494b1d0eaa4eb224fae`.

## Verification and handoff

- Check same-ID replacement changes public nonces and prevents old allocations
  from signing. Different IDs and policies remain available independently.
- Check encrypted persistence, reopening between rounds, FIFO, wrong passwords,
  and refusal after consumption, replacement, or a signing failure.
- Verify cached-origin `4 -> 3 -> 4` and on-demand `0 -> 0 -> 0` flows for hot
  keys and preserve the Ledger equivalents.
- Run focused builds, tests, and lint checks needed to establish functionality.
- Commit bwk and Silent after review. Use the published bwk revision in Cargo and
  Nix so the application no longer depends on a sibling checkout.
