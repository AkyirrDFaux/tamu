#pragma once
#include "Core/Functions/Packet.h"
#include "Core/Functions/TimeSync.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Functions/Dispatcher.h"
#include "Core/Services/Register.h"
#include "Core/Services/Storage.h"
#include "Core/Types/Enums.h"

#ifndef BOARD_DAS_v0_1
// The DAS is a pure sensor node; its subscription service (provider table, ticks and
// service handler) is temporarily removed while its flash budget is being reworked.
// The Tamu v2.0A (core) keeps the requester + provider support below.
#define MAX_PROVIDER_SUBS 20
#define PROVIDER_TOLERANCE_SIZE 16
#define PROVIDER_LASTVALUE_SIZE 16

#define TRID_SUB_BASE 0xFA00
#define TRID_SUB_MAX  0xFBFF

// Reads the current value of the register addressed by a 32-bit BlockInfo.
// The System block (type 0, inst 0) is virtual and resolved via RegisterGetSystemField;
// other blocks are looked up in the static registry (or dynamic registry for 0x3FF).
static inline FieldResult SubscriptionsGetField(uint32_t blockInfo) {
    uint16_t type = BlockInfoType(blockInfo);
    uint8_t inst = BlockInfoInstance(blockInfo);
    uint8_t field = BlockInfoField(blockInfo);
    uint8_t key = BlockInfoKey(blockInfo);

    if (type == 0 && inst == 0) {
#ifndef BOARD_DAS_v0_1
        BlockMeta m;
        uint8_t vbuf[24];
        uint8_t vsz = 0;
        if (RegisterGetSystemField(field, key, m, vbuf, vsz)) {
            FieldResult fr;
            fr.Descriptor = m;
            fr.Data = vbuf;
            return fr;
        }
#endif
        return FieldResult{};
    }
#ifndef DISABLE_DYNAMIC_MEMORY
    if (type == 0x3FF) {
        if (inst < dynamic_block_registry.block_count) {
            DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
            if (block) return block->Get(field);
        }
        return FieldResult{};
    }
#endif
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) return FieldResult{};
    return static_block_registry[idx].Get(field);
}

struct ProviderEntry {
    uint16_t trid = 0;
    uint32_t sourceReg = 0;
    uint16_t requesterAddr = 0;
    TriggerType trigger = TriggerType::Periodic;
    uint32_t periodMs = 0;
    uint32_t lastSentMs = 0;
    uint32_t minTimeMs = 0;
    uint32_t counter = 0;
    uint8_t toleranceLen = 0;
    uint8_t toleranceData[PROVIDER_TOLERANCE_SIZE];
    uint8_t lastValueLen = 0;
    uint8_t lastValueData[PROVIDER_LASTVALUE_SIZE];
    bool awaitingAck = false;
    uint8_t retryCount = 0;
};

static ProviderEntry providerTable[MAX_PROVIDER_SUBS];

static uint16_t SubscriptionsNextTrid() {
    static uint16_t next = TRID_SUB_BASE;
    for (int tries = 0; tries < (TRID_SUB_MAX - TRID_SUB_BASE + 1); tries++) {
        uint16_t cand = next++;
        if (cand > TRID_SUB_MAX) { next = TRID_SUB_BASE; cand = next++; }
        bool used = false;
        for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
            if (providerTable[i].requesterAddr != 0 && providerTable[i].trid == cand) {
                used = true; break;
            }
        }
        if (!used) return cand;
    }
    return 0;
}

static ProviderEntry* ProviderFindByTrid(uint16_t trid) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        if (providerTable[i].requesterAddr != 0 && providerTable[i].trid == trid)
            return &providerTable[i];
    }
    return nullptr;
}

static ProviderEntry* ProviderFindBySourceReg(uint32_t srcReg) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        if (providerTable[i].requesterAddr != 0 && providerTable[i].sourceReg == srcReg)
            return &providerTable[i];
    }
    return nullptr;
}

static ProviderEntry* ProviderFindFree() {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        if (providerTable[i].requesterAddr == 0)
            return &providerTable[i];
    }
    return nullptr;
}

static void ProviderClearEntry(ProviderEntry* e) {
    e->trid = 0;
    e->sourceReg = 0;
    e->requesterAddr = 0;
    e->trigger = TriggerType::Periodic;
    e->periodMs = 0;
    e->lastSentMs = 0;
    e->minTimeMs = 0;
    e->counter = 0;
    e->toleranceLen = 0;
    e->lastValueLen = 0;
    e->awaitingAck = false;
    e->retryCount = 0;
}

static bool TlfvEqual(const uint8_t* a, uint8_t alen, const uint8_t* b, uint8_t blen) {
    if (alen != blen) return false;
    for (uint8_t i = 0; i < alen; i++) if (a[i] != b[i]) return false;
    return true;
}

