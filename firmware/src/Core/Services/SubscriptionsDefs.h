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

// Docs/Services/Subscriptions.md "Subscription table": the shared 16-byte describing record.
// The struct IS the wire layout (sourceReg u32, trigger u8, minTime u24, period u32, deadzone
// i32) with every u32 on a 4-byte offset, so it is copied whole - no field-by-field codec.
struct SubscriptionTable {
    uint32_t sourceReg = 0;              // BlockInfo at the provider's register
    TriggerType trigger = TriggerType::None;
    uint8_t minTime[3] = {0, 0, 0};      // minimum/retry interval, uint24 little-endian
    uint32_t periodMs = 0;
    Number deadzone = N(0);              // for number/vector triggers
};
static_assert(sizeof(SubscriptionTable) == 16, "the subscription table is 16 bytes on the wire");

#define SUB_TABLE_WIRE_SIZE 16

static inline uint32_t SubMinTime(const SubscriptionTable &t) {
    return (uint32_t)t.minTime[0] | ((uint32_t)t.minTime[1] << 8) | ((uint32_t)t.minTime[2] << 16);
}
static inline void SubSetMinTime(SubscriptionTable &t, uint32_t ms) {
    t.minTime[0] = (uint8_t)ms;
    t.minTime[1] = (uint8_t)(ms >> 8);
    t.minTime[2] = (uint8_t)(ms >> 16);
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
    // The System block (type 0) and every static block share the descriptor lookup; the System
    // fields are synthesised by its VirtualGet into a shared buffer.
    const StaticBlockDescriptor *blk = FindBlock(type, inst);
    if (!blk) return FieldResult{};
    return blk->Get(field, key);
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
