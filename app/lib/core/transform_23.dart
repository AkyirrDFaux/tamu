/// 2x3 affine matrix editor model (LED display Position / Offset keys).
///
/// The wire value is [u16 h, u16 w, a, b, tx, c, d, ty] = matrix
///   [ a  b  tx ]
///   [ c  d  ty ]
/// composed as  Translate(offsetX, offsetY) · Rotate(rotation) · Scale(scaleX,
/// scaleY) · XShear(tan(skew)), where the scale signs carry the mirror flags:
///   a =  sx·cos θ            b =  sx·cos θ·k + sy·sin θ
///   c = −sx·sin θ            d = −sx·sin θ·k + sy·cos θ      (k = tan(skew))
/// Positive rotation is counter-clockwise on the screen.
///
/// Decomposition (best-effort, assumes no shear) uses the sign of the
/// determinant to detect a reflection: an axis-aligned negative component maps to
/// Mirror X/Y directly, any other det<0 matrix is represented as Mirror X with the
/// matching angle.
library;

import 'dart:math' as math;

import 'types.dart' show numberFromBytes, numberToBytes;

class Transform23 {
  double offsetX = 0;
  double offsetY = 0;
  double rotation = 0; // degrees, positive = CCW
  double scaleX = 1;
  double scaleY = 1;
  double skew = 0; // degrees
  bool mirrorX = false;
  bool mirrorY = false;

  /// The six matrix cells (a, b, tx, c, d, ty).
  ///
  /// The renderer samples the geometry/texture mask forward, so a shape's centre lands at
  /// `-L^-1 * t` unless the stored translation is pre-rotated by the linear part `L`. Doing
  /// that keeps the centre at `-t` for any rotation (an unrotated transform is unchanged).
  List<double> toCells() {
    final rad = rotation * math.pi / 180;
    final k = math.tan(skew * math.pi / 180);
    final sx = scaleX * (mirrorX ? -1 : 1);
    final sy = scaleY * (mirrorY ? -1 : 1);
    final c = math.cos(rad);
    final s = math.sin(rad);
    final a = sx * c;
    final b = sx * c * k + sy * s;
    final cc = -sx * s;
    final d = -sx * s * k + sy * c;
    return [
      a,
      b,
      a * offsetX + b * offsetY,
      cc,
      d,
      cc * offsetX + d * offsetY,
    ];
  }

  /// 2x3 wire bytes (header + six Numbers).
  List<int> toMatrix() => [
        2, 0, 3, 0,
        for (final v in toCells()) ...numberToBytes(v),
      ];

  /// Decomposes a 2x3 wire value (or plain six cells via [cells]) into fields.
  void fromMatrix(List<int> bytes) {
    double m(int i) => bytes.length >= 4 + (i + 1) * 4
        ? numberFromBytes(bytes, 4 + i * 4)
        : (i == 0 || i == 4 ? 1 : 0);
    fromCells([m(0), m(1), m(2), m(3), m(4), m(5)]);
  }

  void fromCells(List<double> cells) {
    final a = cells[0], b = cells[1], c = cells[3], d = cells[4];
    final det = a * d - b * c;
    // Undo the writer's linear pre-rotation of the translation (see toCells).
    if (det.abs() < 1e-9) {
      offsetX = cells[2];
      offsetY = cells[5];
    } else {
      offsetX = (d * cells[2] - b * cells[5]) / det;
      offsetY = (a * cells[5] - c * cells[2]) / det;
    }
    scaleX = math.sqrt(a * a + c * c);
    scaleY = math.sqrt(b * b + d * d);
    skew = 0;
    mirrorX = false;
    mirrorY = false;

    if (b == 0 && c == 0) {
      // Axis-aligned: a negative component is a mirror, both negative is 180deg.
      if (a < 0 && d > 0) {
        mirrorX = true;
      } else if (a > 0 && d < 0) {
        mirrorY = true;
      }
      rotation = (a < 0 && d < 0) ? 180 : 0;
    } else if (det < 0) {
      // Reflection: represent as Mirror X with the matching angle
      // (a = -|sx|·cos, c = +|sx|·sin  ->  θ = atan2(c, -a)).
      mirrorX = true;
      rotation = math.atan2(c, -a) * 180 / math.pi;
    } else {
      rotation = math.atan2(-c, a) * 180 / math.pi;
    }
  }
}