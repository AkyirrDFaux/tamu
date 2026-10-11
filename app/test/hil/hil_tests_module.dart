/// Module tests: one device and one module (Docs/HIL Tests.md).
///
/// A module is present when the device exposes its block: LED Display
/// (`vysiDisplay`), the fan (`pwm`), Buttons & LEDs (`button`/`led`/`ledButton`)
/// and Measurement (`accGyr`, `resistiveMeasure`). Every test is independent and
/// per-device; a test that drives an actuator writes the field back to the value
/// it found (and drops any scratch dynamic block it created).
library;

import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_api.dart';

// The blocks a module test needs; an unmet prerequisite is a skip.
const HilPrereq _display = HilPrereq(blocks: [BlockType.vysiDisplay]);
const HilPrereq _displayRender = HilPrereq(
    blocks: [BlockType.vysiDisplay],
    capabilities: [Capability.dynamicMemory]);
const HilPrereq _pwm = HilPrereq(blocks: [BlockType.pwm]);
const HilPrereq _button = HilPrereq(blocks: [BlockType.button]);
const HilPrereq _led = HilPrereq(blocks: [BlockType.led]);
const HilPrereq _ledButton = HilPrereq(blocks: [BlockType.ledButton]);
const HilPrereq _accGyr = HilPrereq(blocks: [BlockType.accGyr]);
const HilPrereq _resistive = HilPrereq(blocks: [BlockType.resistiveMeasure]);

/// A 32-bit little-endian word (the PWM/Index fields carry uint32).
List<int> _u32(int value) => intToBytes(value, 4);

/// A layout file name padded to the field's fixed 8 characters.
String _pad8(String name) => name.padRight(8, ' ').substring(0, 8);

