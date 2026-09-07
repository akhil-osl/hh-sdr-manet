/*
 * Radio / SDR abstraction — the hardware-independent boundary.
 *
 * ==========================================================================
 * SCOPE AND WHAT THIS DELIBERATELY DOES NOT DEFINE
 * ==========================================================================
 * The FPGA/PL implementation and its software APIs are NOT available. This
 * header therefore defines only the *hardware-independent* contract the MANET
 * stack requires, expressed in terms the architecture already specifies
 * (HTI-02, HTI-03, HTI-04, HTI-05, HTI-14).
 *
 * It intentionally contains NO:
 *   - AXI register maps            - DMA descriptors or buffer APIs
 *   - PHY or modem control APIs    - RF transceiver / tuner APIs
 *   - modulation or RCC engine interfaces
 *   - sample-streaming interfaces
 *
 * Those belong below this line, inside a hardware adapter, and cannot be
 * specified until the PL contract exists. The HW/SW Interface Specification
 * §12 item 5 records the Radio/SDR Interface C ABI as TBD for exactly this
 * reason: "the source names the categories, not the API".
 *
 * Doc 1 §3 constrains this layer to "no MANET semantics, metric/byte plumbing
 * only" — so nothing here interprets a beacon, scores a link, or knows what a
 * route is. It moves opaque frames and reports measured metrics.
 *
 * The unit of exchange is a FRAME, not a sample buffer: the MANET control plane
 * has no use for baseband samples, and choosing frames keeps every DSP/PHY
 * decision below the boundary where it belongs.
 *
 * SCA 2.2.2: this interface is the Device façade (Doc 1 §3 classifies the
 * Radio/SDR Interface as a CF::Device). See include/hhsdr/sca/ for the
 * lifecycle and capacity model layered over it.
 * ==========================================================================
 */
#ifndef HHSDR_RADIO_RADIO_H
#define HHSDR_RADIO_RADIO_H

#include "hhsdr/core/events.h"
#include "hhsdr/core/types.h"

/* Maximum over-the-air frame payload this stack will hand to a radio.
 * A real value is a PHY/waveform property and is HARDWARE-DEPENDENT / TBD;
 * this bound exists so buffers can be statically sized. */
#define HH_RADIO_MAX_FRAME 512

typedef enum {
    HH_FRAME_BEACON = 1,   /* discovery/heartbeat (HTI-03/04) */
    HH_FRAME_DATA,         /* data-plane payload              */
    HH_FRAME_ROUTING       /* routing update (OGM-style)      */
} hh_frame_kind_t;

typedef struct {
    hh_frame_kind_t kind;
    hh_node_id_t    src;
    hh_node_id_t    dst;        /* HH_NODE_ID_INVALID == broadcast */
    uint16_t        len;
    uint8_t         data[HH_RADIO_MAX_FRAME];
} hh_frame_t;

/*
 * Radio operational status (HTI-02).
 * Payload schema is Recommended, not Defined — HTI spec §12 item 13.
 */
typedef struct {
    bool     operational;      /* false => own-radio failure (Doc 1 §7)      */
    uint32_t channel;          /* logical channel index; mapping to RF TBD   */
    float    frequency_hz;     /* 0 when unknown to the adapter              */
    uint32_t waveform_id;      /* encoding TBD (HTI spec §12 item 7)         */
    uint64_t frames_tx;
    uint64_t frames_rx;
    uint64_t tx_errors;
    uint64_t rx_errors;        /* CRC/decode failures: RF-interference input */
} hh_radio_status_t;

/* Inbound frame delivery. The adapter calls this from its receive path with the
 * frame plus the metrics measured for it (HTI-04 + HTI-05 arrive together
 * because both are properties of the same reception). */
typedef void (*hh_radio_rx_fn)(const hh_frame_t *frame,
                               const hh_link_sample_t *metrics,
                               void *ctx);

/*
 * The abstract radio contract. Production components depend ONLY on this
 * vtable, never on any concrete adapter. Implementing it is the entirety of
 * what future FPGA/PL integration must supply.
 */
