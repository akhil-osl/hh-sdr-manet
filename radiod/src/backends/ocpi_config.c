/*
 * Configuration helpers for the OpenCPI backend.
 *
 * Kept apart from ocpi_backend.cpp and written in C so that radiod can parse
 * and validate an OpenCPI configuration in every build, including one without
 * OpenCPI. A daemon built without it can then reject `backend = ocpi` with a
 * clear message instead of failing on an unknown key.
 */
#include "hhsdr/radiod/ocpi_backend.h"
#include <stdio.h>
#include <string.h>

/* Copy [src, src+len) into dst as a NUL-terminated string. Refuses empty and
 * oversized fields rather than truncating: a silently shortened instance or
 * property name would address a different property, or none. */
static hh_status_t copy_field(char *dst, size_t cap, const char *src, size_t len)
{
    if (len == 0 || len >= cap) return HH_ERR_INVAL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return HH_OK;
}

hh_status_t hh_ocpi_config_add_property(hh_ocpi_config_t *cfg, const char *spec)
{
    const char *dot, *eq;
    hh_ocpi_prop_t p;

    if (!cfg || !spec) return HH_ERR_INVAL;
    if (cfg->prop_count >= HH_OCPI_MAX_PROPS) return HH_ERR_NOMEM;

    /* The value may itself contain '.' or '=' (a file path, an expression), so
     * the instance ends at the first '.', and the property at the first '='
     * after it. */
    dot = strchr(spec, '.');
    if (!dot) return HH_ERR_INVAL;
    eq = strchr(dot + 1, '=');
    if (!eq) return HH_ERR_INVAL;

    memset(&p, 0, sizeof p);
    if (copy_field(p.instance, sizeof p.instance, spec, (size_t)(dot - spec)) != HH_OK ||
        copy_field(p.property, sizeof p.property, dot + 1, (size_t)(eq - dot - 1)) != HH_OK)
        return HH_ERR_INVAL;
    /* An empty value is legitimate for some properties (an empty string), so
     * only length is checked here. */
    if (strlen(eq + 1) >= sizeof p.value) return HH_ERR_INVAL;
    snprintf(p.value, sizeof p.value, "%s", eq + 1);

    cfg->props[cfg->prop_count++] = p;
    return HH_OK;
}
