# HH-SDR + REDHAWK SCA 2.2.2 — integration architecture

Companion to `hh-sdr-redhawk-sca-bd.drawio` (OSL-628-BD-102, concept). Same
content, Mermaid form, for viewing inline on GitHub or in an editor preview.

Derived from the architecture baseline (OSL-628-BD-101 Rev 00) plus what is
actually on `main`. Blocks are marked **BUILT**, **TO BUILD** or **BLOCKED**;
BLOCKED items name the `unknown.md` U-number they wait on. Nothing unspecified
is invented.

## Placement decision

REDHAWK sits **above** radiod. The Core Framework reaches hardware only through
`librc` / ICD-2, so Note 1 of the baseline drawing — *"only radiod opens
OpenCPI. Never a second ACI instance"* — is preserved. The CF becomes another
control-plane client alongside `radioctl`, not a second owner of the PL.

```mermaid
flowchart TB
    subgraph PS["PROCESSING SYSTEM — PetaLinux, dual Cortex-A9 (single PL owner: radiod)"]
        direction TB

        subgraph L1["Applications &amp; routing"]
            APP["APPLICATIONS<br/><i>Voice (MELPe) · Data / C2 / HMI</i><br/><b>BUILT</b>"]
            MANET["MANET ROUTING<br/><i>hh_node_t · DV self-healing</i><br/><i>OLSRv2 deferred — U-09</i><br/><b>BUILT</b>"]
            TEST["TEST AUTOMATION<br/><i>ATP / BIT · radioctl CLI</i><br/><b>BUILT</b>"]
        end

        subgraph SCA["SCA 2.2.2 / REDHAWK CF — NEW LAYER"]
            CF["REDHAWK CORE FRAMEWORK<br/><i>DomainManager · DeviceManager</i><br/><i>ApplicationFactory · omniORB</i><br/><b>TO BUILD</b>"]
            PROFILE["DOMAIN PROFILE ($SDRROOT)<br/><i>SPD · SCD · PRF · SAD · DCD</i><br/><i>PRF = 23 props from resource.c</i><br/><b>TO BUILD</b>"]
            RES["MANET_Node — CF::Resource<br/><i>C++ servant : Resource_impl</i><br/><i>LifeCycle · PropertySet · Port</i><br/><b>TO BUILD</b>"]
            DEV["Radio — CF::Device<br/><i>DeviceManager-owned</i><br/><b>BLOCKED — U-03 / U-04</b>"]
            ORB["ORB THREAD → CONTROL-LOOP BOUNDARY<br/><i>omniORB dispatch threads → bounded request queue</i><br/><i>drained by hh_node_tick() — keeps the no-locks invariant</i><br/><b>TO BUILD — U-11 / U-12</b>"]
        end

        subgraph L3["Existing C control plane"]
            IP["LINUX IP STACK<br/><i>skb priority / DSCP → bearer class</i><br/><b>BUILT</b>"]
            LIBRC["librc / radioctl<br/><i>C API + CLI</i><br/><i>ASCII key=value today — U-01</i><br/><b>BUILT</b>"]
            RADIOD["radiod — PL OWNER<br/><i>Only process that opens OpenCPI</i><br/><i>Lifecycle · events · fault registry</i><br/><b>BUILT</b>"]
            MANET0["manet0 net_device<br/><i>pdu_dma rings · TLAST</i><br/><b>BUILT</b>"]
            CLOCK["RADIO CLOCK DRIVER<br/><i>/dev/ptpN (PHC) + time regs</i><br/><i>Time plane absent — U-07</i><br/><b>TO BUILD</b>"]
        end
    end

    subgraph PL["PL (FPGA FABRIC) + RF/HW — contracts undefined"]
        SEAM["hh_radio_ops_t (8 fns)<br/><i>The ONE hardware seam</i><br/><i>hw_adapter → NOT_IMPLEMENTED</i><br/><b>BUILT</b>"]
        RCC["RCC WORKERS (PS)<br/><i>waveform_ctrl · drc · mac_ps</i><br/><i>ad9361_config_proxy · telemetry</i><br/><b>BLOCKED — U-03</b>"]
        FABRIC["PL FABRIC (FPGA)<br/><i>mac_pl · modem · fh_controller</i><br/><i>pdu_dma · rf_ctrl_fsm · time base</i><br/><b>BLOCKED</b>"]
        RFIC["AD9361 RFIC + RF FRONT-END<br/><i>LVDS DDR 122.88 MHz · SPI</i><br/><b>BLOCKED</b>"]
        TIMEBASE["GNSS 1PPS / PL TIME BASE<br/><i>frame / slot / hop / ns</i><br/><b>BLOCKED — U-07</b>"]
    end

    %% control plane
    APP    -->|IP| MANET
    MANET  -->|rc_* API| LIBRC
    TEST   --> LIBRC
    LIBRC  -->|ICD-2 TLV / UNIX socket| RADIOD
    RADIOD -->|OpenCPI ACI| RCC

    %% SCA control (CORBA)
    CF      -.->|reads| PROFILE
    CF      -.->|deploys| RES
    PROFILE -.->|DCD| DEV
    RES     -.->|CORBA| ORB
    DEV     -.->|CORBA| ORB
    ORB     -->|librc / ICD-2| RADIOD

    %% data plane
    APP    ==>|skb| IP
    IP     ==> MANET0
    MANET0 ==>|ICD-1 PDU-over-DMA| FABRIC

    %% PL / RF
    SEAM --> RCC
    SEAM --> RFIC
    RCC  --> FABRIC

    %% time plane
    CLOCK -.-> TIMEBASE
    TIMEBASE -.->|slot / frame tick| FABRIC

    classDef built   fill:#DEEDE6,stroke:#2C5A45,stroke-width:1.5px,color:#1A1A1A
    classDef tobuild fill:#F3E9D6,stroke:#855E1F,stroke-width:1.5px,stroke-dasharray:6 3,color:#1A1A1A
    classDef blocked fill:#F7E3E2,stroke:#912B27,stroke-width:2px,stroke-dasharray:6 3,color:#1A1A1A
    classDef scanew  fill:#ECE5F3,stroke:#503669,stroke-width:1.5px,stroke-dasharray:6 3,color:#1A1A1A

    class APP,MANET,TEST,IP,LIBRC,RADIOD,MANET0,SEAM built
    class CLOCK tobuild
    class CF,PROFILE,RES,ORB scanew
    class DEV,RCC,FABRIC,RFIC,TIMEBASE blocked
```

