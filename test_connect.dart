import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/device_db.dart';

void main() async {
  final mgr = ConnectionManager.instance;
  await mgr.setAutoRefresh(false);
  final link = DiscoveredLink(id: "/dev/ttyACM0", type: LinkType.usb, name: "DAS");
  final err = await mgr.connectTo(link);
  print("Connect error: $err");
  if (err == null) {
    final db = DeviceDatabase.instance;
    final ping = await db.pingCore();
    print("Ping: $ping");
    await mgr.disconnect();
  }
}