final List<HilTest> moduleTests = <HilTest>[
  // ===========================================================================
  // LED Display (vysiDisplay)
  // ===========================================================================
  HilTest(
    'LED display reports its settings schema',
    category: HilCategory.module,
    prereq: _display,
    body: (ctx) async {
      for (final d in ctx.devices(_display)) {
        final reg = RegisterClient(deviceId: d.id);
        final fields =
            await reg.enumerateFieldIndexes(BlockType.vysiDisplay.value, 0);
        expect(fields, equals([0, 1, 2, 3, 4]),
            reason: '${d.label} display has the five documented fields');

        final brightness =
            await reg.readBlockField(BlockType.vysiDisplay.value, 0, 0, 0);
        expect(brightness, isNotNull);
        expect(brightness!.meta.dataType, DataType.number);
        expect(brightness.meta.readOnly, isFalse, reason: 'Brightness is writable');

        final offset =
            await reg.readBlockField(BlockType.vysiDisplay.value, 0, 1, 0);
        expect(offset!.meta.dataType, DataType.matrix,
            reason: 'Offset is the 2x3 transformation');
        expect(offset.meta.persistent, isTrue);

        final renderBlock =
            await reg.readBlockField(BlockType.vysiDisplay.value, 0, 2, 0);
        expect(renderBlock!.meta.dataType, DataType.integer,
            reason: 'Render Block Index is signed');
        expect(renderBlock.meta.persistent, isTrue);

        final layout =
            await reg.readBlockField(BlockType.vysiDisplay.value, 0, 3, 0);
        expect(layout!.meta.dataType, DataType.filename);
        expect(layout.meta.flags & ValueFlags.trigger, isNot(0),
            reason: 'the layout name is a trigger field');
        expect(layout.meta.persistent, isTrue);

        final rate =
            await reg.readBlockField(BlockType.vysiDisplay.value, 0, 4, 0);
        expect(rate!.meta.readOnly, isTrue, reason: 'Refresh Rate is read-only');

        ctx.note('${d.label}: display schema fields 0..4 ok; '
            'render block ${int32FromBytes(renderBlock.value)}');
      }
    },
  ),

  HilTest(
    'LED display renders a scene above 200 FPS',
    category: HilCategory.module,
    prereq: _displayRender,
    body: (ctx) async {
      for (final d in ctx.devices(_displayRender)) {
        final reg = RegisterClient(deviceId: d.id);
        final instances =
            await reg.enumerateInstanceIds(BlockType.vysiDisplay.value) ??
                const <int>[];
        if (instances.isEmpty) {
          ctx.note('${d.label}: no display instance to drive');
          continue;
        }
        // A Tamu drives its wired panel on instance 1; fall back to the last.
        final target = instances.contains(1) ? 1 : instances.last;

        // Remember the settings this test touches, then park every display.
        final saved =
            <int, ({List<int> render, List<int> brightness, List<int> layout})>{};
        for (final inst in instances) {
          final r =
              await reg.readBlockField(BlockType.vysiDisplay.value, inst, 2, 0);
          final b =
              await reg.readBlockField(BlockType.vysiDisplay.value, inst, 0, 0);
          final l =
              await reg.readBlockField(BlockType.vysiDisplay.value, inst, 3, 0);
          expect(r != null && b != null && l != null, isTrue,
              reason: '${d.label} display $inst settings are readable');
          saved[inst] = (render: r!.value, brightness: b!.value, layout: l!.value);
          await reg.writeBlockField(BlockType.vysiDisplay.value, inst, 2, 0,
              ValueInfo(type: DataType.integer.value, size: 4), _u32(-1));
        }

        int? created;
        try {
          // A scratch render block: a red square (mask + fill), the minimal scene
          // Docs/Modules and blocks/LED display.md renders.
          final index = await reg.createDynamicBlock('HILRNDR');
          expect(index, isNotNull, reason: '${d.label} creates a render block');
          created = index;
          final block = DynBlock(
              index: index!,
              meta: ValueInfo(type: dynamicTypeForIndex(index)),
              name: 'HILRNDR');
          Future<void> entry(int field, int key, int type, List<int> value) async {
            final r = await reg.writeDynamicEntry(
                block, field, key, ValueInfo(type: type, key: key), value);
            expect(r, isNotNull, reason: 'render entry f$field.k$key accepted');
          }

          await entry(0, 0, DataType.geometry.value, const []);
          await entry(0, 1, DataType.enum_.value, const [3]); // Square
          await entry(0, 4, DataType.number.value, numberToBytes(4.0));
          await entry(1, 0, DataType.texture.value, const []);
          await entry(1, 1, DataType.enum_.value, const [1]); // Fill
          await entry(1, 4, DataType.colour.value, const [255, 0, 0, 255]);

          // Point the target display at the render block.
          expect(
              await reg.writeBlockField(
                  BlockType.vysiDisplay.value, target, 2, 0,
                  ValueInfo(type: DataType.integer.value, size: 4), _u32(index)),
              isNotNull,
              reason: '${d.label} display $target accepts the render block');

          // Let the renderer run, then read the achieved rate.
          double fps = 0;
          for (var attempt = 0; attempt < 10; attempt++) {
            await Future<void>.delayed(const Duration(milliseconds: 400));
            final rate = await reg.readBlockField(
                BlockType.vysiDisplay.value, target, 4, 0);
            if (rate != null && rate.value.length >= 4) {
              fps = numberFromBytes(rate.value);
              if (fps > 200) break;
            }
          }
          ctx.note('${d.label}: display $target rendered '
              '${fps.toStringAsFixed(1)} FPS');
          expect(fps, greaterThan(200),
              reason: '${d.label} display $target renders above 200 FPS');
        } finally {
          if (created != null) await reg.deleteDynamic(block: created);
          for (final inst in instances) {
            final s = saved[inst]!;
            await reg.writeBlockField(BlockType.vysiDisplay.value, inst, 2, 0,
                ValueInfo(type: DataType.integer.value, size: 4), s.render);
            await reg.writeBlockField(BlockType.vysiDisplay.value, inst, 0, 0,
                ValueInfo(type: DataType.number.value, size: 4), s.brightness);
            await reg.writeBlockField(BlockType.vysiDisplay.value, inst, 3, 0,
                ValueInfo(type: DataType.filename.value, size: 8), s.layout);
          }
        }
      }
    },
  ),

  HilTest(
    'LED display rejects a layout file it cannot load',
    category: HilCategory.module,
    prereq: _display,
    body: (ctx) async {
      for (final d in ctx.devices(_display)) {
        final reg = RegisterClient(deviceId: d.id);
        List<int>? originalValue;
        var done = false;
        try {
          final before =
              await reg.readBlockField(BlockType.vysiDisplay.value, 0, 3, 0);
          expect(before, isNotNull);
          final original = before!.value;
          originalValue = original;

          // A name that names no file must be refused, and must not become the
          // stored value (the trigger validates before committing).
          final rejected = await reg.writeBlockField(
              BlockType.vysiDisplay.value,
              0,
              3,
              0,
              ValueInfo(type: DataType.filename.value, size: 8),
              _pad8('HILNONE').codeUnits);
          ctx.note('${d.label}: unloadable layout name -> '
              '${rejected == null ? 'rejected' : 'accepted'}');
          expect(rejected, isNull,
              reason: '${d.label} rejects a layout file it cannot load');
          final after =
              await reg.readBlockField(BlockType.vysiDisplay.value, 0, 3, 0);
          expect(after!.value, equals(original),
              reason: 'the stored name still matches the layout in use');

          // Put the original name back (it loads, so it is accepted).
          final restored = await reg.writeBlockField(
              BlockType.vysiDisplay.value,
              0,
              3,
              0,
              ValueInfo(type: DataType.filename.value, size: 8),
              original);
          expect(restored, isNotNull,
              reason: '${d.label} restores the original layout');
          done = true;
        } finally {
          if (!done && originalValue != null) {
            await reg.writeBlockField(
                BlockType.vysiDisplay.value,
                0,
                3,
                0,
                ValueInfo(type: DataType.filename.value, size: 8),
                originalValue);
          }
        }
      }
    },
  ),

  // ===========================================================================
  // Fan / PWM (pwm)
  // ===========================================================================
  HilTest(
    'fan reports the PWM schema',
    category: HilCategory.module,
    prereq: _pwm,
    body: (ctx) async {
      for (final d in ctx.devices(_pwm)) {
        final reg = RegisterClient(deviceId: d.id);
        final fields =
            await reg.enumerateFieldIndexes(BlockType.pwm.value, 0);
        expect(fields, equals([0, 1]),
            reason: '${d.label} PWM has Frequency and Duty');

        final freq = await reg.readBlockField(BlockType.pwm.value, 0, 0, 0);
        expect(freq, isNotNull);
        expect(freq!.meta.dataType, DataType.uint32);
        expect(freq.meta.size, 4);
        expect(freq.meta.flags & ValueFlags.trigger, isNot(0));
        expect(freq.meta.persistent, isTrue);
        expect(freq.meta.readOnly, isFalse);

        final duty = await reg.readBlockField(BlockType.pwm.value, 0, 1, 0);
        expect(duty!.meta.dataType, DataType.uint32);
        expect(duty.meta.size, 4);
        expect(duty.meta.flags & ValueFlags.trigger, isNot(0));
        expect(duty.meta.readOnly, isFalse);

        ctx.note('${d.label}: PWM ${uint32FromBytes(freq.value)} Hz, '
            'duty ${uint32FromBytes(duty.value)} %');
      }
    },
  ),

  HilTest(
    'fan frequency and duty round-trip and are restored',
    category: HilCategory.module,
    prereq: _pwm,
    body: (ctx) async {
      for (final d in ctx.devices(_pwm)) {
        final reg = RegisterClient(deviceId: d.id);
        List<int>? freqOriginalV;
        List<int>? dutyOriginalV;
        var done = false;
        try {
          final freqBefore =
              await reg.readBlockField(BlockType.pwm.value, 0, 0, 0);
          final dutyBefore =
              await reg.readBlockField(BlockType.pwm.value, 0, 1, 0);
          expect(freqBefore != null && dutyBefore != null, isTrue);
          final freqOriginal = freqBefore!.value;
          final dutyOriginal = dutyBefore!.value;
          freqOriginalV = freqOriginal;
          dutyOriginalV = dutyOriginal;

          // Probe both triggers with a safe value, then read it back.
          const probeFreq = 20000;
          const probeDuty = 42;
          expect(
              await reg.writeBlockField(BlockType.pwm.value, 0, 0, 0,
                  ValueInfo(type: DataType.uint32.value, size: 4), _u32(probeFreq)),
              isNotNull,
              reason: '${d.label} accepts a frequency write');
          expect(
              await reg.writeBlockField(BlockType.pwm.value, 0, 1, 0,
                  ValueInfo(type: DataType.uint32.value, size: 4), _u32(probeDuty)),
              isNotNull,
              reason: '${d.label} accepts a duty write');

          final freqRead = await reg.readBlockField(BlockType.pwm.value, 0, 0, 0);
          final dutyRead = await reg.readBlockField(BlockType.pwm.value, 0, 1, 0);
          expect(uint32FromBytes(freqRead!.value), probeFreq);
          expect(uint32FromBytes(dutyRead!.value), probeDuty);

          // Restore the values found (the fan is an actuator).
          await reg.writeBlockField(BlockType.pwm.value, 0, 0, 0,
              ValueInfo(type: DataType.uint32.value, size: 4), freqOriginal);
          await reg.writeBlockField(BlockType.pwm.value, 0, 1, 0,
              ValueInfo(type: DataType.uint32.value, size: 4), dutyOriginal);
          expect(uint32FromBytes(
                  (await reg.readBlockField(BlockType.pwm.value, 0, 0, 0))!.value),
              uint32FromBytes(freqOriginal));
          expect(uint32FromBytes(
                  (await reg.readBlockField(BlockType.pwm.value, 0, 1, 0))!.value),
              uint32FromBytes(dutyOriginal));
          ctx.note('${d.label}: PWM probed $probeFreq Hz/$probeDuty % and restored');
          done = true;
        } finally {
          if (!done) {
            if (freqOriginalV != null) {
              await reg.writeBlockField(BlockType.pwm.value, 0, 0, 0,
                  ValueInfo(type: DataType.uint32.value, size: 4), freqOriginalV);
            }
            if (dutyOriginalV != null) {
              await reg.writeBlockField(BlockType.pwm.value, 0, 1, 0,
                  ValueInfo(type: DataType.uint32.value, size: 4), dutyOriginalV);
            }
          }
        }
      }
    },
  ),

  // ===========================================================================
  // Buttons & LEDs (button / led / ledButton)
  // ===========================================================================
  HilTest(
    'button reports a read-only raw state',
    category: HilCategory.module,
    prereq: _button,
    body: (ctx) async {
      for (final d in ctx.devices(_button)) {
        final reg = RegisterClient(deviceId: d.id);
        final fields =
            await reg.enumerateFieldIndexes(BlockType.button.value, 0);
        expect(fields, equals([0]), reason: '${d.label} button has one field');
        final state = await reg.readBlockField(BlockType.button.value, 0, 0, 0);
        expect(state, isNotNull);
        expect(state!.meta.dataType, DataType.bool_);
        expect(state.meta.size, 1);
        expect(state.meta.readOnly, isTrue,
            reason: '${d.label} button raw state is read-only');
        ctx.note('${d.label}: button raw state '
            '${state.value.isNotEmpty && state.value.first != 0 ? 'pressed' : 'free'}');
      }
    },
  ),

  HilTest(
    'LED state round-trips and is restored',
    category: HilCategory.module,
    prereq: _led,
    body: (ctx) async {
      for (final d in ctx.devices(_led)) {
        final reg = RegisterClient(deviceId: d.id);
        List<int>? originalValue;
        var done = false;
        try {
          final fields = await reg.enumerateFieldIndexes(BlockType.led.value, 0);
          expect(fields, equals([0]), reason: '${d.label} LED has one field');
          final before = await reg.readBlockField(BlockType.led.value, 0, 0, 0);
          expect(before, isNotNull);
          expect(before!.meta.dataType, DataType.bool_);
          expect(before.meta.flags & ValueFlags.trigger, isNot(0));
          expect(before.meta.readOnly, isFalse);
          final original = before.value;
          originalValue = original;
          final probe = (original.isNotEmpty && original.first != 0) ? 0 : 1;

          expect(
              await reg.writeBlockField(BlockType.led.value, 0, 0, 0,
                  ValueInfo(type: DataType.bool_.value, size: 1), [probe]),
              isNotNull,
              reason: '${d.label} accepts an LED state write');
          final lit = await reg.readBlockField(BlockType.led.value, 0, 0, 0);
          expect(lit!.value.first, probe);

          // Restore what was found.
          await reg.writeBlockField(BlockType.led.value, 0, 0, 0,
              ValueInfo(type: DataType.bool_.value, size: 1), original);
          final restored = await reg.readBlockField(BlockType.led.value, 0, 0, 0);
          expect(restored!.value, equals(original));
          ctx.note('${d.label}: LED toggled to $probe and restored');
          done = true;
        } finally {
          if (!done && originalValue != null) {
            await reg.writeBlockField(BlockType.led.value, 0, 0, 0,
                ValueInfo(type: DataType.bool_.value, size: 1), originalValue);
          }
        }
      }
    },
  ),

  HilTest(
    'LED/Button keeps its reserved fields and a trigger LEDState',
    category: HilCategory.module,
    prereq: _ledButton,
    body: (ctx) async {
      for (final d in ctx.devices(_ledButton)) {
        final reg = RegisterClient(deviceId: d.id);
        List<int>? originalValue;
        var done = false;
        try {
          final fields =
              await reg.enumerateFieldIndexes(BlockType.ledButton.value, 0);
          expect(fields, equals([0, 3]),
              reason: '${d.label} LED/Button exposes Button (0) and LEDState (3)');

          final button =
              await reg.readBlockField(BlockType.ledButton.value, 0, 0, 0);
          expect(button!.meta.dataType, DataType.bool_);
          expect(button.meta.readOnly, isTrue,
              reason: '${d.label} button raw state is read-only');

          final ledState =
              await reg.readBlockField(BlockType.ledButton.value, 0, 3, 0);
          expect(ledState!.meta.dataType, DataType.bool_);
          expect(ledState.meta.flags & ValueFlags.trigger, isNot(0),
              reason: '${d.label} LEDState is a trigger field');
          expect(ledState.meta.readOnly, isFalse);

          final original = ledState.value;
          originalValue = original;
          final probe = (original.isNotEmpty && original.first != 0) ? 0 : 1;
          expect(
              await reg.writeBlockField(BlockType.ledButton.value, 0, 3, 0,
                  ValueInfo(type: DataType.bool_.value, size: 1), [probe]),
              isNotNull,
              reason: '${d.label} accepts an LEDState write');
          expect(
              (await reg.readBlockField(BlockType.ledButton.value, 0, 3, 0))!
                  .value
                  .first,
              probe);
          // Restore what was found.
          await reg.writeBlockField(BlockType.ledButton.value, 0, 3, 0,
              ValueInfo(type: DataType.bool_.value, size: 1), original);
          ctx.note('${d.label}: LED/Button LEDState toggled to $probe and restored');
          done = true;
        } finally {
          if (!done && originalValue != null) {
            await reg.writeBlockField(BlockType.ledButton.value, 0, 3, 0,
                ValueInfo(type: DataType.bool_.value, size: 1), originalValue);
          }
        }
      }
    },
  ),

  // ===========================================================================
  // Measurement (accGyr / resistiveMeasure)
  // ===========================================================================
  HilTest(
    'resistive measurement reports its schema and value',
    category: HilCategory.module,
    prereq: _resistive,
    body: (ctx) async {
      for (final d in ctx.devices(_resistive)) {
        final reg = RegisterClient(deviceId: d.id);
        final instances =
            await reg.enumerateInstanceIds(BlockType.resistiveMeasure.value) ??
                const <int>[];
        expect(instances, isNotEmpty,
            reason: '${d.label} exposes a measurement channel');

        for (final inst in instances) {
          final fields = await reg
              .enumerateFieldIndexes(BlockType.resistiveMeasure.value, inst);
          expect(fields, equals([0, 1, 2, 3, 4]),
              reason: '${d.label} channel $inst has the documented fields');

          final rate = await reg.readBlockField(
              BlockType.resistiveMeasure.value, inst, 0, 0);
          expect(rate!.meta.dataType, DataType.number);
          expect(rate.meta.persistent, isTrue);
          expect(rate.meta.flags & ValueFlags.trigger, isNot(0));

          final sensor = await reg.readBlockField(
              BlockType.resistiveMeasure.value, inst, 1, 0);
          expect(sensor!.meta.dataType, DataType.enum_);
          expect(sensor.meta.persistent, isTrue);
          expect(sensor.meta.readOnly, isFalse);
          expect(sensor.value.first, inInclusiveRange(0, 5),
              reason: '${d.label} channel $inst sensor type is a known enum');

          final filter = await reg.readBlockField(
              BlockType.resistiveMeasure.value, inst, 2, 0);
          expect(filter!.meta.dataType, DataType.number);
          expect(filter.meta.persistent, isTrue);
          expect(filter.meta.flags & ValueFlags.trigger, isNot(0));

          final value = await reg.readBlockField(
              BlockType.resistiveMeasure.value, inst, 3, 0);
          expect(value!.meta.dataType, DataType.number);
          expect(value.meta.readOnly, isTrue,
              reason: '${d.label} Measured Value is read-only');

          final range = await reg.readBlockField(
              BlockType.resistiveMeasure.value, inst, 4, 0);
          expect(range!.meta.dataType, DataType.number);
          expect(range.meta.readOnly, isTrue,
              reason: '${d.label} Current Range is read-only');

          ctx.note('${d.label} channel $inst: sensor ${sensor.value.first}, '
              '${numberFromBytes(value.value).toStringAsFixed(2)} '
              '(range ${numberFromBytes(range.value).toStringAsFixed(2)} kOhm)');
        }
      }
    },
  ),

  HilTest(
    'resistive measurement filter coefficient round-trips',
    category: HilCategory.module,
    prereq: _resistive,
    body: (ctx) async {
      for (final d in ctx.devices(_resistive)) {
        final reg = RegisterClient(deviceId: d.id);
        List<int>? originalValue;
        var done = false;
        try {
          final before = await reg.readBlockField(
              BlockType.resistiveMeasure.value, 0, 2, 0);
          expect(before, isNotNull);
          final original = before!.value;
          originalValue = original;

          expect(
              await reg.writeBlockField(
                  BlockType.resistiveMeasure.value,
                  0,
                  2,
                  0,
                  ValueInfo(type: DataType.number.value, size: 4),
                  numberToBytes(0.5)),
              isNotNull,
              reason: '${d.label} accepts a filter coefficient write');
          final read = await reg.readBlockField(
              BlockType.resistiveMeasure.value, 0, 2, 0);
          expect(numberFromBytes(read!.value), closeTo(0.5, 0.01));

          // Restore the value found.
          await reg.writeBlockField(BlockType.resistiveMeasure.value, 0, 2, 0,
              ValueInfo(type: DataType.number.value, size: 4), original);
          final restored = await reg.readBlockField(
              BlockType.resistiveMeasure.value, 0, 2, 0);
          expect(numberFromBytes(restored!.value),
              closeTo(numberFromBytes(original), 0.01));
          ctx.note('${d.label}: filter coefficient probed 0.5 and restored');
          done = true;
        } finally {
          if (!done && originalValue != null) {
            await reg.writeBlockField(BlockType.resistiveMeasure.value, 0, 2, 0,
                ValueInfo(type: DataType.number.value, size: 4), originalValue);
          }
        }
      }
    },
  ),

  HilTest(
    'accelerometer/gyroscope reports its schema and live vectors',
    category: HilCategory.module,
    prereq: _accGyr,
    body: (ctx) async {
      for (final d in ctx.devices(_accGyr)) {
        final reg = RegisterClient(deviceId: d.id);
        final fields =
            await reg.enumerateFieldIndexes(BlockType.accGyr.value, 0);
        expect(fields, equals([0, 1, 2, 3, 4, 5, 6]),
            reason: '${d.label} acc/gyr has the documented fields');

        for (final f in [0, 1, 2]) {
          final r = await reg.readBlockField(BlockType.accGyr.value, 0, f, 0);
          expect(r!.meta.dataType, DataType.enum_,
              reason: '${d.label} field $f is an enum');
          expect(r.meta.flags & ValueFlags.trigger, isNot(0));
          expect(r.meta.persistent, isTrue);
        }
        for (final f in [3, 4]) {
          final r = await reg.readBlockField(BlockType.accGyr.value, 0, f, 0);
          expect(r!.meta.dataType, DataType.number,
              reason: '${d.label} field $f is an EMA coefficient');
          expect(r.meta.persistent, isTrue);
          expect(r.meta.readOnly, isFalse);
        }

        final acc = await reg.readBlockField(BlockType.accGyr.value, 0, 5, 0);
        expect(acc!.meta.dataType, DataType.vector);
        expect(acc.meta.readOnly, isTrue);
        expect(acc.value.length, 12, reason: 'Acceleration is a Vector<3>');
        final ax = numberFromBytes(acc.value, 0);
        final ay = numberFromBytes(acc.value, 4);
        final az = numberFromBytes(acc.value, 8);
        final magnitude = math.sqrt(ax * ax + ay * ay + az * az);

        final gyro = await reg.readBlockField(BlockType.accGyr.value, 0, 6, 0);
        expect(gyro!.meta.dataType, DataType.vector);
        expect(gyro.meta.readOnly, isTrue);
        expect(gyro.value.length, 12,
            reason: 'Angular Velocity is a Vector<3>');

        ctx.note('${d.label}: acc ${ax.toStringAsFixed(2)},'
            '${ay.toStringAsFixed(2)},${az.toStringAsFixed(2)} m/s^2 '
            '(|a| ${magnitude.toStringAsFixed(2)})');
        // A live sensor never reads zero; the bound only catches a dead part.
        expect(magnitude, greaterThan(0.1),
            reason: '${d.label} accelerometer is live (not stuck at zero)');
        expect(magnitude, lessThan(100),
            reason: '${d.label} acceleration is within a sane range');
      }
    },
  ),

  HilTest(
    'accelerometer sampling rate writes back the applied index',
    category: HilCategory.module,
    prereq: _accGyr,
    body: (ctx) async {
      for (final d in ctx.devices(_accGyr)) {
        final reg = RegisterClient(deviceId: d.id);
        int? originalValue;
        var done = false;
        try {
          final before =
              await reg.readBlockField(BlockType.accGyr.value, 0, 0, 0);
          expect(before, isNotNull);
          final original = before!.value.first;
          originalValue = original;
          final probe = (original + 1) % 8; // a valid ODR index

          expect(
              await reg.writeBlockField(BlockType.accGyr.value, 0, 0, 0,
                  ValueInfo(type: DataType.enum_.value, size: 1), [probe]),
              isNotNull,
              reason: '${d.label} accepts an ODR write');
          final read = await reg.readBlockField(BlockType.accGyr.value, 0, 0, 0);
          expect(read!.value.first, probe,
              reason: 'the stored ODR matches the applied one');

          // Restore the index found.
          await reg.writeBlockField(BlockType.accGyr.value, 0, 0, 0,
              ValueInfo(type: DataType.enum_.value, size: 1), [original]);
          final restored =
              await reg.readBlockField(BlockType.accGyr.value, 0, 0, 0);
          expect(restored!.value.first, original);
          ctx.note('${d.label}: ODR probed index $probe and restored $original');
          done = true;
        } finally {
          if (!done && originalValue != null) {
            await reg.writeBlockField(BlockType.accGyr.value, 0, 0, 0,
                ValueInfo(type: DataType.enum_.value, size: 1), [originalValue]);
          }
        }
      }
    },
  ),
];
