# Issues

## DAS provider table has stale persisted entries (HIL test state)
- `hil_subscriptions_test.dart` "DAS provider table empty initially" fails when the DAS
  restores provider subscriptions persisted in its flash from an earlier session whose
  requester (Tamu) is gone.
- Unrelated to the LED display rewrite. Fix/cleanup: reflash the DAS (erases flash) or
  cancel the stale providers via a matching requester entry before running the suite.