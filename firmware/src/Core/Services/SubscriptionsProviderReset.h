#pragma once

#include <cstdint>

#include "Core/Types/Enums.h"

// The provider's RE-INSTALL decision: does applying a subscription table have to throw away the
// trigger state the entry holds (LastSentMs, Hash - the edge counter with the confirmed counter in
// its high half - LastBool, LastVec)?
//
// A subscription is kept alive by repeating the Set Subscription command (0401) with the same entry
// (Docs/Services/Subscriptions.md:27, "there is no separate renewal command"), and the owner's ruling
// (2026-10-25) makes that a timeout extension and nothing else: "The renewal should only extend the
// timeouts if the subscription was already running... nothing else special should happen." So an
// identical re-install must NOT reset, or the counter restarts under the requester (a backward jump
// it reads as a provider restart), a counted-but-unsent edge is erased, and a held button looks like
// a fresh level change.
//
// What DOES reset, because the stored state no longer means anything:
//   - the explicit 0421 Set (`Reset`): an app-driven re-install, not a renewal;
//   - a new entry (`NewEntry`, requesterAddr 0): there is no state to keep;
//   - a different SOURCE register: the hash/counter/last value describe the old field;
//   - a different TRIGGER: an edge counter and a value hash are not the same accounting, and
//     LastBool belongs to the edge triggers only.
//
// What deliberately does NOT reset: minTime, period and deadzone. They tune WHEN a trigger sends,
// not what the stored state means, and resetting the counter for an interval tweak would be exactly
// the state change a renewal must never cause. (An app-driven re-install goes through 0421, which
// passes `Reset` - so a deliberate retune still starts clean.)
//
// Host-clean on purpose, in its own header: ProviderApply pulls the packet/register/device layer and
// does not link on the host, so the native test drives THIS decision directly (test/native/
// renewal_test.cpp) and SubscriptionsProvider.h calls it, which is what keeps the test and the device
// from drifting apart.
static inline bool SubProviderReinstallResets(bool Reset, bool NewEntry, uint32_t OldSourceReg,
                                              TriggerType OldTrigger, uint32_t NewSourceReg,
                                              TriggerType NewTrigger) {
    return Reset || NewEntry || OldSourceReg != NewSourceReg || OldTrigger != NewTrigger;
}