## Line meanings

| Style | Plane |
|---|---|
| solid arrow | control — `rc_*` / ICD-2 / properties |
| dotted arrow | CORBA / GIOP — SCA control (**new**) |
| thick arrow | data — PDU / skb / DMA |
| dotted (time) | time — 1PPS / PHC |

## What the diagram asserts, and why

**The ORB boundary is drawn as its own block deliberately.** REDHAWK's ORB is
threaded: CORBA calls arrive on omniORB dispatch threads. This codebase has zero
`pthread` and a documented single-threaded, no-locks invariant, and
`hh_node_tick()` runs a 10 ms control loop. Those two facts collide. The block
names the resolution: ORB threads enqueue, the control loop drains, so every
call into `hh_node_*` still happens on one thread and the invariant holds. The
one lock lives in the servant, at the boundary, not inside the MANET stack.

**`Radio — CF::Device` is BLOCKED, not TO BUILD.** A Device wrapping real
hardware needs the PL contract (U-03/U-04). A Device wrapping the *mock* is
legitimate and would exercise the DCD/DeviceManager path without inventing a
register map.

**The PRF is not new work.** All 23 properties already exist in
`src/sca/resource.c` with stable ids, `configure`/`execparam`/`allocation`
kinds, and bindings to real `hh_config_t` keys.

## Open decisions this diagram does not settle

1. **Domain topology.** SCA assumes one domain per platform; this is a *mesh* of
   N self-healing nodes. One domain per node, or one spanning the mesh? The
   latter puts CORBA on the RF link and couples node survival to a remote
   DomainManager, which contradicts the self-healing premise. The sheet draws
   one node.
2. **Footprint.** DomainManager + DeviceManager + omniORB per node on a Zynq PS
   is unmeasured, and is the feasibility question for the whole approach.

## Regenerating

```bash
python3 docs/diagrams/build_redhawk_bd.py docs/diagrams/hh-sdr-redhawk-sca-bd.drawio
```

Edit the `CARDS`, `BANNERS` and `EDGES` tables at the top of that script; every
connector is placed by the obstacle-avoiding router, so nothing needs hand
routing. The authoritative render is draw.io itself (**File ▸ Export As**); the
checked-in PNG is a QC preview.
