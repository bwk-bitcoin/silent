# Plugin loader preadmission

- Leave the plugin SDK, ABI 4 IID, registry cast guard and unrelated changes intact.
- Before either platform loader executes library code, read embedded Qt metadata
  from the supplied bytes. Do not use sidecars or QPluginLoader metadata discovery,
  which is disabled by Qt 6.6.3 in static builds.
- Recognize Qt's ELF note and regular magic header, validate bounds and metadata
  version, parse CBOR with public Qt APIs, and require exactly one root IID matching
  SILENT_IPlugin_IID. Reject missing, malformed and duplicate metadata.
- Extend existing ABI fixtures with library initializer and constructor sentinels.
  Add missing/malformed metadata cases and a helper-level ABI 4 versus expected
  ABI 3 check without changing the public host API.
- Review source and run git diff --check only. Do not build, run tests or trigger CI.
  Commit only this task's files on musig.

Format references: qt/qtbase tag v6.6.3, src/corelib/plugin/qplugin.h
(QPluginMetaData, QPluginMetaDataV2), qplugin_p.h (root IID key 2), and
qpluginloader.cpp (QT_SHARED-only setFileName metadata discovery).
