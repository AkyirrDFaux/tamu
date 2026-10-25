#pragma once

// Shared subscription tables, lookups and field access, plus the wire entry structs.
//
// Part of Core/Services/Subscriptions.h (included from there, before the service class,
// because the SubscriptionsService member layout and the role files both need the entry
// structs and their sizes).

#include "Core/Functions/Packet.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Services/Register.h"
#include "Core/Services/Storage.h"
#include "Core/Services/SubscriptionsLease.h" // SUB_TIMEOUT_MS/SubTimeoutFrom + the requester timing
#include "Core/Types/Enums.h"

// Subscription service (Docs/Services/Subscriptions.md).
//   USE_SUB_PROVIDE - provider side: sends source values (raw bytes) with hash-based
//                     on-change detection, checked from the main loop.
//   USE_SUB_REQUEST - requester side (Tamu): stores target/source, applies values with the
//                     foreign-origin flag and confirms with a hash.
// Value updates carry the RAW value bytes (no TLFV header); the requester's confirmation
// is the 4-byte FNV-1a hash of the received bytes.
// The subscription TRID range (TRID_SUB_BASE..TRID_SUB_MAX) is defined in Functions/Packet.h.

// Packet priorities (Docs/RSBus and Packets.md: high-priority subscriptions sit above
// "Other" (default 8), low-priority ones below it). Shared with the class table in Packet.h.

// Subscription service command IDs (Docs/Services/Subscriptions.md). The same numbers are the
// packet-build CIDs; the provider and requester roles split them into the low (0x0X) and high
// (0x2X) management ranges, with 0x1X for the requester's management commands.
enum class SubCid : uint8_t {
    ValueUpdate     = 0x00, // provider -> requester value (and the requester's confirmation)
    Change          = 0x01, // requester -> provider install/cancel
    RequesterGet    = 0x10, // get subscriptions (requester)
    RequesterSet    = 0x11, // set subscription (requester)
    RequesterRecall = 0x12, // recall all subscriptions (requester)
    RequesterSave   = 0x13, // save all subscriptions (requester)
    ProviderGet     = 0x20, // get subscriptions (provider)
    ProviderSet     = 0x21, // set subscription (provider)
};

// FNV-1a hash over the value bytes (provider on-change detection + requester confirmation).
// A generic hash over data - not service state - so it stays a free helper.
static inline uint32_t Fnv1a(const uint8_t *Data, uint8_t Len) {
    uint32_t Hash = 0x811C9DC5u;
    for (uint8_t i = 0; i < Len; i++) {
        Hash ^= Data[i];
        Hash *= 0x01000193u;
    }
    return Hash;
}

// Docs/Services/Subscriptions.md "Subscription table": the shared 16-byte describing record.
// The struct IS the wire layout (sourceReg u32, trigger u8, minTime u24, period u32, deadzone
// i32) with every u32 on a 4-byte offset, so it is copied whole - no field-by-field codec.
struct SubscriptionTable {
    uint32_t SourceReg = 0;              // BlockInfo at the provider's register
    TriggerType Trigger = TriggerType::None;
    uint8_t MinTime[3] = {0, 0, 0};      // minimum/retry interval, uint24 little-endian
    uint32_t PeriodMs = 0;
    Number Deadzone = N(0);              // for number/vector triggers
};
static_assert(sizeof(SubscriptionTable) == 16, "the subscription table is 16 Bytes on the wire");

// The wire size IS the struct size (pinned by the static_assert above), derived here so the
// two cannot drift apart.
static constexpr uint16_t SUB_TABLE_WIRE_SIZE = sizeof(SubscriptionTable);

static inline uint32_t SubMinTime(const SubscriptionTable &Table) {
    return (uint32_t)Table.MinTime[0] | ((uint32_t)Table.MinTime[1] << 8) | ((uint32_t)Table.MinTime[2] << 16);
}

// SUB_TIMEOUT_MS ("Timeout 120s, renewed with new request") and SubTimeoutFrom live in
// SubscriptionsLease.h, next to the keepalive cadence that has to stay inside that lease.

// ---------------------------------------------------------------------------
// Shared table search. The requester and provider tables are searched the same
// way; only what makes an entry "occupied" differs (RequesterEntry::active vs
// ProviderEntry::requesterAddr), so the role files supply that predicate.
// ---------------------------------------------------------------------------

// First occupied entry with `trid`, or nullptr.
template <typename Value, size_t Size, typename Occupied>
static Value *SubTableFindByTrid(Value (&Table)[Size], uint16_t Trid, Occupied IsOccupied) {
    for (size_t i = 0; i < Size; i++) {
        if (IsOccupied(Table[i]) && Table[i].Trid == Trid) return &Table[i];
    }
    return nullptr;
}

// ===========================================================================
// Entry wire structs. These live before the SubscriptionsService class (whose
// member layout is these tables) and before the role files that operate on them.
// ===========================================================================

