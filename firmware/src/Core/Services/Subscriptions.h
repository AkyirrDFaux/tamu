#pragma once

// Subscription service (Docs/Services/Subscriptions.md): a value can be pushed from a
// provider register to a requester register on a trigger. The service is split by role -
// requester (Tamu) and provider (any device) - because a device normally has one of them.
// Parts are included here in order so the service stays one translation unit:
//   SubscriptionsDefs.h       shared tables/lookups
//   SubscriptionsRequester.h  requester core
//   SubscriptionsProvider.h   provider table and triggers
//   SubscriptionsPersist.h    requester persistence / re-registration
//   SubscriptionsControl.h    replies, management CIDs, tick

#include "Core/Services/SubscriptionsDefs.h"
#include "Core/Services/SubscriptionsRequester.h"
#include "Core/Services/SubscriptionsProvider.h"
#include "Core/Services/SubscriptionsPersist.h"
#include "Core/Services/SubscriptionsControl.h"
