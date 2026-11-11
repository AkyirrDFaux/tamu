/// The two window sizes a responsive layout is checked at, and the harness that pumps one.
///
/// Three layout suites carried their own copy of the sizes (the devices page, the restore plan and
/// the restore result) and two carried the pump as well; a copy that drifted would have measured a
/// different window than the next suite while both claimed the same gate.
///
/// The SIZES are evidence, not the rule. A layout decision is the structural statement in the
/// suite's own header - what the element does in each orientation - and these numbers only show it
/// holds on a phone held either way.
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/theme.dart';

/// A phone held upright.
const Size portraitSize = Size(360, 640);

/// The same phone held sideways.
const Size landscapeSize = Size(800, 360);

/// Both orientations, in the order the suites name them.
const List<Size> bothOrientations = [portraitSize, landscapeSize];

/// The size's name as the cards name it ("360x640").
String sizeName(Size size) => '${size.width.toInt()}x${size.height.toInt()}';

/// Sets the logical window size for one test and restores it afterwards.
void useWindow(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

/// Pumps [child] as the whole window: one layout frame, then a painted one.
///
/// `pumpAndSettle` is not usable in these suites - a page's autorefresh timer never settles.
Future<void> pumpAtSize(WidgetTester tester, Size size, Widget child) async {
  useWindow(tester, size);
  await tester.pumpWidget(MaterialApp(theme: buildTheme(), home: child));
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 100));
}
