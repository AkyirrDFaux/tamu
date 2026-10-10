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
    void Init();                           // resets the service state
    bool Handle(const PacketFrame &frame); // CID dispatch; true if consumed
    void Tick(uint32_t nowMs);             // main-loop trigger evaluation + expiry

#ifdef USE_SUB_REQUEST
    // Provider-side re-registration of every (or one node's) restored requester entry.
    void ReRegister();
    void ReRegisterForNode(uint16_t addr);
    void RequestReRegister(uint16_t addr);
    void LoadRequesterTable(); // boot restore (Dispatcher.h LoadAllBackups)
#endif

private:
    // ---------------------------------------------------------------------
    // Provider state + helpers (USE_SUB_PROVIDE). Active until canceled, not persistent.
    // ---------------------------------------------------------------------
#ifdef USE_SUB_PROVIDE
    ProviderEntry ProviderTable[MAX_PROVIDER_SUBS];

    static bool ProviderOccupied(const ProviderEntry &e);
    static bool ProviderEntryLive(const uint8_t *table, uint16_t i);
    ProviderEntry* ProviderFindByTrid(uint16_t trid);
    ProviderEntry* ProviderFindFree();
    void ProviderClearEntry(ProviderEntry* e);
    void ProviderApply(ProviderEntry* e, uint16_t trid, uint16_t requesterAddr,
                       const SubscriptionTable &t, bool reset);
    ProviderEntry* ProviderInstall(uint16_t trid, uint16_t requesterAddr,
                                   const SubscriptionTable &t, bool reset = false);
    void HandleProviderConfirmation(const PacketFrame &frame);
    uint32_t SubscriptionsDeltaHash(const FieldResult &fr, uint32_t deadzone);
    uint32_t SubscriptionsAbsDelta(int32_t now, int32_t last);
    bool SubscriptionsIsScalar(const FieldResult &fr);
#ifndef SCALAR_ONLY
    uint32_t SubscriptionsVectorDist2(const FieldResult &fr, const int32_t *last);
#endif
    void EvaluateProviderTriggers(uint32_t nowMs);
    void ProviderStreamTable(const PacketFrame &frame);
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
    uint16_t ReRegisterPending[kReRegisterMax];
    uint8_t ReRegisterCount = 0;

    static bool RequesterOccupied(const RequesterEntry &e);
    static bool RequesterEntryLive(const uint8_t *table, uint16_t i);
    RequesterEntry* RequesterFindByTrid(uint16_t trid);
    void RequesterClearEntry(RequesterEntry* e);
    void RequesterRemove(RequesterEntry* e);
    RequesterEntry* RequesterAlloc(uint16_t trid);
    RequesterEntry* RequesterUpsert(uint16_t trid);
    void ApplyRequesterValue(RequesterEntry *e, const uint8_t *val, uint8_t vlen, bool confirm);
    void HandleRequesterValueUpdate(const PacketFrame &frame);
    void SaveRequesterTable();
    void RegisterRequesterProvider(RequesterEntry* e);
    void SubscriptionsReRegisterPending();
    void RequesterInitCheck(uint32_t nowMs);
    void RequesterStreamTable(const PacketFrame &frame);
    void SubscriptionsCancelProvider(uint16_t providerAddr, uint16_t trid);
#endif

    // ---------------------------------------------------------------------
    // Shared helpers (both roles).
    // ---------------------------------------------------------------------
    bool SubscriptionsHighPriority(TriggerType t);
    FieldResult SubscriptionsGetField(uint32_t blockInfo);
    void SubReply(const PacketFrame &frame, const uint8_t *payload, uint16_t len,
                  uint8_t extraFlags = 0);
    void StreamSubscriptionTable(const PacketFrame &frame, const uint8_t *table, uint16_t max,
                                 uint16_t entrySize, bool (*occupied)(const uint8_t *, uint16_t));
};

// One instance per binary, in Core/Services/.
SubscriptionsService Subscriptions;

// ---------------------------------------------------------------------------
// Temporary forwarders: the device main loops (SubscriptionsTick/ReRegisterSubscriptions)
// and the Device service (SubscriptionsRequestReRegister) still call the old free-function names.
// ---------------------------------------------------------------------------
#ifdef USE_SUB_REQUEST
inline void SubscriptionsRequestReRegister(uint16_t addr) { Subscriptions.RequestReRegister(addr); }
#endif

inline void SubscriptionsService::Init()
{
#ifdef USE_SUB_PROVIDE
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) ProviderTable[i] = ProviderEntry{};
#endif
#ifdef USE_SUB_REQUEST
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) RequesterTable[i] = RequesterEntry{};
    for (uint8_t i = 0; i < kReRegisterMax; i++) ReRegisterPending[i] = 0;
    ReRegisterCount = 0;
#endif
}

// True for the high-priority triggers (confirmation and edge detection).
inline bool SubscriptionsService::SubscriptionsHighPriority(TriggerType t)
{
    return t == TriggerType::OnChangeConfirm || t == TriggerType::EdgeRising ||
           t == TriggerType::EdgeFalling || t == TriggerType::EdgeAny;
}

// Reads the current value of the register addressed by a 32-bit BlockInfo. Delegates the
// script/dynamic/static routing to the register service (RegisterResolveByBlockInfo) so the
// two services cannot drift; the returned Data points at the source value (or is null when
// the BlockInfo does not resolve).
inline FieldResult SubscriptionsService::SubscriptionsGetField(uint32_t blockInfo)
{
    FieldResult fr;
    RegisterResolveByBlockInfo(blockInfo, fr);
    return fr;
}

#include "Core/Services/SubscriptionsRequester.h"
#include "Core/Services/SubscriptionsProvider.h"
#include "Core/Services/SubscriptionsPersist.h"
#include "Core/Services/SubscriptionsControl.h"
