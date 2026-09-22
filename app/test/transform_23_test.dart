@Tags(['transform'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/transform_23.dart';

/// Round-trip the transform fields through compose/decompose and compare cells.
bool nearList(List<double> a, List<double> b, [double eps = 0.001]) {
  if (a.length != b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if ((a[i] - b[i]).abs() > eps) return false;
  }
  return true;
}

void main() {
  test('identity round-trips', () {
    final t = Transform23();
    final cells = t.toCells();
    expect(nearList(cells, [1, 0, 0, 0, 1, 0]), isTrue);
    final t2 = Transform23()..fromCells(cells);
    expect(t2.rotation, 0);
    expect(t2.mirrorX, isFalse);
    expect(t2.mirrorY, isFalse);
    expect(nearList(t2.toCells(), cells), isTrue);
  });

  test('translation preserves cells', () {
    final t = Transform23()
      ..offsetX = 2
      ..offsetY = 3;
    final cells = t.toCells();
    expect(nearList(cells, [1, 0, 2, 0, 1, 3]), isTrue);
    expect(nearList((Transform23()..fromCells(cells)).toCells(), cells), isTrue);
  });

  test('positive rotation is CCW in cells', () {
    final t = Transform23()..rotation = 90;
    final c = t.toCells();
    expect(c[0], closeTo(0, 0.001)); // a = cos90
    expect(c[1], closeTo(1, 0.001)); // b = +sin90
    expect(c[3], closeTo(-1, 0.001)); // c = -sin90
    expect(c[4], closeTo(0, 0.001)); // d = cos90
    final t2 = Transform23()..fromCells(c);
    expect(t2.rotation, closeTo(90, 0.5));
  });

  test('axis-aligned X mirror detected', () {
    final t = Transform23()..fromCells([-1, 0, 0, 0, 1, 0]);
    expect(t.mirrorX, isTrue);
    expect(t.mirrorY, isFalse);
    expect(t.rotation, 0);
    expect(nearList(t.toCells(), [-1, 0, 0, 0, 1, 0]), isTrue);
  });

  test('axis-aligned Y mirror detected', () {
    final t = Transform23()..fromCells([1, 0, 0, 0, -1, 0]);
    expect(t.mirrorY, isTrue);
    expect(t.mirrorX, isFalse);
    expect(nearList(t.toCells(), [1, 0, 0, 0, -1, 0]), isTrue);
  });

test('compose/decompose round-trips for rotations and mirrors', () {
    for (final deg in [0.0, 30.0, -45.0, 90.0, 180.0]) {
      for (final sx in [1.0, 2.0, 0.5]) {
        for (final sy in [1.0, 3.0]) {
          for (final mx in [false, true]) {
            for (final my in [false, true]) {
              final t = Transform23()
                ..offsetX = -1.5
                ..offsetY = 2.5
                ..rotation = deg
                ..scaleX = sx
                ..scaleY = sy
                ..mirrorX = mx
                ..mirrorY = my;
              final cells = t.toCells();
              final t2 = Transform23()..fromCells(cells);
              expect(t2.offsetX, closeTo(-1.5, 0.01),
                  reason: 'offsetX deg=$deg s($sx,$sy) mirror($mx,$my)');
              expect(nearList(t2.toCells(), cells, 0.05), isTrue,
                  reason: 'cells round-trip deg=$deg s($sx,$sy) mirror($mx,$my)\n'
                      '  in:  $cells\n  out: ${t2.toCells()}');
            }
          }
        }
      }
    }
  });

  test('decomposition stays within a valid rotation for mirrored skew-free matrices',
      () {
    // A reflection about y=x is [0 1; 1 0] (det -1) -> represents as Mirror X @ 90deg.
    final t = Transform23()..fromCells([0, 1, 0, 1, 0, 0]);
    expect(t.mirrorX, isTrue);
    expect(t.rotation, closeTo(90, 1));
    expect(nearList(t.toCells(), [0, 1, 0, 1, 0, 0], 0.001), isTrue);
  });

  test('matrix wire round-trips through number encode', () {
    final t = Transform23()
      ..offsetX = 1
      ..rotation = 45
      ..scaleX = 2;
    final wire = t.toMatrix();
    expect(wire.length, 28);
    expect(wire[0], 2);
    expect(wire[2], 3);
    final t2 = Transform23()..fromMatrix(wire);
    expect(t2.rotation, closeTo(45, 0.1));
    expect(t2.scaleX, closeTo(2, 0.01));
    final c1 = t.toCells();
    final c2 = t2.toCells();
    expect(c1[0] / c2[0], closeTo(1, 0.005));
  });
}