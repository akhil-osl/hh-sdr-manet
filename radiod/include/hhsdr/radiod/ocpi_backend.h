/*
 * radiod OpenCPI backend — radiod as the owner of an OpenCPI application.
 *
 * This is the backend that makes radiod the single PL owner the target
 * architecture requires (Note 1: "only radiod opens OpenCPI"). It implements
 * the same hh_radio_ops_t vtable as the mock backend, so radiod's state
 * machine, IPC and control loop do not change when it is selected.
 *
 * WHAT IT DOES
 *   open  : create the OpenCPI application from an application XML file,
 *           initialize it, apply the configured property values, start it.
 *   close : stop the application and release it. Safe to call twice.
 *   poll  : notice when the application has finished on its own, and report
 *           the radio as not operational from then on.
 *   get_status : operational = application created, started, not finished.
 *
 * The open sequence is the one the OpenCPI radio project's own ACI program
 * uses (txrx_worker_rt_ch2_dma, applications/dma_stream/dma_stream.cc):
 * construct -> initialize -> setProperty -> start. Properties are written
 * between initialize and start so the first data already carries them.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   - Name any worker, property or application. All of them come from
 *     configuration, so nothing about a particular waveform is compiled in.
 *   - Map a channel index to a frequency. set_channel returns
 *     HH_ERR_UNSUPPORTED: the channel plan is undefined (unknown.md U-14).
 *   - Carry frames. transmit returns HH_ERR_NOT_IMPLEMENTED: whether the data
 *     plane runs through radiod's process is undecided (unknown.md U-15).
 *   - Report frame or error counters. No application property is defined as
 *     their source, so they stay zero rather than being guessed.
 *   - Run board bring-up (ad9361_init, a separate radio-setup application).
 *     Whether radiod owns those steps is an open question (unknown.md U-19).
 *
 * THREADS AND ALLOCATION
 *   radiod calls every function here from its single control thread. The
 *   OpenCPI runtime, however, runs its own container threads inside the
 *   process once an application exists, and allocates on the heap during
 *   open/close. Both are properties of the OpenCPI library, not of radiod's
 *   control path, and are recorded against unknown.md U-12.
 *
 * Only this header is plain C and always available. The implementation is
 * C++ (the OpenCPI ACI has no C binding) and is built only when the project
 * is configured with -DHH_WITH_OPENCPI=ON.
 */
#ifndef HHSDR_RADIOD_OCPI_BACKEND_H
#define HHSDR_RADIOD_OCPI_BACKEND_H

#include "hhsdr/radio/radio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HH_OCPI_MAX_PATH   256
#define HH_OCPI_MAX_PROPS  16
#define HH_OCPI_MAX_NAME   64
#define HH_OCPI_MAX_VALUE  128
#define HH_OCPI_MAX_ERROR  256

/* One property value written after initialize and before start. */
typedef struct {
    char instance[HH_OCPI_MAX_NAME];
    char property[HH_OCPI_MAX_NAME];
    char value[HH_OCPI_MAX_VALUE];
} hh_ocpi_prop_t;

typedef struct {
    /* Application XML handed to the ACI. Required. */
    char           app_path[HH_OCPI_MAX_PATH];

    /* When non-empty, exported as OCPI_LIBRARY_PATH before the application is
     * created. Empty means "use the environment radiod was started with". */
    char           library_path[HH_OCPI_MAX_PATH];

    hh_ocpi_prop_t props[HH_OCPI_MAX_PROPS];
    size_t         prop_count;
} hh_ocpi_config_t;

/* Append one property from "instance.property=value" (the form the config
 * key ocpi_property takes). HH_ERR_INVAL if the text is malformed or a field
 * is too long; HH_ERR_NOMEM once HH_OCPI_MAX_PROPS are held. */
hh_status_t hh_ocpi_config_add_property(hh_ocpi_config_t *cfg, const char *spec);

typedef struct {
    hh_ocpi_config_t cfg;

    /* The OpenCPI application (OCPI::API::Application *), owned. NULL whenever
     * the backend is closed. Opaque here because this header is C. */
    void            *app;

    bool             finished;    /* application ended on its own */

    hh_radio_rx_fn   rx_fn;       /* stored, never invoked: no frames (U-15) */
    void            *rx_ctx;

    /* Text of the most recent OpenCPI error, for logs and tests. */
    char             last_error[HH_OCPI_MAX_ERROR];
} hh_ocpi_backend_t;

/* Bind the backend to `out` and copy `cfg`. Acquires nothing: the application
 * is created by open(). HH_ERR_INVAL if cfg has no application path. */
hh_status_t hh_ocpi_backend_init(hh_ocpi_backend_t *b, const hh_ocpi_config_t *cfg,
                                 hh_radio_t *out);

/* Read one property of a running application as text. HH_ERR_STATE when no
 * application is open; HH_ERR_NOTFOUND when OpenCPI rejects the name;
 * HH_ERR_NOMEM when the value does not fit in `cap`. */
hh_status_t hh_ocpi_backend_get_property(hh_ocpi_backend_t *b, const char *instance,
                                         const char *property, char *buf, size_t cap);

/* Most recent OpenCPI error text, or "" if none. */
const char *hh_ocpi_backend_last_error(const hh_ocpi_backend_t *b);

#ifdef __cplusplus
}
#endif

#endif /* HHSDR_RADIOD_OCPI_BACKEND_H */
