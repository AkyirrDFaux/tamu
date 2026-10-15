#pragma once

// Subscription service (Docs/Services/Subscriptions.md): a value can be pushed from a
// provider register to a requester register on a trigger. One class, one instance
// (`Subscriptions`, below); the implementation is split by role and included after the class
// so the service stays one translation unit:
//   SubscriptionsDefs.h       shared tables/lookups + the entry wire structs (before the class)
//   SubscriptionsRequester.h  requester core
//   SubscriptionsProvider.h   provider table and triggers
//   SubscriptionsPersist.h    requester persistence / re-registration
//   SubscriptionsControl.h    replies, management CIDs, tick

#include "Core/Functions/Packet.h"
#include "Core/Services/SubscriptionsDefs.h"

class SubscriptionsService
{
public:
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed
    void Tick(uint32_t NowMs);             // main-loop trigger evaluation + expiry

#ifdef USE_SUB_REQUEST
    // Provider-side re-registration of every (or one node's) restored requester entry.
    void ReRegister();
    void ReRegisterForNode(uint16_t Addr);
    void RequestReRegister(uint16_t Addr);
    void LoadRequesterTable(); // boot restore (Dispatcher.h LoadAllBackups)
#endif

private:
    // ---------------------------------------------------------------------
    // Provider state + helpers (USE_SUB_PROVIDE). Active until canceled, not persistent.
    // ---------------------------------------------------------------------
#ifdef USE_SUB_PROVIDE
    ProviderEntry ProviderTable[MAX_PROVIDER_SUBS];

    static bool ProviderOccupied(const ProviderEntry &Entry);
    static bool ProviderEntryLive(const uint8_t *Entry);
    ProviderEntry* ProviderFindByTrid(uint16_t Trid);
    ProviderEntry* ProviderFindFree();
    void ProviderClearEntry(ProviderEntry* Entry);
    void ProviderApply(ProviderEntry* Entry, uint16_t Trid, uint16_t RequesterAddr,
                       const SubscriptionTable &Table, bool Reset);
    ProviderEntry* ProviderInstall(uint16_t Trid, uint16_t RequesterAddr,
                                   const SubscriptionTable &Table, bool Reset = false);
    void HandleProviderConfirmation(const PacketFrame &Frame);
    uint32_t SubscriptionsDeltaHash(const FieldResult &Fr, uint32_t Deadzone);
    uint32_t SubscriptionsAbsDelta(int32_t NowMs, int32_t Last);
    bool SubscriptionsIsScalar(const FieldResult &Fr);
#ifndef SCALAR_ONLY
    uint32_t SubscriptionsVectorDist2(const FieldResult &Fr, const int32_t *Last);
#endif
    void EvaluateProviderTriggers(uint32_t NowMs);
    void ProviderStreamTable(const PacketFrame &Frame);
#endif

    // ---------------------------------------------------------------------
    // Requester state (USE_SUB_REQUEST) + persistence / re-registration state.
    // ---------------------------------------------------------------------
#ifdef USE_SUB_REQUEST
    RequesterEntry RequesterTable[MAX_REQUESTER_SUBS];
    // 8-byte storage name. The storage layer keys files by exactly 8 bytes; the trailing NUL is
    // replaced by a space on flash (PackName padding) and by '~' for WriteBackupFile's staging
    // name (".SUBREQ~").
    static constexpr char SubscriptionsRequesterFile[8] = {'.','S','U','B','R','E','Q','\0'};
    uint16_t ReRegisterPending[KReRegisterMax];
    uint8_t ReRegisterCount = 0;
    // Round-robin cursor for the unserved-entry re-registration: one entry per tick, so a send
    // cannot put several blocking transmits back to back with no bus service between them.
    uint8_t ReRegisterCursor = 0;

    static bool RequesterOccupied(const RequesterEntry &Entry);
    static bool RequesterEntryLive(const uint8_t *Entry);
    RequesterEntry* RequesterFindByTrid(uint16_t Trid);
    uint16_t RequesterProviderAddr(const RequesterEntry &Entry);
    void RequesterClearEntry(RequesterEntry* Entry);
    void RequesterRemove(RequesterEntry* Entry);
    RequesterEntry* RequesterAlloc(uint16_t Trid);
    RequesterEntry* RequesterUpsert(uint16_t Trid);
    void ApplyRequesterValue(RequesterEntry *Entry, const uint8_t *Val, uint8_t Vlen, bool Confirm);
    void HandleRequesterValueUpdate(const PacketFrame &Frame);
    bool SaveRequesterTable(bool KeepFileEntries = false); // false when a write failed
    void RegisterRequesterProvider(RequesterEntry* Entry);
    void SubscriptionsReRegisterPending();
    void RequesterInitCheck(uint32_t NowMs);
    void RequesterStreamTable(const PacketFrame &Frame);
    void SubscriptionsCancelProvider(uint16_t ProviderAddr, uint16_t Trid);
#endif

    // ---------------------------------------------------------------------
    // Shared helpers (both roles).
    // ---------------------------------------------------------------------
    bool SubscriptionsHighPriority(TriggerType Trigger);
    FieldResult SubscriptionsGetField(uint32_t BlockInfo);
    void SubReply(const PacketFrame &Frame, const uint8_t *Payload, uint16_t Len,
                  uint8_t ExtraFlags = 0);
    void StreamSubscriptionTable(const PacketFrame &Frame, const uint8_t *Table, uint16_t Max,
                                 uint16_t EntrySize, uint16_t Stride,
                                 bool (*Occupied)(const uint8_t *));
};

// One instance per binary, in Core/Services/.
SubscriptionsService Subscriptions;

// ---------------------------------------------------------------------------
// Include-cycle workaround: Device.h must request a re-registration (its response handler)
// but cannot include Subscriptions.h - the service's headers already pull in Device.h - so it
// declares SubscriptionsRequestReRegister and this forwarder supplies the definition.
// ---------------------------------------------------------------------------
#ifdef USE_SUB_REQUEST
inline void SubscriptionsRequestReRegister(uint16_t Addr) { Subscriptions.RequestReRegister(Addr); }
#endif

// True for the high-priority triggers (confirmation and edge detection).
inline bool SubscriptionsService::SubscriptionsHighPriority(TriggerType Trigger)
{
    return Trigger == TriggerType::OnChangeConfirm || Trigger == TriggerType::EdgeRising ||
           Trigger == TriggerType::EdgeFalling || Trigger == TriggerType::EdgeAny;
}

// Reads the current value of the register addressed by a 32-bit BlockInfo. Delegates the
// script/dynamic/static routing to the register service (RegisterResolveByBlockInfo) so the
// two services cannot drift; the returned Data points at the source value (or is null when
// the BlockInfo does not resolve).
inline FieldResult SubscriptionsService::SubscriptionsGetField(uint32_t BlockInfo)
{
    FieldResult Fr;
    RegisterResolveByBlockInfo(BlockInfo, Fr);
    return Fr;
}

#include "Core/Services/SubscriptionsRequester.h"
#include "Core/Services/SubscriptionsProvider.h"
#include "Core/Services/SubscriptionsPersist.h"
#include "Core/Services/SubscriptionsControl.h"
