# workers/ — OpenCPI RCC workers

**Status: EMPTY. Nothing here is implemented.**

This directory marks where the target architecture's PS-side OpenCPI RCC
workers belong. No worker, spec, or build file exists here, because their
names and property surfaces are unspecified — see U-03.

## What belongs here

The architecture shows five RCC workers running on the PS, held inside the
OpenCPI application that **radiod alone** opens:

```
radiod — PL OWNER
    |  OpenCPI ACI
    v
RCC WORKERS (PS)
  waveform_ctrl | drc | mac_ps | ad9361_config_proxy | telemetry
```

Those are box labels from the drawing, **not verified instance names**. Also
unknown: each worker's property names, types, units, ranges and defaults;
which properties are readable, writable or volatile; the application XML name
and path; and the PL container/assembly identity.

## The single-owner rule

The drawing's Note 1 is explicit:

> Single-owner rule: only radiod opens OpenCPI. All other processes use librc
> (ICD-2). Never a second ACI instance.

Workers are part of radiod's application. They must not become independently
managed PL owners, and nothing outside radiod may open an ACI instance.

## Current state of the repository

No OpenCPI integration exists. A sweep for `OpenCPI`, `ocpi`, `OCPI`, `ACI`
and `RCC` returns **zero** implementation hits — the only matches are three
prose lines stating the absence deliberately.

Outside this repository:

- `/home/ospl/opencpi` — an OpenCPI framework checkout (built, `platforms/zynq`,
  registry with `ocpi.core` / `ocpi.assets`). Not on `PATH`; `OCPI_CDK_DIR` is
  unset; not referenced by this project.
- `/home/ospl/projects/manet-opencpi/manet/` — an **empty** OpenCPI project
  skeleton. Its `project-metadata.xml` declares `local.manet.manet` with
  `<workers/>`, `<tests/>` and `<specs/>` all empty. No HDL, specs or
  assemblies.

Whether that skeleton is merged here, kept as a sibling project, or becomes a
submodule is itself undecided (U-10 in the audit; tracked as part of U-03).

One further constraint: the OpenCPI ACI is a **C++** API and this project is
strict C11 (`project(hh_sdr_manet C)`). How that boundary is crossed is U-11.

See [`../unknown.md`](../unknown.md).
