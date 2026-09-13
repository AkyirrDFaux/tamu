# Issues

- **App tests reference a stale debug probe** (`test/debug_register_cache.dart`): pre-existing warnings (unused imports, `DataType` vs `int` equality, `avoid_print`). Revisit/delete when the app's register cache debug test is cleaned up.
- **DAS flash is near its budget again** (12 252 / 16 128 B for `0x3F00`): the re-enabled subscription provider cost ~1 KB. Acceptable for now; revisit if the DAS feature set grows.

## Solved

- ~~DAS subscription service was compiled out + not routed (the Dispatcher guard dropped the `Subscriptions` case for the DAS)~~ — restored routing via `#if defined(USE_SUB_PROVIDE) || defined(USE_SUB_REQUEST)`.
- ~~Value-update TRID mismatch between the requester and the provider~~ — the requester's TRID now comes from the frame (shared with the provider registration), matching the app's 8-bit transaction ID.
- ~~App parsed the DAS provider entries as 24 B though the wire entry is 28 B (lastSent + hash)~~ — the parse length is 28.