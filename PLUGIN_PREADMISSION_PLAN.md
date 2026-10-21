# Plugin metadata candidate counting

- Leave the plugin SDK, ABI 4 IID, registry cast guard and unrelated changes intact.
- Replace raw marker counts with structurally validated candidates: ELF note
  name/type/descriptor bounds, or a regular magic header followed by Qt metadata.
- Parse each candidate's CBOR and root IID independently of the expected host IID.
  Skip validated payload ranges so marker text inside descriptions is only data.
  Reject independent duplicate records and malformed recognized envelopes.
- Keep the existing Qt metadata/version guards and non-executing static-Qt path.
  Supported host and plugin targets are little-endian; do not add cross-endian parsing.
- Add regression sources for marker strings in JSON metadata descriptions,
  unrelated invalid note-like strings, duplicate records and ABI 3/4 mixtures.
- Inspect existing Nix library metadata read-only. Check formatting and git diff
  only; do not build, run tests or trigger CI. Commit a follow-up on musig.

Format references: qt/qtbase tag v6.6.3, src/corelib/plugin/qplugin.h
(QPluginMetaData, QPluginMetaDataV2), qplugin_p.h (root IID key 2), and
qpluginloader.cpp (QT_SHARED-only setFileName metadata discovery).