#ifdef USE_SUB_REQUEST
#define MAX_REQUESTER_SUBS 16

// Docs "Requester table entry" (28 B wire): providerAddr, TRID, the shared subscription
// table, target register, timeout. The timeout is local uptime and is not persistent.
struct RequesterEntry {
    uint16_t ProviderAddr = 0;
    uint16_t Trid = 0;
    SubscriptionTable Sub;
    uint32_t TargetReg = 0;      // local write target
    // The 120 s deadline the documented entry carries: local uptime, not persisted, and refreshed by
    // the provider's answer. It is NOT enforced on this side - the requester keeps its rows (owner
    // ruling t_eb35360c) and only the provider expires its own entry - so it is a wire field the app
    // can read, not a lifetime.
    uint32_t Timeout = 0;
    bool Active = false;
    // Install/keepalive bookkeeping (transient, after the wire prefix): whether the provider last
    // answered the install decides the retry cadence. See SubscriptionsLease.h. The counter is
    // RAM-only - it is not part of the 28 B wire prefix and not part of the 24 B `.SUBREQ` entry - so
    // every boot, recall and Set starts it at 0.
    SubRequesterLease Lease;
};

// The wire prefix of the entry IS the struct prefix, so serialization is a plain copy of the
// first 28 bytes (Docs "Requester table entry"): providerAddr, trid, table, targetReg, timeout.
static_assert(offsetof(RequesterEntry, ProviderAddr) == 0, "wire order: providerAddr first");
static_assert(offsetof(RequesterEntry, Trid) == 2, "wire order: Trid after providerAddr");
static_assert(offsetof(RequesterEntry, Sub) == 4, "wire order: subscription table after Trid");
static_assert(offsetof(RequesterEntry, TargetReg) == 20, "wire order: targetReg after the table");
static_assert(offsetof(RequesterEntry, Timeout) == 24, "wire order: timeout after targetReg");
#define REQUESTER_ENTRY_WIRE_SIZE 28
#define REQUESTER_FILE_ENTRY_SIZE 24 // the wire entry minus the timeout (not persisted)

// The deferred re-registration queue capacity: a short address list (not a bitmask) keeps any
// 10-bit node id addressable, sized to the device registry's capacity (SNDB_MAX_ENTRIES) so a
// burst of assignments can never silently drop a re-push.
static const uint8_t KReRegisterMax = 128;
#endif // USE_SUB_REQUEST

#ifdef USE_SUB_PROVIDE
// The active provider table size. Defaults to 20; a RAM-constrained target overrides it with
// -D MAX_PROVIDER_SUBS=N (the DAS's 2 KB RAM needs only a couple of active providers). Kept as
// a build flag rather than a BOARD_X ifdef so Core/ stays board-agnostic (Docs/Code Style.md).
#ifndef MAX_PROVIDER_SUBS
#define MAX_PROVIDER_SUBS 20
#endif

// Docs "Provider table entry" (32 B wire): requesterAddr, trid, the shared subscription
// table, last sent, hash/hashlike, timeout. Active until canceled; not persistent.
struct ProviderEntry {
    uint16_t RequesterAddr = 0;  // 0 = invalid entry
    uint16_t Trid = 0;
    SubscriptionTable Sub;
    uint32_t LastSentMs = 0;
    uint32_t Hash = 0;           // FNV-1a hash / edge counter / last value / subresolution
    uint32_t Timeout = 0;        // local uptime deadline
    // Transient (not serialized): last boolean sample for edge detection and the last
    // counter value that was transmitted.
    bool LastBool = false;
    uint32_t SentCounter = 0;
    // Last SENT vector for a delta subscription on a Vector source (up to 3 axes); the
    // scalar delta reuses `hash` for the same purpose.
    int32_t LastVec[3] = {0, 0, 0};
};

// The wire prefix of the entry IS the struct prefix, so serialization is a plain copy of the
// first 32 bytes (Docs "Provider table entry"): requesterAddr, trid, table, lastSent, hash,
// timeout. The provider table is not ordered (only the requester table is TRID-sorted).
static_assert(offsetof(ProviderEntry, RequesterAddr) == 0, "wire order: requesterAddr first");
static_assert(offsetof(ProviderEntry, Trid) == 2, "wire order: Trid after requesterAddr");
static_assert(offsetof(ProviderEntry, Sub) == 4, "wire order: subscription table after Trid");
static_assert(offsetof(ProviderEntry, LastSentMs) == 20, "wire order: lastSent after the table");
static_assert(offsetof(ProviderEntry, Hash) == 24, "wire order: hash after lastSent");
static_assert(offsetof(ProviderEntry, Timeout) == 28, "wire order: timeout after hash");
#define PROVIDER_ENTRY_WIRE_SIZE 32
#endif // USE_SUB_PROVIDE
