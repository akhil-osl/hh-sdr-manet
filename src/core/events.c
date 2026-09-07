#include "hhsdr/core/events.h"

const char *hh_event_type_str(hh_event_type_t t)
{
    switch (t) {
    case HH_EV_NONE:               return "none";
    case HH_EV_BEACON_RX:          return "beacon_rx";
    case HH_EV_LINK_SAMPLE:        return "link_sample";
    case HH_EV_NEIGHBOR_UP:        return "neighbor_up";
    case HH_EV_NEIGHBOR_DOWN:      return "neighbor_down";
    case HH_EV_NEIGHBOR_CHANGED:   return "neighbor_changed";
    case HH_EV_LINK_STATE_CHANGED: return "link_state_changed";
    case HH_EV_ROUTE_INSTALLED:    return "route_installed";
    case HH_EV_ROUTE_WITHDRAWN:    return "route_withdrawn";
    case HH_EV_FAILURE_DETECTED:   return "failure_detected";
    case HH_EV_RECOVERY_STARTED:   return "recovery_started";
    case HH_EV_RECOVERY_COMPLETED: return "recovery_completed";
    case HH_EV_PARTITION_DETECTED: return "partition_detected";
    case HH_EV_NETWORK_MERGED:     return "network_merged";
    case HH_EV_CADENCE_HINT:       return "cadence_hint";
    case HH_EV__MAX:               break;
    }
    return "unknown";
}

const char *hh_withdraw_reason_str(hh_withdraw_reason_t r)
{
    switch (r) {
    case HH_WITHDRAW_EXPIRED:          return "expired";
    case HH_WITHDRAW_FAILURE_CASCADE:  return "failure_cascade";
    case HH_WITHDRAW_EXPLICIT:         return "explicit";
    }
    return "unknown";
}

const char *hh_recovery_strategy_str(hh_recovery_strategy_t s)
{
    switch (s) {
    case HH_RECOVERY_ALTERNATE_ROUTE: return "alternate_route";
    case HH_RECOVERY_REDISCOVERY:     return "rediscovery";
    case HH_RECOVERY_CHANNEL_CHANGE:  return "channel_change";
    }
    return "unknown";
}