static void TlfvCopy(uint8_t* dst, uint8_t* dstLen, const uint8_t* src, uint8_t slen) {
    if (slen > PROVIDER_LASTVALUE_SIZE) slen = PROVIDER_LASTVALUE_SIZE;
    memcpy(dst, src, slen);
    *dstLen = slen;
}

static uint8_t DataTypeSize(DataType dt) {
    switch (dt) {
        case DataType::Bool: return 1;
        case DataType::Uint32: case DataType::Id: return 4;
        case DataType::Number: case DataType::Index: return 4;
        case DataType::SN: return 14;
        case DataType::Vector: return 12;
        case DataType::Matrix: return 0;
        case DataType::String: case DataType::Filename: return 0;
        default: return 4;
    }
}

#ifndef BOARD_DAS_v0_1
static void HandleRequesterValueUpdate(const PacketFrame &frame);
#endif

static void SendValueUpdate(ProviderEntry* e, const uint8_t* value, uint8_t vlen, bool isRetry = false) {
    if (!e || e->requesterAddr == 0) return;

    uint16_t payloadLen = vlen;
    if (payloadLen > MAX_PAYLOAD_SIZE) payloadLen = MAX_PAYLOAD_SIZE;

#ifndef BOARD_DAS_v0_1
    // Self-subscription (provider and requester are the same device, e.g. the core
    // subscribing to its own System Uptime): apply the update locally. A bus round-trip
    // would never come back, since no other device owns this address.
    if (e->requesterAddr == DeviceStatus.ShortAddress)
    {
        PacketFrame self;
        memset(&self, 0, sizeof(self));
        self.id_tgt = DeviceStatus.ShortAddress;
        self.id_src = DeviceStatus.ShortAddress;
        self.srv_tgt = MakeService(ServiceType::Subscriptions, 0);
        self.trid = e->trid;
        self.flags = FLAG_TYPE | FLAG_START | FLAG_STOP;
        self.payload_len = (uint8_t)((payloadLen + 3) / 4);
        memcpy(self.payload, value, payloadLen);
        HandleRequesterValueUpdate(self);
    }
#endif

    // Docs/Services/Subscriptions.md CID 0: the provider's value update is "sent as a
    // response packet" (FLAG_TYPE); the requester's confirmation is a request (no TYPE).
    // The shared output buffer keeps a full frame off the DAS's tight stack.
    PacketConstruct(&tx_frame, e->requesterAddr,
                    MakeService(ServiceType::Subscriptions, 0),
                    e->trid,
                    FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_REQACK,
                    value, payloadLen);

    if (!isRetry) {
        e->lastSentMs = DeviceStatus.UptimeMs;
        e->awaitingAck = (e->trigger == TriggerType::OnChangeConfirm ||
                          e->trigger == TriggerType::DeltaConfirm ||
                          e->trigger == TriggerType::EdgeRise ||
                          e->trigger == TriggerType::EdgeFall);
        e->retryCount = 0;
    } else {
        e->retryCount++;
    }
    SendAndVerifyPacket(tx_frame);
}

static void SendValueUpdateFromRegister(ProviderEntry* e) {
    FieldResult fr = SubscriptionsGetField(e->sourceReg);
    if (!fr.Data) return;

    uint8_t tlfv[32];
    uint8_t tlfvLen = 0;
    tlfv[tlfvLen++] = (uint8_t)BlockMetaType(fr.Descriptor.FlagsAndType);
    tlfv[tlfvLen++] = fr.Descriptor.Size;
    tlfv[tlfvLen++] = (uint8_t)BlockMetaFlags(fr.Descriptor.FlagsAndType);
    uint8_t vlen = fr.Descriptor.Size;
    if (vlen > 16) vlen = 16;
    memcpy(tlfv + tlfvLen, fr.Data, vlen);
    tlfvLen += vlen;

    TlfvCopy(e->lastValueData, &e->lastValueLen, tlfv, tlfvLen);
    SendValueUpdate(e, tlfv, tlfvLen);
}

static bool TriggerShouldFire(ProviderEntry* e, uint32_t nowMs) {
    if (e->requesterAddr == 0) return false;

    uint32_t elapsed = nowMs - e->lastSentMs;
    if (elapsed < e->minTimeMs) return false;

    switch (e->trigger) {
        case TriggerType::Periodic:
            return (e->periodMs > 0) && (elapsed >= e->periodMs);

        case TriggerType::OnChangePeriodic:
        case TriggerType::OnChangeConfirm:
            return true;

        case TriggerType::EdgeRise:
        case TriggerType::EdgeFall:
            return true;

        case TriggerType::DeltaPeriodic:
        case TriggerType::DeltaConfirm:
            return true;

        default:
            return false;
    }
}

