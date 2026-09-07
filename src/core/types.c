#include "hhsdr/core/types.h"

const char *hh_status_str(hh_status_t s)
{
    switch (s) {
    case HH_OK:                   return "OK";
    case HH_ERR_INVAL:            return "EINVAL";
    case HH_ERR_NOMEM:            return "ENOMEM";
    case HH_ERR_NOTFOUND:         return "ENOTFOUND";
    case HH_ERR_AGAIN:            return "EAGAIN";
    case HH_ERR_STATE:            return "ESTATE";
    case HH_ERR_UNSUPPORTED:      return "EUNSUPPORTED";
    case HH_ERR_NOT_IMPLEMENTED:  return "ENOTIMPL";
    case HH_ERR_IO:               return "EIO";
    }
    return "EUNKNOWN";
}

const char *hh_link_state_str(hh_link_state_t s)
{
    switch (s) {
    case HH_LINK_HEALTHY:            return "Healthy";
    case HH_LINK_DEGRADED:           return "Degraded";
    case HH_LINK_SUSPECTED_FAILURE:  return "SuspectedFailure";
    case HH_LINK_FAILED:             return "Failed";
    case HH_LINK_RECOVERING:         return "Recovering";
    }
    return "Unknown";
}

const char *hh_cause_hint_str(hh_cause_hint_t c)
{
    switch (c) {
    case HH_CAUSE_UNKNOWN:            return "unknown";
    case HH_CAUSE_NODE_FAILURE:       return "node-failure";
    case HH_CAUSE_RF_INTERFERENCE:    return "rf-interference";
    case HH_CAUSE_MOBILITY:           return "mobility";
    case HH_CAUSE_OWN_RADIO_FAILURE:  return "own-radio-failure";
    case HH_CAUSE_ASYMMETRIC_LINK:    return "asymmetric-link";
    }
    return "unknown";
}
