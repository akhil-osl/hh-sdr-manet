/*
 * radiod worker-property control surface.
 *
 * ==========================================================================
 * WHY THIS INDIRECTION EXISTS
 * ==========================================================================
 * hh_radio_ops_t (radio.h) is the RF/frame contract: open, transmit, status,
 * channel. It has no notion of a named, typed, individually addressable
 * property, and it should not — adding one would put a signal-processing
 * worker's configuration namespace into the radio abstraction that the MANET
 * stack depends on.
 *
 * So the opencpi_* verbs are serviced through this second, orthogonal vtable.
 * A backend may implement neither, either, or both: the mock backend
 * implements only hh_radio_ops_t, and the OpenCPI backend implements both.
 *
 * THE DESIGN RULE THIS ENFORCES: radiod knows no property names.
 * Not one worker name, property name, property type or range appears in
 * radiod. The backend publishes its own control surface through list(), and
 * radiod's only additions are (a) routing the verb and (b) refusing a write
 * to a start-time-only property while the application is running — which it
 * does from the access string the backend reported, not from a table of its
 * own. A new worker, or a new property on an existing worker, is therefore
 * reachable through the existing verbs with no change here and none in
 * radiod.c.
 * ==========================================================================
 */
#ifndef HHSDR_RADIOD_PROPS_H
#define HHSDR_RADIOD_PROPS_H

#include "hhsdr/core/types.h"
#include "hhsdr/protocol/rc.h"

/* Access classes a backend may report for a property. The strings are what
 * travel on the wire and what `opencpi list <worker>` prints. */
#define HH_PROPS_ACCESS_RW       "rw"        /* writable at any time         */
#define HH_PROPS_ACCESS_RW_START "rw-start"  /* writable only while stopped  */
#define HH_PROPS_ACCESS_RO       "ro"        /* readable only               */

/*
 * Every operation fills `resp` (items/count, or worker/prop/value/type/access)
 * on success, and `resp->text` with a sentence on failure. Returning anything
 * but HH_OK makes radiod answer `err <verb> reason=<status> text=<that
 * sentence>`, so the reason a specific write was refused reaches the operator
 * instead of being flattened to a status code.
 *
 * `running` tells set() whether the application is live, so the backend can
 * choose between applying a value now and staging it for the next start. It is
 * passed rather than queried because radiod's lifecycle state is the
 * authority on it.
 */
typedef struct hh_radiod_props_ops {
    const char *name;

    /* worker == NULL or "" : list the workers. Otherwise list that worker's
     * properties as "name:type:access" items. HH_ERR_NOTFOUND for a worker
     * the backend does not publish. */
    hh_status_t (*list)(void *self, const char *worker, hh_rc_response_t *resp);

    /* Read one property. HH_ERR_NOTFOUND if worker/prop is unknown,
     * HH_ERR_STATE if the value cannot be read in the current state. */
    hh_status_t (*get)(void *self, const char *worker, const char *prop,
                       hh_rc_response_t *resp);

    /* Write one property. HH_ERR_UNSUPPORTED for a read-only property,
     * HH_ERR_STATE for a start-time-only property while running,
     * HH_ERR_INVAL for a value the backend or the framework rejects. */
    hh_status_t (*set)(void *self, const char *worker, const char *prop,
                       const char *value, bool running, hh_rc_response_t *resp);

    /* Sample everything readable in one exchange, as "name:value" items. */
    hh_status_t (*status)(void *self, bool running, hh_rc_response_t *resp);
} hh_radiod_props_ops_t;

/* Handle pairing the vtable with its instance state, matching hh_radio_t. */
typedef struct {
    const hh_radiod_props_ops_t *ops;
    void                        *self;
} hh_radiod_props_t;

#endif /* HHSDR_RADIOD_PROPS_H */