static void EvaluateProviderTriggers(uint32_t nowMs) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* e = &providerTable[i];
        if (e->requesterAddr == 0) continue;

        if (e->awaitingAck) {
            if (nowMs - e->lastSentMs > 1000 && e->retryCount < 5) {
                SendValueUpdate(e, e->lastValueData, e->lastValueLen, true);
            }
            continue;
        }

        if (TriggerShouldFire(e, nowMs)) {
            SendValueUpdateFromRegister(e);
        }
    }
}

// Fires the provider table's on-change/edge/delta triggers after a local register write.
// Non-static: forward-declared in Register.h (which calls it) to keep the include
// dependency one-directional. Reaching the provider table is a no-op when the written
// register is not the source of any active subscription.
void SubscriptionsOnRegisterWrite(uint32_t srcReg, const uint8_t *newValue, uint8_t vlen) {
    ProviderEntry* e = ProviderFindBySourceReg(srcReg);
    if (!e) return;

    FieldResult fr = SubscriptionsGetField(srcReg);
    if (!fr.Data) return;

    uint8_t tlfv[32];
    uint8_t tlfvLen = 0;
    tlfv[tlfvLen++] = (uint8_t)BlockMetaType(fr.Descriptor.FlagsAndType);
    tlfv[tlfvLen++] = fr.Descriptor.Size;
    tlfv[tlfvLen++] = (uint8_t)BlockMetaFlags(fr.Descriptor.FlagsAndType);
    uint8_t copyLen = fr.Descriptor.Size;
    if (copyLen > 16) copyLen = 16;
    memcpy(tlfv + tlfvLen, fr.Data, copyLen);
    tlfvLen += copyLen;

    bool fire = false;
    switch (e->trigger) {
        case TriggerType::OnChangePeriodic:
        case TriggerType::OnChangeConfirm:
            fire = !TlfvEqual(e->lastValueData, e->lastValueLen, tlfv, tlfvLen);
            break;

        case TriggerType::EdgeRise: {
            bool prev = (e->lastValueLen > 0 && e->lastValueData[3] != 0);
            bool curr = (tlfvLen > 3 && tlfv[3] != 0);
            fire = (!prev && curr);
            e->counter++;
            break;
        }
        case TriggerType::EdgeFall: {
            bool prev = (e->lastValueLen > 0 && e->lastValueData[3] != 0);
            bool curr = (tlfvLen > 3 && tlfv[3] != 0);
            fire = (prev && !curr);
            e->counter++;
            break;
        }
        case TriggerType::DeltaPeriodic:
        case TriggerType::DeltaConfirm: {
            if (e->toleranceLen == 0 || BlockMetaType(fr.Descriptor.FlagsAndType) != e->toleranceData[0]) {
                fire = false;
            } else {
                int32_t tolerance = *(int32_t*)(e->toleranceData + 1);
                int32_t prevVal = *(int32_t*)(e->lastValueData + 3);
                int32_t currVal = *(int32_t*)(tlfv + 3);
                int32_t diff = currVal - prevVal;
                if (diff < 0) diff = -diff;
                fire = (diff > tolerance);
            }
            break;
        }
        default:
            fire = false;
    }

    if (fire) {
        TlfvCopy(e->lastValueData, &e->lastValueLen, tlfv, tlfvLen);
        SendValueUpdate(e, tlfv, tlfvLen);
    }
}

// Serializes one provider entry in the CID 2 wire format (Docs/Services/Subscriptions.md):
// sourceReg, requesterAddr, trigger, periodMs, lastSentMs, minTimeMs, counter, tolerance,
// lastValue. Returns the advanced buffer offset.
static uint16_t ProviderEntrySerialize(uint8_t *buf, uint16_t off, const ProviderEntry *e) {
    *(uint32_t *)(buf + off) = e->sourceReg; off += 4;
    *(uint16_t *)(buf + off) = e->requesterAddr; off += 2;
    buf[off++] = (uint8_t)e->trigger;
    *(uint32_t *)(buf + off) = e->periodMs; off += 4;
    *(uint32_t *)(buf + off) = e->lastSentMs; off += 4;
    *(uint32_t *)(buf + off) = e->minTimeMs; off += 4;
    *(uint32_t *)(buf + off) = e->counter; off += 4;
    buf[off++] = e->toleranceLen;
    if (e->toleranceLen > 0) { memcpy(buf + off, e->toleranceData, e->toleranceLen); off += e->toleranceLen; }
    buf[off++] = e->lastValueLen;
    if (e->lastValueLen > 0) { memcpy(buf + off, e->lastValueData, e->lastValueLen); off += e->lastValueLen; }
    return off;
}

