/*
 * radiod OpenCPI backend — implementation. See ocpi_backend.h for scope.
 *
 * C++ because the OpenCPI Application Control Interface (ACI) is C++ only:
 * OpenCPI 2.4.7 ships an aci/OcpiApi.h, but it declares nothing callable.
 * This file is the only C++ in radiod, and nothing C++ crosses its boundary:
 * every entry point is extern "C" and every exception is caught here and
 * turned into an hh_status_t plus a log record.
 */
#include "OcpiApi.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

extern "C" {
#include "hhsdr/core/log.h"
#include "hhsdr/radiod/ocpi_backend.h"
}

namespace OA = OCPI::API;

namespace {

const char *const COMP = "ocpi_backend";

hh_ocpi_backend_t *self_of(void *self) { return static_cast<hh_ocpi_backend_t *>(self); }
OA::Application *app_of(hh_ocpi_backend_t *b) { return static_cast<OA::Application *>(b->app); }

void remember_error(hh_ocpi_backend_t *b, const std::string &what)
{
    std::snprintf(b->last_error, sizeof b->last_error, "%s", what.c_str());
}

/* Run one ACI call and translate whatever it throws. The ACI reports errors
 * as thrown std::string (verified against OpenCPI 2.4.7); std::exception and
 * anything else are caught too so no exception can escape into C. */
template <typename Fn>
hh_status_t guarded(hh_ocpi_backend_t *b, const char *step, Fn fn)
{
    try {
        fn();
        return HH_OK;
    } catch (std::string &e) {
        remember_error(b, e);
    } catch (std::exception &e) {
        remember_error(b, e.what());
    } catch (...) {
        remember_error(b, "unknown exception");
    }
    HH_LOGE(COMP, "aci_error", "step=%s app=%s error=\"%s\"", step, b->cfg.app_path,
            b->last_error);
    return HH_ERR_IO;
}

void destroy_app(hh_ocpi_backend_t *b)
{
    OA::Application *app = app_of(b);
    b->app = nullptr;
    /* The destructor tears down the OpenCPI containers. It is not expected to
     * throw, but it runs OpenCPI code, so it is guarded like everything else. */
    guarded(b, "release", [&] { delete app; });
}

hh_status_t ocpi_open(void *self)
{
    hh_ocpi_backend_t *b = self_of(self);
    OA::Application *app = nullptr;
    hh_status_t st;

    if (b->app) return HH_ERR_STATE;
    b->finished = false;
    b->last_error[0] = '\0';

    if (b->cfg.library_path[0] != '\0')
        setenv("OCPI_LIBRARY_PATH", b->cfg.library_path, 1);

    st = guarded(b, "create", [&] { app = new OA::Application(b->cfg.app_path); });
    if (st != HH_OK) return st;
    b->app = app;

    st = guarded(b, "initialize", [&] { app->initialize(); });
    for (size_t i = 0; st == HH_OK && i < b->cfg.prop_count; i++) {
        const hh_ocpi_prop_t &p = b->cfg.props[i];
        st = guarded(b, "set_property", [&] {
            app->setProperty(p.instance, p.property, p.value);
        });
        if (st == HH_OK)
            HH_LOGI(COMP, "property", "instance=%s property=%s value=%s", p.instance,
                    p.property, p.value);
    }
    if (st == HH_OK) st = guarded(b, "start", [&] { app->start(); });

    /* A half-built application holds OpenCPI resources (on the board, the DMA
     * block), so any failure releases it rather than leaving it for close(). */
    if (st != HH_OK) {
        destroy_app(b);
        return st;
    }
    HH_LOGI(COMP, "started", "app=%s properties=%zu", b->cfg.app_path, b->cfg.prop_count);
    return HH_OK;
}

hh_status_t ocpi_close(void *self)
{
    hh_ocpi_backend_t *b = self_of(self);
    hh_status_t st = HH_OK;

    if (!b->app) return HH_OK;   /* closed already, or never opened */
    /* An application that finished on its own has nothing left to stop. */
    if (!b->finished) st = guarded(b, "stop", [&] { app_of(b)->stop(); });
    destroy_app(b);
    HH_LOGI(COMP, "stopped", "app=%s status=%s", b->cfg.app_path, hh_status_str(st));
    return st;
}

hh_status_t ocpi_transmit(void *self, const hh_frame_t *frame)
{
    (void)self; (void)frame;
    /* Frames are data plane. Whether they enter the OpenCPI application
     * through radiod's process is the undecided U-15, so this refuses rather
     * than choosing a port. */
    HH_LOGW(COMP, "not_implemented", "op=transmit reason=\"data plane undecided, unknown.md U-15\"");
    return HH_ERR_NOT_IMPLEMENTED;
}

hh_status_t ocpi_set_rx_callback(void *self, hh_radio_rx_fn fn, void *ctx)
{
    hh_ocpi_backend_t *b = self_of(self);
    b->rx_fn = fn;
    b->rx_ctx = ctx;
    return HH_OK;
}

hh_status_t ocpi_get_status(void *self, hh_radio_status_t *out)
{
    hh_ocpi_backend_t *b = self_of(self);
    if (!out) return HH_ERR_INVAL;
    std::memset(out, 0, sizeof *out);
    /* Only what the backend actually knows. Channel, frequency, waveform and
     * the counters have no defined application property behind them, so they
     * stay zero — which radio.h defines as "unknown". */
    out->operational = b->app != nullptr && !b->finished;
    return HH_OK;
}

hh_status_t ocpi_get_link_metrics(void *self, hh_node_id_t neighbor, hh_link_sample_t *out)
{
    (void)self; (void)neighbor; (void)out;
    return HH_ERR_UNSUPPORTED;
}

hh_status_t ocpi_set_channel(void *self, uint32_t channel)
{
    (void)self;
    HH_LOGW(COMP, "unsupported", "op=set_channel channel=%u reason=\"channel plan undefined, unknown.md U-14\"",
            (unsigned)channel);
    return HH_ERR_UNSUPPORTED;
}

hh_status_t ocpi_poll(void *self, hh_time_ms_t now)
{
    hh_ocpi_backend_t *b = self_of(self);
    bool timed_out = true;
    (void)now;

    if (!b->app || b->finished) return HH_OK;
    /* wait() returns true when it times out (still running) and false when the
     * application has finished. 1 us is the shortest non-zero timeout; zero
     * means "wait forever" and would stall radiod's control loop. */
    if (guarded(b, "wait", [&] { timed_out = app_of(b)->wait(1); }) != HH_OK) {
        b->finished = true;
        return HH_ERR_IO;
    }
    if (!timed_out) {
        b->finished = true;
        HH_LOGW(COMP, "app_finished", "app=%s operational=0", b->cfg.app_path);
    }
    return HH_OK;
}

const hh_radio_ops_t OCPI_OPS = {
    "ocpi",
    ocpi_open,
    ocpi_close,
    ocpi_transmit,
    ocpi_set_rx_callback,
    ocpi_get_status,
    ocpi_get_link_metrics,
    ocpi_set_channel,
    ocpi_poll,
};

} // namespace

extern "C" hh_status_t hh_ocpi_backend_init(hh_ocpi_backend_t *b, const hh_ocpi_config_t *cfg,
                                            hh_radio_t *out)
{
    if (!b || !cfg || !out || cfg->app_path[0] == '\0') return HH_ERR_INVAL;
    std::memset(b, 0, sizeof *b);
    b->cfg = *cfg;
    out->ops = &OCPI_OPS;
    out->self = b;
    return HH_OK;
}

extern "C" hh_status_t hh_ocpi_backend_get_property(hh_ocpi_backend_t *b, const char *instance,
                                                    const char *property, char *buf, size_t cap)
{
    std::string value;
    if (!b || !instance || !property || !buf || cap == 0) return HH_ERR_INVAL;
    if (!b->app) return HH_ERR_STATE;
    if (guarded(b, "get_property",
                [&] { app_of(b)->getProperty(instance, property, value); }) != HH_OK)
        return HH_ERR_NOTFOUND;
    if (value.size() >= cap) return HH_ERR_NOMEM;
    std::memcpy(buf, value.c_str(), value.size() + 1);
    return HH_OK;
}

extern "C" const char *hh_ocpi_backend_last_error(const hh_ocpi_backend_t *b)
{
    return b ? b->last_error : "";
}
