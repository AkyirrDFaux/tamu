/// The bus that ACCEPTS a register write, which is what most restore fakes need and two of them
/// spelled out by hand.
///
/// A register write is answered by echoing the request (`BlockInfo` + `ValueInfo` + the value), and
/// the flagged path carries the SUCCESS flag instead of the bytes - `registerWriteEcho` in
/// `fake_bus.dart`. `backup_format_test` and `backup_restore_requester_prune_test` each wrote those
/// two overrides out in full, so a fake that answered a write differently from the firmware would
/// have made one suite's restore fail and the other's pass on the same request.
///
/// A subclass writes only the requests its own subject needs: [plainOther] for the payload path and
/// [flaggedOther] for the flagged one.
library;

import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';

import 'fake_bus.dart';

/// A bus whose register writes are accepted the way the firmware accepts them.
///
/// Usable as-is when a suite only needs a device that refuses nothing (restore_link_refusal). A fake that answers other requests subclasses it and overrides plainOther/flaggedOther.
class RegisterWriteBus extends FakeBus {
  RegisterWriteBus({super.firstTxId});

  @override
  Future<List<int>> plain(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async {
    if (service == ServiceType.register && functionCid == RegisterCid.write) {
      return payload;
    }
    return plainOther(targetId, service, functionCid, payload);
  }

  @override
  Future<PacketResponse> flagged(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async {
    if (service == ServiceType.register && functionCid == RegisterCid.write) {
      return registerWriteEcho(payload);
    }
    return flaggedOther(targetId, service, functionCid, payload);
  }

  /// The payload-path answer to a request that is not a register write.
  Future<List<int>> plainOther(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async => const [];

  /// The flagged-path answer to a request that is not a register write.
  Future<PacketResponse> flaggedOther(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async => busAccepted;
}