// ===== Requester Side (Tamu v2.0A only) =====
#ifndef BOARD_DAS_v0_1
#define MAX_REQUESTER_SUBS 16

struct RequesterEntry {
    uint32_t targetReg = 0;
    uint32_t sourceReg = 0;
    uint16_t providerAddr = 0;
    TriggerType trigger = TriggerType::Periodic;
    uint32_t periodMs = 0;
    uint32_t minTimeMs = 0;
    uint32_t counter = 0;
    uint8_t toleranceLen = 0;
    uint8_t toleranceData[16];
    uint16_t trid = 0;
    bool active = false;
};

static RequesterEntry requesterTable[MAX_REQUESTER_SUBS];

static RequesterEntry* RequesterFindByTrid(uint16_t trid) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (requesterTable[i].active && requesterTable[i].trid == trid)
            return &requesterTable[i];
    }
    return nullptr;
}

static RequesterEntry* RequesterFindByTargetReg(uint32_t targetReg) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (requesterTable[i].active && requesterTable[i].targetReg == targetReg)
            return &requesterTable[i];
    }
    return nullptr;
}

static RequesterEntry* RequesterFindFree() {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (!requesterTable[i].active)
            return &requesterTable[i];
    }
    return nullptr;
}

static void RequesterClearEntry(RequesterEntry* e) {
    e->targetReg = 0;
    e->sourceReg = 0;
    e->providerAddr = 0;
    e->trigger = TriggerType::Periodic;
    e->periodMs = 0;
    e->minTimeMs = 0;
    e->counter = 0;
    e->toleranceLen = 0;
    e->trid = 0;
    e->active = false;
}

static const char* SubscriptionsRequesterFile = "SUBREQ";

static void SaveRequesterTable() {
    uint8_t buf[256];
    uint16_t off = 0;
    uint8_t count = 0;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (requesterTable[i].active) count++;
    }
    buf[off++] = count;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &requesterTable[i];
        if (!e->active) continue;
        *(uint32_t*)(buf + off) = e->targetReg; off += 4;
        *(uint32_t*)(buf + off) = e->sourceReg; off += 4;
        *(uint16_t*)(buf + off) = e->providerAddr; off += 2;
        buf[off++] = (uint8_t)e->trigger;
        *(uint32_t*)(buf + off) = e->periodMs; off += 4;
        *(uint32_t*)(buf + off) = e->minTimeMs; off += 4;
        *(uint32_t*)(buf + off) = e->counter; off += 4;
        buf[off++] = e->toleranceLen;
        if (e->toleranceLen > PROVIDER_TOLERANCE_SIZE) e->toleranceLen = PROVIDER_TOLERANCE_SIZE;
        if (e->toleranceLen > 0) { memcpy(buf + off, e->toleranceData, e->toleranceLen); off += e->toleranceLen; }
        *(uint16_t*)(buf + off) = e->trid; off += 2;
    }
    // Write via a temp file + rename (NOR-safe, like the backup files): an in-place
    // rewrite of the same flash region can need 0->1 bit transitions (e.g. count going
    // 0 -> 1), which NOR flash cannot program - the write would silently fail and the
    // restored table would come back empty. The rename always lands on a freshly
    // erased region.
    static const char tmp_name[8] = {'S','U','B','R','E','Q','~',' '};
    if (Storage.FileExists(tmp_name) != 0xFFFFFFFF)
        Storage.DeleteFile(tmp_name);
    if (!Storage.CreateFile(tmp_name, off))
        return;
    if (!Storage.WriteToFile(tmp_name, 0, off, (const char*)buf))
    {
        Storage.DeleteFile(tmp_name);
        return;
    }
    if (!Storage.RenameFile(tmp_name, SubscriptionsRequesterFile))
    {
        Storage.DeleteFile(tmp_name);
        return;
    }
}