typedef struct hh_radio_ops {
    const char *name;

    /* Lifecycle. open() is where an adapter would acquire hardware. */
    hh_status_t (*open)(void *self);
    hh_status_t (*close)(void *self);

    /* HTI-03: transmit one framed message. Fire-and-forget; a radio-level
     * failure is reported synchronously but never blocks the caller on RF. */
    hh_status_t (*transmit)(void *self, const hh_frame_t *frame);

    /* HTI-04/05: register the inbound frame+metrics callback. */
    hh_status_t (*set_rx_callback)(void *self, hh_radio_rx_fn fn, void *ctx);

    /* HTI-02: current radio status. */
    hh_status_t (*get_status)(void *self, hh_radio_status_t *out);

    /* HTI-05: per-neighbor metrics the adapter maintains independently of frame
     * arrival (e.g. periodic PHY sampling). HH_ERR_NOTFOUND if the adapter has
     * no sample for that neighbor; HH_ERR_UNSUPPORTED if it cannot sample. */
    hh_status_t (*get_link_metrics)(void *self, hh_node_id_t neighbor,
                                    hh_link_sample_t *out);

    /* HTI-14: request a channel change. Self-Healing prefers this over route
     * churn when the cause hint is RF interference (Doc 1 §8). Adapters without
     * channel agility return HH_ERR_UNSUPPORTED, and the caller falls back to
     * route-based recovery (HTI spec §12 item 11 records that fallback as TBD;
     * this stack chooses route-based recovery and logs the decision). */
    hh_status_t (*set_channel)(void *self, uint32_t channel);

    /* Service the adapter: deliver any pending receptions. Called from the
     * control loop so the stack needs no thread of its own at this boundary. */
    hh_status_t (*poll)(void *self, hh_time_ms_t now);
} hh_radio_ops_t;

/* Handle pairing a vtable with its instance state. */
typedef struct {
    const hh_radio_ops_t *ops;
    void                 *self;
} hh_radio_t;

/* Thin inline forwarders so call sites read plainly and NULL-op entries in a
 * partial adapter degrade to HH_ERR_UNSUPPORTED rather than crashing. */
static inline hh_status_t hh_radio_open(hh_radio_t *r)
{ return (r && r->ops && r->ops->open) ? r->ops->open(r->self) : HH_ERR_INVAL; }

static inline hh_status_t hh_radio_close(hh_radio_t *r)
{ return (r && r->ops && r->ops->close) ? r->ops->close(r->self) : HH_ERR_INVAL; }

static inline hh_status_t hh_radio_transmit(hh_radio_t *r, const hh_frame_t *f)
{ return (r && r->ops && r->ops->transmit) ? r->ops->transmit(r->self, f) : HH_ERR_INVAL; }

static inline hh_status_t hh_radio_set_rx_callback(hh_radio_t *r, hh_radio_rx_fn fn, void *ctx)
{ return (r && r->ops && r->ops->set_rx_callback) ? r->ops->set_rx_callback(r->self, fn, ctx) : HH_ERR_INVAL; }

static inline hh_status_t hh_radio_get_status(hh_radio_t *r, hh_radio_status_t *out)
{ return (r && r->ops && r->ops->get_status) ? r->ops->get_status(r->self, out) : HH_ERR_INVAL; }

static inline hh_status_t hh_radio_get_link_metrics(hh_radio_t *r, hh_node_id_t n, hh_link_sample_t *out)
{ return (r && r->ops && r->ops->get_link_metrics) ? r->ops->get_link_metrics(r->self, n, out) : HH_ERR_UNSUPPORTED; }

static inline hh_status_t hh_radio_set_channel(hh_radio_t *r, uint32_t ch)
{ return (r && r->ops && r->ops->set_channel) ? r->ops->set_channel(r->self, ch) : HH_ERR_UNSUPPORTED; }

static inline hh_status_t hh_radio_poll(hh_radio_t *r, hh_time_ms_t now)
{ return (r && r->ops && r->ops->poll) ? r->ops->poll(r->self, now) : HH_OK; }

#endif /* HHSDR_RADIO_RADIO_H */
