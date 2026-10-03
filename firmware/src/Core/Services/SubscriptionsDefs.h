#pragma once

// Shared subscription tables, lookups and field access.
//
// Part of Core/Services/Subscriptions.h (included from there).


#pragma once
#include "Core/Functions/Packet.h"
#include "Core/Functions/TimeSync.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Functions/Dispatcher.h"
#include "Core/Services/Register.h"
#include "Core/Services/Storage.h"
#include "Core/Types/Enums.h"

// Reduced subscription service (Docs/Services/Subscriptions.md).
//   USE_SUB_PROVIDE - provider side: sends source values (raw bytes) with hash-based
//                     on-change detection, checked from the main loop.
//   USE_SUB_REQUEST - requester side (Tamu): stores target/source, applies values with the
//                     foreign-origin flag and confirms with a hash.
// Value updates carry the RAW value bytes (no TLFV header); the requester's confirmation
// is the 4-byte FNV-1a hash of the received bytes.
// The subscription TRID range (TRID_SUB_BASE..TRID_SUB_MAX) is defined in Functions/Packet.h.

// Packet priorities (Docs/RSBus and Packets.md: high-priority subscriptions sit above
// "Other" (default 8), low-priority ones below it).
#define SUB_PRIORITY_HIGH 4
#define SUB_PRIORITY_LOW  12

// True for the high-priority triggers (confirmation and edge detection).
static inline bool SubscriptionsHighPriority(TriggerType t) {
    return t == TriggerType::OnChangeConfirm || t == TriggerType::EdgeRising ||
           t == TriggerType::EdgeFalling || t == TriggerType::EdgeAny;
}

// FNV-1a hash over the value bytes (provider on-change detection + requester confirmation).
static inline uint32_t Fnv1a(const uint8_t *data, uint8_t len) {
    uint32_t hash = 0x811C9DC5u;
    for (uint8_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

// Docs/Services/Subscriptions.md "Subscription table": the shared 16-byte describing record
// embedded by both the requester and provider entries. On the wire it is
// sourceReg(u32) | trigger(u8) | minTime(u24) | period(u32) | deadzone(i32) = 16 bytes.
struct SubscriptionTable {
    uint32_t sourceReg = 0;              // BlockInfo at the provider's register
    TriggerType trigger = TriggerType::None;
    uint32_t minTimeMs = 0;              // minimum/retry interval, uint24 on the wire
    uint32_t periodMs = 0;
    Number deadzone = N(0);              // for number/vector triggers
};

#define SUB_TABLE_WIRE_SIZE 16

static inline uint16_t SubTableSerialize(uint8_t *buf, uint16_t off, const SubscriptionTable &t) {
    StoreUnaligned(buf + off, t.sourceReg); off += 4;
    buf[off++] = (uint8_t)t.trigger;
    uint32_t mt = t.minTimeMs & 0xFFFFFFu; // uint24, little-endian
    buf[off++] = (uint8_t)mt;
    buf[off++] = (uint8_t)(mt >> 8);
    buf[off++] = (uint8_t)(mt >> 16);
    StoreUnaligned(buf + off, t.periodMs); off += 4;
    StoreUnaligned(buf + off, (uint32_t)t.deadzone.Value); off += 4;
    return off;
}

static inline uint16_t SubTableDeserialize(const uint8_t *buf, uint16_t off, SubscriptionTable &t) {
    t.sourceReg = LoadUnaligned<uint32_t>(buf + off); off += 4;
    t.trigger = (TriggerType)buf[off++];
    t.minTimeMs = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) | ((uint32_t)buf[off + 2] << 16);
    off += 3;
    t.periodMs = LoadUnaligned<uint32_t>(buf + off); off += 4;
    t.deadzone = Number::FromRaw(LoadUnaligned<int32_t>(buf + off)); off += 4;
    return off;
}

// Docs: "Timeout 120s, renewed with new request." Both sides expire entries that stop being
// renewed (a value update / re-registration refreshes the deadline).
#define SUB_TIMEOUT_MS 120000u

static inline uint32_t SubTimeoutFrom(uint32_t nowMs) { return nowMs + SUB_TIMEOUT_MS; }

// Reads the current value of the register addressed by a 32-bit BlockInfo.
static inline FieldResult SubscriptionsGetField(uint32_t blockInfo) {
    uint16_t type = BlockInfoType(blockInfo);
    uint8_t inst = BlockInfoInstance(blockInfo);
    uint8_t field = BlockInfoField(blockInfo);
    uint8_t key = BlockInfoKey(blockInfo);

    if (type == 0 && inst == 0) {
        // System fields are synthesised on the fly; keep the value in a shared buffer
        // (single-threaded main loop, and the caller consumes it before any nested call).
        static uint8_t s_sysFieldBuf[24];
        ValueInfo m;
        uint8_t vsz = 0;
        if (RegisterGetSystemField(field, key, m, s_sysFieldBuf, vsz)) {
            FieldResult fr;
            fr.Descriptor = m;
            fr.Data = s_sysFieldBuf;
            return fr;
        }
        return FieldResult{};
    }
#ifdef USE_SCRIPTS
    if (BlockTypeRange::IsScript(type)) { // Script I/O (inputs/outputs)
        ValueInfo m;
        void *p = nullptr;
        if (ScriptGetIoPointer(BlockTypeRange::ScriptGlobal(type, inst), field, key, m, p)) {
            FieldResult fr;
            fr.Descriptor = m;
            fr.Data = p;
            return fr;
        }
        return FieldResult{};
    }
#endif
#ifdef USE_DYNAMIC_BLOCKS
    if (BlockTypeRange::IsDynamic(type)) {
        uint16_t gi = BlockTypeRange::DynamicGlobal(type, inst);
        if (gi < dynamic_block_registry.block_count) {
            DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(gi);
            if (block) {
                KeyResult kr = block->GetKey(field, key);
                FieldResult fr;
                fr.Descriptor = kr.meta;
                fr.Data = kr.data_ptr;
                return fr;
            }
        }
        return FieldResult{};
    }
#endif
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) return FieldResult{};
    return static_block_registry[idx].Get(field);
}

// ---------------------------------------------------------------------------
// Shared table search. The requester and provider tables are searched the same
// way; only what makes an entry "occupied" differs (RequesterEntry::active vs
// ProviderEntry::requesterAddr), so the role files supply that predicate.
// ---------------------------------------------------------------------------

// First occupied entry with `trid`, or nullptr.
template <typename T, size_t N, typename Occupied>
static T *SubTableFindByTrid(T (&table)[N], uint16_t trid, Occupied occupied) {
    for (size_t i = 0; i < N; i++) {
        if (occupied(table[i]) && table[i].trid == trid) return &table[i];
    }
    return nullptr;
}