// Re-registers the (non-persistent) provider side for a requester subscription so a
// restored table keeps pushing values after boot (docs: requester "re-activates after
// boot"). Same-device providers get a direct provider-table entry; remote providers get
// a CID 1 "Change subscription" packet.
static void RegisterRequesterProvider(RequesterEntry* e) {
    if (!e || !e->active) return;

    if (e->providerAddr == DeviceStatus.ShortAddress) {
        ProviderEntry* p = ProviderFindByTrid(e->trid);
        if (!p) { p = ProviderFindFree(); if (!p) return; }
        p->trid = e->trid;
        p->sourceReg = e->sourceReg;
        p->requesterAddr = DeviceStatus.ShortAddress;
        p->trigger = e->trigger;
        p->periodMs = e->periodMs;
        p->minTimeMs = e->minTimeMs;
        p->counter = e->counter;
        p->toleranceLen = e->toleranceLen;
        memcpy(p->toleranceData, e->toleranceData, e->toleranceLen);
        p->lastSentMs = 0;
        p->awaitingAck = false;
        p->retryCount = 0;
        SendValueUpdateFromRegister(p);
    } else {
        uint8_t payload[64]; uint16_t off = 0;
        *(uint32_t*)(payload + off) = e->targetReg; off += 4;
        *(uint32_t*)(payload + off) = e->sourceReg; off += 4;
        *(uint16_t*)(payload + off) = DeviceStatus.ShortAddress; off += 2;
        payload[off++] = (uint8_t)e->trigger;
        *(uint32_t*)(payload + off) = e->periodMs; off += 4;
        *(uint32_t*)(payload + off) = e->minTimeMs; off += 4;
        *(uint32_t*)(payload + off) = e->counter; off += 4;
        payload[off++] = e->toleranceLen;
        if (e->toleranceLen > 0) { memcpy(payload + off, e->toleranceData, e->toleranceLen); off += e->toleranceLen; }
        PacketFrame req;
        PacketConstruct(&req, e->providerAddr,
                        MakeService(ServiceType::Subscriptions, 1),
                        e->trid,
                        FLAG_START | FLAG_STOP,
                        payload, off);
        SendAndVerifyPacket(req);
    }
}

static void LoadRequesterTable() {
    uint8_t buf[256];
    uint16_t len = Storage.ReadFromFile(SubscriptionsRequesterFile, 0, sizeof(buf), (char*)buf);
    if (len == 0) return;

    uint16_t off = 0;
    uint8_t count = buf[off++];
    for (uint8_t i = 0; i < count && off < len; i++) {
        RequesterEntry* e = RequesterFindFree();
        if (!e) break;
        e->targetReg = *(uint32_t*)(buf + off); off += 4;
        e->sourceReg = *(uint32_t*)(buf + off); off += 4;
        e->providerAddr = *(uint16_t*)(buf + off); off += 2;
        e->trigger = (TriggerType)buf[off++];
        e->periodMs = *(uint32_t*)(buf + off); off += 4;
        e->minTimeMs = *(uint32_t*)(buf + off); off += 4;
        e->counter = *(uint32_t*)(buf + off); off += 4;
        e->toleranceLen = buf[off++];
        if (e->toleranceLen > PROVIDER_TOLERANCE_SIZE) e->toleranceLen = PROVIDER_TOLERANCE_SIZE;
        if (e->toleranceLen > 0 && off + e->toleranceLen <= len) {
            memcpy(e->toleranceData, buf + off, e->toleranceLen);
            off += e->toleranceLen;
        }
        e->trid = *(uint16_t*)(buf + off); off += 2;
        if (e->trid == 0) e->trid = SubscriptionsNextTrid();
        e->active = true;
    }
}

// Re-registers the provider side for every active requester subscription. Called after
// boot once the device has its bus address, since the provider registration needs the
// requester's own address (Remote) or the local-address check (same-device).
#ifndef BOARD_DAS_v0_1
void ReRegisterSubscriptions() {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (requesterTable[i].active)
            RegisterRequesterProvider(&requesterTable[i]);
    }
}
#endif

static void HandleRequesterValueUpdate(const PacketFrame &frame) {
    if (!(frame.flags & FLAG_TYPE)) return;
    
    RequesterEntry* e = RequesterFindByTrid(frame.trid);
    if (!e) return;

    FieldResult fr = SubscriptionsGetField(e->targetReg);
    if (!fr.Data) return;

    const uint8_t* payload = frame.payload;
    uint8_t payloadBytes = PayloadBytes(frame);
    if (payloadBytes < 3) return;
    
    uint8_t size = payload[1];
    const uint8_t* val = payload + 3;
    uint8_t vlen = payloadBytes - 3;
    if (vlen > size) vlen = size;
    if (vlen > fr.Descriptor.Size) vlen = fr.Descriptor.Size;

    uint16_t newFlags = fr.Descriptor.FlagsAndType | FieldFlags::External;
    
    uint32_t bi = e->targetReg;
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    
    if (type == 0 && inst == 0) {
        return;
    }
    
    int idx = FindStaticBlock(type, inst);
    if (idx >= 0) {
        BlockMeta meta = static_block_registry[idx].Schema->Map[field];
        meta.FlagsAndType = newFlags;
        static_block_registry[idx].Set(field, val, vlen, meta.FlagsAndType);
    }
#ifndef DISABLE_DYNAMIC_MEMORY
    else if (type == 0x3FF) {
        if (inst < dynamic_block_registry.block_count) {
            DynamicBlockDescriptor* block = dynamic_block_registry.GetBlock(inst);
            if (block) {
                BlockMeta meta;
                meta.FlagsAndType = newFlags;
                meta.Size = vlen;
                block->Set(field, val, vlen, meta.FlagsAndType);
            }
        }
    }
#endif

    // Confirm the update to the provider (docs CID 0: the confirmation is a request,
    // no FLAG_TYPE, carrying our last value). The provider clears its awaitingAck on
    // this frame, so confirm-required triggers stop retrying.
    PacketFrame reply;
    PacketConstruct(&reply, e->providerAddr,
                    MakeService(ServiceType::Subscriptions, 0),
                    e->trid,
                    FLAG_START | FLAG_STOP,
                    frame.payload, PayloadBytes(frame));
    SendAndVerifyPacket(reply);
}

