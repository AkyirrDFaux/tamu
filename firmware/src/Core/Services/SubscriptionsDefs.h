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

// FNV-1a hash over the value bytes (provider on-change detection + requester confirmation).
// A generic hash over data - not service state - so it stays a free helper.
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
static_assert(sizeof(SubscriptionTable) == 16, "the subscription table is 16 Bytes on the wire");

#define SUB_TABLE_WIRE_SIZE 16

static inline uint32_t SubMinTime(const SubscriptionTable &t) {
    return (uint32_t)t.minTime[0] | ((uint32_t)t.minTime[1] << 8) | ((uint32_t)t.minTime[2] << 16);
}

// Docs: "Timeout 120s, renewed with new request." Both sides expire entries that stop being
// renewed (a value update / re-registration refreshes the deadline).
#define SUB_TIMEOUT_MS 120000u

static inline uint32_t SubTimeoutFrom(uint32_t nowMs) { return nowMs + SUB_TIMEOUT_MS; }

// ---------------------------------------------------------------------------
// Shared table search. The requester and provider tables are searched the same
// way; only what makes an entry "occupied" differs (RequesterEntry::active vs
// ProviderEntry::requesterAddr), so the role files supply that predicate.
// ---------------------------------------------------------------------------

// First occupied entry with `trid`, or nullptr.
template <typename T, size_t N, typename Occupied>
static T *SubTableFindByTrid(T (&table)[N], uint16_t Trid, Occupied occupied) {
    for (size_t i = 0; i < N; i++) {
        if (occupied(table[i]) && table[i].Trid == Trid) return &table[i];
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
    uint16_t providerAddr = 0;
    uint16_t Trid = 0;
    SubscriptionTable sub;
    uint32_t targetReg = 0;      // local write target
    uint32_t timeout = 0;        // local uptime deadline (not persisted)
    bool active = false;
    // Initialization tracking (transient): the entry is "initialized" once a value update
    // arrives; until then the requester re-sends the registration (CID 1) to wake the provider.
    uint32_t registeredAtMs = 0;   // start of the initialization window (set/boot)
    uint32_t lastRegisteredMs = 0; // last re-registration sent
    uint32_t lastValueMs = 0;      // uptime of the last received value (0 = never)
};

#define SUB_RETRY_MS          100   // re-registration interval while un-initialized
#define SUB_INIT_WINDOW_MS    10000 // give up re-registering after this long
#define SUB_KEEPALIVE_MS      60000 // renew the provider's 120 s lease once values flow

// The wire prefix of the entry IS the struct prefix, so serialization is a plain copy of the
// first 28 bytes (Docs "Requester table entry"): providerAddr, trid, table, targetReg, timeout.
static_assert(offsetof(RequesterEntry, providerAddr) == 0, "wire order: providerAddr first");
static_assert(offsetof(RequesterEntry, Trid) == 2, "wire order: Trid after providerAddr");
static_assert(offsetof(RequesterEntry, sub) == 4, "wire order: subscription table after Trid");
static_assert(offsetof(RequesterEntry, targetReg) == 20, "wire order: targetReg after the table");
static_assert(offsetof(RequesterEntry, timeout) == 24, "wire order: timeout after targetReg");
#define REQUESTER_ENTRY_WIRE_SIZE 28
#define REQUESTER_FILE_ENTRY_SIZE 24 // the wire entry minus the timeout (not persisted)

// The deferred re-registration queue capacity: a short address list (not a bitmask) keeps any
// 10-bit node id addressable, sized to the device registry's capacity (SNDB_MAX_ENTRIES) so a
// burst of assignments can never silently drop a re-push.
static const uint8_t kReRegisterMax = 128;
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
    uint16_t requesterAddr = 0;  // 0 = invalid entry
    uint16_t Trid = 0;
    SubscriptionTable sub;
    uint32_t lastSentMs = 0;
    uint32_t hash = 0;           // FNV-1a hash / edge counter / last value / subresolution
    uint32_t timeout = 0;        // local uptime deadline
    // Transient (not serialized): last boolean sample for edge detection and the last
    // counter value that was transmitted.
    bool lastBool = false;
    uint32_t sentCounter = 0;
    // Last SENT vector for a delta subscription on a Vector source (up to 3 axes); the
    // scalar delta reuses `hash` for the same purpose.
    int32_t lastVec[3] = {0, 0, 0};
};

// The wire prefix of the entry IS the struct prefix, so serialization is a plain copy of the
// first 32 bytes (Docs "Provider table entry"): requesterAddr, trid, table, lastSent, hash,
// timeout. The provider table is not ordered (only the requester table is TRID-sorted).
static_assert(offsetof(ProviderEntry, requesterAddr) == 0, "wire order: requesterAddr first");
static_assert(offsetof(ProviderEntry, Trid) == 2, "wire order: Trid after requesterAddr");
static_assert(offsetof(ProviderEntry, sub) == 4, "wire order: subscription table after Trid");
static_assert(offsetof(ProviderEntry, lastSentMs) == 20, "wire order: lastSent after the table");
static_assert(offsetof(ProviderEntry, hash) == 24, "wire order: hash after lastSent");
static_assert(offsetof(ProviderEntry, timeout) == 28, "wire order: timeout after hash");
#define PROVIDER_ENTRY_WIRE_SIZE 32
#endif // USE_SUB_PROVIDE
