import 'package:flutter/material.dart';

import '../core/types.dart';

/// Rough device-type icons (Docs/App/Devices.md: "Use icons for rough device
/// types").
IconData deviceTypeIcon(DeviceType type) => switch (type) {
      DeviceType.tamuV20A => Icons.developer_board,
      DeviceType.dualAnalogSensor => Icons.sensors,
      DeviceType.unknown => Icons.devices_other,
    };