#endif // BOARD_DAS_v0_1

// Shared reply for every subscription CID (docs: responses are packets with FLAG_TYPE).
// Centralizing the PacketConstruct + routing keeps the repeated handlers flash-lean, and
// the shared output buffer (tx_frame) avoids a full-size frame on the stack.
static void SubReply(const PacketFrame &frame, const uint8_t *payload, uint16_t len) {
    PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                    FLAG_TYPE | FLAG_START | FLAG_STOP, payload, len);
#ifdef USE_APP_INTERFACE
    if (frame.id_src == 0xFFFE) {
        AppInterfaceSend(tx_frame);
    } else {
        DispatchPacket(tx_frame);
    }
#else
    // Node (no app interface): the reply always goes back to the requester over the bus,
    // so skip re-entering the router and transmit it directly.
    SendAndVerifyPacket(tx_frame);
#endif
}

__attribute__((noinline)) static void HandleSubscriptions(const PacketFrame &frame) {
    DeviceLog("SUB", "HandleSubscriptions cid=%d, flags=0x%02X, trid=0x%04X, payload_len=%d", GetServiceCID(frame.srv_tgt), frame.flags, frame.trid, PayloadBytes(frame));
    uint8_t cid = GetServiceCID(frame.srv_tgt);

    switch (cid) {
        case 0: {
            if (frame.flags & FLAG_TYPE) {
                // Provider value update (response, docs CID 0): apply it to the
                // target register and confirm back (requester role).
                #ifndef BOARD_DAS_v0_1
                HandleRequesterValueUpdate(frame);
                #endif
            } else {
                // Requester confirmation (request) of our outgoing value update:
                // clear the outstanding ack so confirm-required triggers stop retrying.
                ProviderEntry* e = ProviderFindByTrid(frame.trid);
                if (e && e->awaitingAck) {
                    e->awaitingAck = false;
                    e->retryCount = 0;
                }
            }
            break;
        }
        case 1: {
            if (frame.payload_len == 0) {
                ProviderEntry* e = ProviderFindByTrid(frame.trid);
                if (e) ProviderClearEntry(e);
                // Send response to cancel request
                uint8_t resp = 1; // success
                SubReply(frame, &resp, 1);
                break;
            }
            if (PayloadBytes(frame) < 4 + 4 + 2 + 1 + 4 + 4 + 4 + 1) break;

            uint16_t offset = 0;
            offset += 4; // targetReg (not used by provider)
            uint32_t sourceReg = *(uint32_t*)(frame.payload + offset); offset += 4;
            uint16_t requesterAddr = *(uint16_t*)(frame.payload + offset); offset += 2;
            TriggerType trigger = (TriggerType)frame.payload[offset++];
            uint32_t periodMs = *(uint32_t*)(frame.payload + offset); offset += 4;
            uint32_t minTimeMs = *(uint32_t*)(frame.payload + offset); offset += 4;
            uint32_t counter = *(uint32_t*)(frame.payload + offset); offset += 4;
            uint8_t toleranceLen = frame.payload[offset++];
            uint8_t toleranceData[PROVIDER_TOLERANCE_SIZE] = {0};
            if (toleranceLen > PROVIDER_TOLERANCE_SIZE) toleranceLen = PROVIDER_TOLERANCE_SIZE;
            if (toleranceLen > 0 && offset + toleranceLen <= PayloadBytes(frame)) {
                memcpy(toleranceData, frame.payload + offset, toleranceLen);
            }

            ProviderEntry* e = ProviderFindByTrid(frame.trid);
            bool isNew = false;
            if (!e) {
                e = ProviderFindFree();
                if (!e) break;
                isNew = true;
            }
            if (isNew) {
                e->trid = frame.trid ? frame.trid : SubscriptionsNextTrid();
                if (e->trid == 0) break;
            }
            e->sourceReg = sourceReg;
            e->requesterAddr = requesterAddr;
            e->trigger = trigger;
            e->periodMs = periodMs;
            e->minTimeMs = minTimeMs;
            e->counter = counter;
            e->toleranceLen = toleranceLen;
            memcpy(e->toleranceData, toleranceData, toleranceLen);
            e->lastSentMs = 0;
            e->awaitingAck = false;
            e->retryCount = 0;

            // Send response to CID 1 request
            uint8_t resp = 1; // success
            SubReply(frame, &resp, 1);
            break;
        }
        case 2: {
            if (frame.payload_len == 0) {
                uint8_t count = 0;
                for (int i = 0; i < MAX_PROVIDER_SUBS; i++) if (providerTable[i].requesterAddr != 0) count++;
                uint8_t buf[1 + MAX_PROVIDER_SUBS * (4 + 2 + 1 + 4 + 4 + 4 + 4 + 1 + PROVIDER_TOLERANCE_SIZE + 1 + PROVIDER_LASTVALUE_SIZE)];
                uint16_t off = 0;
                buf[off++] = count;
                for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
                    if (providerTable[i].requesterAddr == 0) continue;
                    off = ProviderEntrySerialize(buf, off, &providerTable[i]);
                }
                SubReply(frame, buf, off);
            } else if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_PROVIDER_SUBS && providerTable[index].requesterAddr != 0) {
                    uint8_t buf[64];
                    uint16_t off = ProviderEntrySerialize(buf, 0, &providerTable[index]);
                    SubReply(frame, buf, off);
                }
            }
            break;
        }
        case 3: {
            #ifndef BOARD_DAS_v0_1
            if (frame.payload_len == 0) {
                uint8_t count = 0;
                for (int i = 0; i < MAX_REQUESTER_SUBS; i++) if (requesterTable[i].active) count++;
                uint8_t buf[1 + MAX_PROVIDER_SUBS * (4 + 2 + 1 + 4 + 4 + 4 + 4 + 1 + PROVIDER_TOLERANCE_SIZE + 1 + PROVIDER_LASTVALUE_SIZE)];
                uint16_t off = 0;
                buf[off++] = count;
                for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
                    if (!requesterTable[i].active) continue;
                    RequesterEntry* e = &requesterTable[i];
                    *(uint32_t*)(buf + off) = e->targetReg; off += 4;
                    *(uint32_t*)(buf + off) = e->sourceReg; off += 4;
                    *(uint16_t*)(buf + off) = e->providerAddr; off += 2;
                    buf[off++] = (uint8_t)e->trigger;
                    *(uint32_t*)(buf + off) = e->periodMs; off += 4;
                    *(uint32_t*)(buf + off) = e->minTimeMs; off += 4;
                    *(uint32_t*)(buf + off) = e->counter; off += 4;
                    buf[off++] = e->toleranceLen;
                    if (e->toleranceLen > 0) { memcpy(buf + off, e->toleranceData, e->toleranceLen); off += e->toleranceLen; }
                    *(uint16_t*)(buf + off) = e->trid; off += 2;
                }
                SubReply(frame, buf, off);
            } else if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_REQUESTER_SUBS && requesterTable[index].active) {
                    RequesterEntry* e = &requesterTable[index];
                    uint8_t buf[64];
                    uint16_t off = 0;
                    *(uint32_t*)(buf + off) = e->targetReg; off += 4;
                    *(uint32_t*)(buf + off) = e->sourceReg; off += 4;
                    *(uint16_t*)(buf + off) = e->providerAddr; off += 2;
                    buf[off++] = (uint8_t)e->trigger;
                    *(uint32_t*)(buf + off) = e->periodMs; off += 4;
                    *(uint32_t*)(buf + off) = e->minTimeMs; off += 4;
                    *(uint32_t*)(buf + off) = e->counter; off += 4;
                    buf[off++] = e->toleranceLen;
                    if (e->toleranceLen > 0) { memcpy(buf + off, e->toleranceData, e->toleranceLen); off += e->toleranceLen; }
                    *(uint16_t*)(buf + off) = e->trid; off += 2;

                    SubReply(frame, buf, off);
                }
            }
            #else
            uint8_t resp = 0;
            SubReply(frame, &resp, 1);
            #endif
            break;
        }
        case 4: {
            #ifndef BOARD_DAS_v0_1
            if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_REQUESTER_SUBS) {
                    if (frame.payload_len == 1) {
                        // Delete + compact (docs: the requester table is "sequential, sorted
                        // by address"). Leaving a hole would make the app's ordinal index
                        // disagree with the array index, so a later delete/edit hits the
                        // wrong slot.
                        for (int i = index; i < MAX_REQUESTER_SUBS - 1; i++)
                            requesterTable[i] = requesterTable[i + 1];
                        RequesterClearEntry(&requesterTable[MAX_REQUESTER_SUBS - 1]);
                        SaveRequesterTable();

                        // Send response to delete request
                        uint8_t resp = 1; // success
                        SubReply(frame, &resp, 1);
                    } else {
                        if (PayloadBytes(frame) < 1 + 4 + 4 + 2 + 1 + 4 + 4 + 4 + 1) {
                            break;
                        }
                        uint16_t offset = 1;
                        RequesterEntry* e = &requesterTable[index];
                        e->targetReg = *(uint32_t*)(frame.payload + offset); offset += 4;
                        e->sourceReg = *(uint32_t*)(frame.payload + offset); offset += 4;
                        e->providerAddr = *(uint16_t*)(frame.payload + offset); offset += 2;
                        e->trigger = (TriggerType)frame.payload[offset++];
                        e->periodMs = *(uint32_t*)(frame.payload + offset); offset += 4;
                        e->minTimeMs = *(uint32_t*)(frame.payload + offset); offset += 4;
                        e->counter = *(uint32_t*)(frame.payload + offset); offset += 4;
                        e->toleranceLen = frame.payload[offset++];
                        if (e->toleranceLen > PROVIDER_TOLERANCE_SIZE) e->toleranceLen = PROVIDER_TOLERANCE_SIZE;
                        if (e->toleranceLen > 0 && offset + e->toleranceLen <= PayloadBytes(frame)) {
                            memcpy(e->toleranceData, frame.payload + offset, e->toleranceLen);
                            offset += e->toleranceLen;
                        }
                        // Use frame.trid (from packet header) as the TRID for round-trip
                        // The app sends transaction ID in srvSource = makeService(App, txId)
                        e->trid = frame.trid;
                        e->active = true;
                        SaveRequesterTable();

                        // Send response to CID 4 request FIRST (before contacting provider)
                        uint8_t resp = 1; // success
                        PacketFrame reply;
                        PacketConstruct(&reply, frame.id_src,
                                        frame.srv_src,  // Echo app's transaction ID for response matching
                                        frame.trid,
                                        FLAG_TYPE | FLAG_START | FLAG_STOP,
                                        &resp, 1);
                        #ifdef USE_APP_INTERFACE
                        if (frame.id_src == 0xFFFE) {
                            AppInterfaceSend(reply);
                        } else {
                            DispatchPacket(reply);
                        }
                        #else
                        DispatchPacket(reply);
                        #endif
                        DeviceLog("SUB", "CID4 response sent");

                        // Then send CID 1 to provider (fire and forget, no REQACK)
                        // Provider will send CID 0 (Value Update) with current value later
                        uint8_t reqBuf[64];
                        uint16_t reqOff = 0;
                        *(uint32_t*)(reqBuf + reqOff) = e->targetReg; reqOff += 4;
                        *(uint32_t*)(reqBuf + reqOff) = e->sourceReg; reqOff += 4;
                        *(uint16_t*)(reqBuf + reqOff) = DeviceStatus.ShortAddress; reqOff += 2;
                        reqBuf[reqOff++] = (uint8_t)e->trigger;
                        *(uint32_t*)(reqBuf + reqOff) = e->periodMs; reqOff += 4;
                        *(uint32_t*)(reqBuf + reqOff) = e->minTimeMs; reqOff += 4;
                        *(uint32_t*)(reqBuf + reqOff) = e->counter; reqOff += 4;
                        reqBuf[reqOff++] = e->toleranceLen;
                        if (e->toleranceLen > 0) { memcpy(reqBuf + reqOff, e->toleranceData, e->toleranceLen); reqOff += e->toleranceLen; }

                        PacketFrame reqFrame;
                        PacketConstruct(&reqFrame, e->providerAddr,
                                        MakeService(ServiceType::Subscriptions, 1),
                                        e->trid,
                                        FLAG_START | FLAG_STOP,  // No FLAG_REQACK - fire and forget
                                        reqBuf, reqOff);
                        SendAndVerifyPacket(reqFrame);
                        DeviceLog("SUB", "CID1 sent to provider");
                        }
                }
            }
            #else
            if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_PROVIDER_SUBS) {
                    if (frame.payload_len == 1) {
                        ProviderClearEntry(&providerTable[index]);

                        // Send response to delete request
                        uint8_t resp = 1; // success
                        SubReply(frame, &resp, 1);
                    }
                }
            }
            #endif
            break;
        }
        case 5: { // Save the requester table to its file (persist current entries).
            #ifndef BOARD_DAS_v0_1
            SaveRequesterTable();
            #endif
            uint8_t resp = 1; // success
            SubReply(frame, &resp, 1);
            break;
        }
    }
}

void SubscriptionsTick(uint32_t nowMs) {
    EvaluateProviderTriggers(nowMs);
}
#endif // BOARD_DAS_v0_1