#!/usr/bin/env python3
"""
build_full_bd.py - HH-SDR PS/PL three-plane bridge, REFACTORED WITH REDHAWK SCA.

Reproduces the full architecture baseline (every block, zone, edge label, note
and the legend) and inserts the REDHAWK SCA 2.2.2 control layer.

Deliberately absent: company name, logo, ISO strap, doc number, revision table,
signature block. Colour carries plane identity only.

REDHAWK placement: ABOVE radiod. The CF reaches hardware only through
librc / ICD-2, so Note 1 (single PL owner, never a second ACI instance) holds.

Usage: python3 build_full_bd.py <out.drawio>
"""
import argparse, os, sys, html

SKILL = ("/home/ospl/.claude/skills/synced/"
         "f58a9748-1fab-44c0-84d9-2d64487b5b8e_79937046-a967-436a-80ee-ebea1dd77e5d/"
         "pro-draw-io-bd")
sys.path.insert(0, os.path.join(SKILL, "scripts"))
from osl_route import Rect, route

INK = "#1A1A1A"; WHITE = "#FFFFFF"; GREY = "#4D4E4C"

# plane / domain: (stroke, fill)
P = {
    "app":     ("#5A6475", "#EEF0F3"),   # applications - neutral
    "control": ("#2A4674", "#DCE6F5"),   # control plane - blue
    "sca":     ("#6E4D8F", "#EADFF5"),   # SCA / REDHAWK - purple
    "data":    ("#B07D2B", "#FBEBD2"),   # data plane - amber
    "time":    ("#1F7A8C", "#D6F0F5"),   # time plane - cyan
    "pl":      ("#2C7A55", "#D8F0E2"),   # PL fabric - green
    "rf":      ("#B83732", "#FBDEDE"),   # RF / HW - red
    "test":    ("#C2701C", "#FCE6CD"),   # test automation - orange
}

PW, PH = 2100, 1620

# ---------------------------------------------------------------- zones
# (id, x, y, w, h, stroke, fill, label, label_at)
ZONES = [
    ("zps",  40,  150, 2020, 800, "#2A4674", "none",
     "PROCESSING SYSTEM — PetaLinux, dual Cortex-A9   (single PL owner: radiod)", "tl"),
    ("zsca", 790, 330, 900, 420, "#6E4D8F", "#F7F3FB",
     "SCA 2.2.2 / REDHAWK CORE FRAMEWORK   (new control layer)", "tl"),
    ("zpl",  40,  990, 1730, 450, "#2C7A55", "none",
     "PL (FPGA FABRIC)", "tl"),
    ("zrf", 1800, 990, 260, 450, "#B83732", "none",
     "RF / HW", "tl"),
]

# ---------------------------------------------------------------- blocks
# (id, x, y, w, h, kind, title, [detail lines])
CARDS = [
    # ---- PS row 1: applications, routing, test -------------------------
    ("apps",   70, 200, 250, 110, "app", "APPLICATIONS",
     ["Voice (MELPe on PS)", "Data / C2 / HMI", "IP sockets on manet0"]),
    ("manet", 370, 200, 280, 110, "control", "MANET ROUTING",
     ["OLSRv2 daemon (RFC 7181)", "Neighbour stats from librc", "IPv4 / IPv6"]),
    ("test", 1760, 200, 280, 110, "test", "TEST AUTOMATION",
     ["ATP / BIT scripts", "iperf3, MGEN, CORE/EMANE", "radioctl CLI"]),

    # ---- SCA layer ------------------------------------------------------
    ("cf",    830, 380, 380, 100, "sca", "REDHAWK CORE FRAMEWORK",
     ["DomainManager · DeviceManager", "ApplicationFactory · omniORB"]),
    ("prof", 1270, 380, 380, 100, "sca", "DOMAIN PROFILE  ($SDRROOT)",
     ["SPD · SCD · PRF · SAD · DCD", "PRF = 23 props from resource.c"]),
    ("res",   830, 530, 380, 100, "sca", "MANET_Node  —  CF::Resource",
     ["C++ servant : Resource_impl", "LifeCycle · PropertySet · PortSupplier"]),
    ("dev",  1270, 530, 380, 100, "sca", "Radio  —  CF::Device",
     ["DeviceManager-owned", "blocked on PL contract"]),
    ("orb",   830, 670, 820,  60, "sca", "ORB THREAD → CONTROL-LOOP BOUNDARY",
     ["omniORB dispatch threads → bounded queue, drained by hh_node_tick()"]),

    # ---- PS row 2: IP stack, librc, clock -------------------------------
    ("ip",    370, 420, 280, 100, "control", "LINUX IP STACK",
     ["skb priority / DSCP →", "bearer class"]),
    ("librc",  70, 600, 280, 100, "control", "librc / radioctl",
     ["C API + CLI", "versioned TLV over UNIX socket"]),
    ("clk",  1760, 600, 280, 110, "time", "RADIO CLOCK DRIVER",
     ["/dev/ptpN (PHC) + time regs", "frame / slot / hop / ns",
      "seqlock read-only to users"]),

    # ---- PS row 3: manet0, radiod ---------------------------------------
    ("net0",  370, 780, 280, 120, "data", "manet0 net_device DRIVER",
     ["pdu_dma rings (E5)", "descriptor length + TLAST",
      "zero-copy, per-class queues"]),
    ("radiod", 830, 790, 380, 110, "control", "radiod  —  PL OWNER",
     ["Holds OpenCPI ACI Application", "Only process that opens OpenCPI",
      "Events, fault registry"]),

    # ---- PS row 4: RCC workers ------------------------------------------
    ("rcc",   830, 1030, 820, 70, "control", "RCC WORKERS (PS)",
     ["waveform_ctrl | drc | mac_ps | ad9361_config_proxy | telemetry"]),

    # ---- PL fabric row 1 -------------------------------------------------
    ("audio",   70, 1150, 250, 110, "pl", "audio_pl",
     ["I2S slave (codec master)", "AEC (NLMS) + NS (Wiener)", "AXI-Stream → PS"]),
    ("dma",    370, 1150, 280, 110, "pl", "pdu_dma_tx / pdu_dma_rx",
     ["AXI-HP DMA, descriptor len", "TLAST = PDU boundary"]),
    ("macpl",  700, 1150, 300, 110, "pl", "mac_pl",
     ["TDMA slot timer, slot gating", "PDU CRC, hop-synchronous frame",
      "Generates MANET STROBE"]),
    ("modem", 1050, 1150, 300, 110, "pl", "MODEM CHAIN",
     ["framer / FEC / modulator (TX)", "demod / FEC / deframer (RX)",
      "runtime properties"]),
    ("ad9361w",1400, 1150, 300, 110, "pl", "ad9361 DEVICE WORKERS",
     ["adc / dac / data_sub", "LVDS 2R2T DDR"]),
    ("rfic",  1830, 1150, 210, 110, "rf", "AD9361 RFIC",
     ["LVDS DDR 122.88 MHz", "SPI control"]),

    # ---- PL fabric row 2 -------------------------------------------------
    ("gnss",    70, 1310, 250, 110, "time", "GNSS 1PPS (Card 2)",
     ["L1 + NavIC L5 (E7 item)", "SYS_1PPS on backplane"]),
    ("timebase",370, 1310, 280, 110, "time", "PL TIME BASE",
     ["1PPS disciplined counters", "frame / slot / hop / ns"]),
    ("fh",     700, 1310, 300, 110, "pl", "fh_controller",
     ["1000 hop/s LO retune", "HOP_TRIG / ACK, dwell"]),
    ("rfctrl", 1050, 1310, 400, 110, "pl", "rf_ctrl_fsm",
     ["RF_OFF/RX_PREP/RX/TX_PREP/TX", "TURNAROUND/FAULT/SAFE"]),
    ("fe",    1830, 1310, 210, 110, "rf", "RF FRONT-END",
     ["PA / LNA / T-R switch", "Interlock (HW)"]),
]

# ---------------------------------------------------------------- edges
# (src, tgt, kind, label)
# kind: data | control | time | strobe | rf | corba
EDGES = [
    # data plane
    ("apps",  "manet",    "data",    "IP"),
    ("manet", "ip",       "data",    "sockets"),
    ("ip",    "net0",     "data",    "skb"),
    ("net0",  "dma",      "data",    "ICD-1 PDU-over-DMA (AXI-HP)"),
    ("audio", "apps",     "data",    "clean PCM 8 kHz"),

    # control plane - existing
    ("manet", "librc",    "control", "rc_* API"),
    ("librc", "radiod",   "control", "ICD-2 TLV / UNIX socket"),
    ("test",  "librc",    "control", "radioctl"),
    ("radiod","rcc",      "control", "OpenCPI ACI"),
    ("rcc",   "macpl",    "control", "properties"),
    ("rcc",   "modem",    "control", "properties"),
    ("rcc",   "ad9361w",  "control", "device / DRC props"),
    ("rcc",   "fh",       "control", "hopset id"),
    ("ad9361w","rfic",    "control", "SPI (proxy)"),

    # SCA / CORBA - new
    ("cf",    "prof",     "corba",   ""),
    ("cf",    "res",      "corba",   "deploy"),
    ("prof",  "dev",      "corba",   "DCD"),
    ("res",   "orb",      "corba",   "CORBA"),
    ("dev",   "orb",      "corba",   "CORBA"),
    ("orb",   "radiod",   "corba",   "librc / ICD-2"),
    ("manet", "cf",       "corba",   "neighbour stats"),

    # time plane
    ("gnss",    "timebase", "time", "1PPS"),
    ("timebase","fh",       "time", "hop tick"),
    ("timebase","macpl",    "time", "slot / frame tick"),
    ("rcc",     "clk",      "time", "ICD-3 time regs (AXI-Lite)"),
    ("clk",     "test",     "time", "clock_gettime(PHC)"),

    # MANET STROBE / RF state authority
    ("macpl", "rfctrl",  "strobe", "MANET STROBE"),
    ("fh",    "rfctrl",  "strobe", "hop tick"),
    ("rfctrl","fe",      "strobe", "PA/LNA/TR"),

    # PL signal chain + RF
    ("dma",    "macpl",   "pl", "PDU"),
    ("macpl",  "modem",   "pl", "frame"),
    ("modem",  "ad9361w", "pl", "IQ"),
    ("ad9361w","rfic",    "pl", "LVDS"),
    ("rfic",   "fe",      "rf", "RF"),
]

EDGE_KIND = {
    "data":    ("#2A5CB8", 2.5, "", "classic"),
    "control": ("#C2701C", 2.0, "dashed=1;dashPattern=6 4;", "classic"),
    "corba":   ("#6E4D8F", 2.5, "dashed=1;dashPattern=8 4;", "classic"),
    "time":    ("#1F7A8C", 1.8, "dashed=1;dashPattern=2 4;", "classic"),
    "strobe":  ("#A03060", 2.0, "dashed=1;dashPattern=8 4;", "classic"),
    "pl":      ("#2C7A55", 2.2, "", "classic"),
    "rf":      ("#B83732", 2.5, "", "classic"),
}

LEGEND = [
    ("data",    "DATA plane — PDU / IQ / audio stream"),
    ("control", "CONTROL plane — properties / API / SPI"),
    ("corba",   "SCA / CORBA — REDHAWK control (new)"),
    ("time",    "TIME plane — 1PPS / slot / hop timing"),
    ("strobe",  "MANET STROBE / RF state authority"),
    ("rf",      "RF / analogue path"),
]

NOTES = [
    "1. Single-owner rule: only radiod opens OpenCPI. All other processes use librc (ICD-2). Never a second ACI instance.",
    "2. REDHAWK CF sits ABOVE radiod: the Domain/Device Managers reach hardware only through librc, so Note 1 is preserved.",
    "3. ORB threads never call the MANET stack directly. They enqueue; hh_node_tick() drains. Keeps the single-threaded, no-locks invariant.",
    "4. Data contract ICD-1: one PDU = one descriptor = one skb; TLAST marks PDU boundary; length carried in descriptor, not in payload.",
    "5. MANET STROBE from mac_pl is the sole RF state authority; rf_ctrl_fsm may only add safety (interlock), never contradict it.",
]


def esc(s):
    return html.escape(str(s), quote=True)


class Cells:
    def __init__(self):
        self.rows = []
        self.rects = {}
        self.ids = set()

    def _claim(self, cid):
        if cid in self.ids:
            raise ValueError(f"duplicate mxCell id: {cid!r}")
        self.ids.add(cid)

    def vertex(self, cid, value, style, x, y, w, h, collide=True):
        self._claim(cid)
        self.rows.append(
            f'        <mxCell id="{esc(cid)}" value="{esc(value)}" style="{style}" '
            f'vertex="1" parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" '
            f'as="geometry"/></mxCell>')
        if collide:
            self.rects[cid] = Rect(x, y, w, h)

    def edge(self, eid, src, tgt, style, waypoints, label=""):
        self._claim(eid)
        if waypoints:
            arr = "".join(f'<mxPoint x="{int(round(px))}" y="{int(round(py))}"/>'
                          for px, py in waypoints)
            geo = (f'<mxGeometry relative="1" as="geometry"><Array as="points">'
                   f'{arr}</Array></mxGeometry>')
        else:
            geo = '<mxGeometry relative="1" as="geometry"/>'
        self.rows.append(
            f'        <mxCell id="{esc(eid)}" value="{esc(label)}" style="{style}" edge="1" '
            f'parent="1" source="{esc(src)}" target="{esc(tgt)}">{geo}</mxCell>')


def txt(fs, col=INK, bold=False, ital=False, align="left"):
    return (f"text;html=1;strokeColor=none;fillColor=none;fontColor={col};fontFamily=Arial;"
            f"fontSize={fs};fontStyle={(1 if bold else 0)|(2 if ital else 0)};"
            f"align={align};verticalAlign=middle;")


def build():
    C = Cells()

    # title (no company, no doc number)
    C.vertex("title", "HH-SDR PS/PL SOFTWARE ARCHITECTURE — THREE-PLANE BRIDGE "
                      "with REDHAWK SCA 2.2.2",
             txt(18, INK, bold=True, align="center"), 40, 40, 2020, 30, collide=False)
    C.vertex("subtitle",
             "Data plane (PDU-over-DMA net_device)  |  Control plane (radiod / OpenCPI ACI)  |  "
             "Time plane (1PPS / PHC)  |  SCA control (REDHAWK CF / CORBA)",
             txt(11, GREY, align="center"), 40, 75, 2020, 20, collide=False)

    # zones
    for zid, x, y, w, h, stroke, fill, lab, _at in ZONES:
        C.vertex(zid, "", f"rounded=0;html=1;fillColor={fill};strokeColor={stroke};"
                 f"strokeWidth=1.5;dashed=1;", x, y, w, h, collide=False)
        C.vertex(zid + "_l", lab, txt(11, stroke, bold=True),
                 x + 12, y - 16, min(900, 7 * len(lab)), 18)

    # blocks
    for cid, x, y, w, h, kind, title, details in CARDS:
        stroke, fill = P[kind]
        lines = "<br>".join(f'<font style="font-size:10px;color:{GREY}">{d}</font>'
                            for d in details)
        val = f"<b>{title}</b>" + (f"<br>{lines}" if lines else "")
        C.vertex(cid, val,
                 f"rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor={stroke};"
                 f"fontColor={INK};fontFamily=Arial;fontSize=11;align=center;"
                 f"verticalAlign=middle;strokeWidth=1.5;", x, y, w, h)

    # routing
    all_ids = list(C.rects.keys())
    used = {}
    # Force a side pair where the natural entry would cross the target's
    # centred label or collide with a neighbouring lane.
    FORCED = {
        ("orb", "radiod"): [("S", "N")],
        ("res", "orb"):    [("S", "N")],
        ("dev", "orb"):    [("S", "N")],
        ("radiod", "rcc"): [("S", "N")],
        ("ip", "net0"):    [("S", "N")],
        ("net0", "dma"):   [("S", "N")],
        ("audio", "apps"): [("W", "W")],     # was cutting "Data / C2 / HMI"
        ("librc", "radiod"): [("E", "W")],
        ("rcc", "macpl"):  [("S", "N")],     # was cutting "TDMA slot timer"
        ("rcc", "modem"):  [("S", "N")],
        ("rcc", "ad9361w"):[("S", "N")],
        ("rcc", "fh"):     [("S", "N")],
        ("clk", "test"):   [("N", "S")],     # was cutting "frame / slot / hop"
        ("macpl", "rfctrl"): [("S", "N")],   # was cutting "rf_ctrl_fsm"
        ("fh", "rfctrl"):  [("E", "W")],
        ("rfctrl", "fe"):  [("E", "W")],
    }

    report = []
    for i, (s, t, kind, label) in enumerate(EDGES, start=1):
        colour, wid, dash, arrow = EDGE_KIND[kind]
        style = (f"edgeStyle=orthogonalEdgeStyle;rounded=0;html=1;jettySize=auto;{dash}"
                 f"strokeColor={colour};strokeWidth={wid};endArrow={arrow};endFill=1;"
                 f"fontFamily=Arial;fontSize=9;fontColor={INK};"
                 f"labelBackgroundColor=#FFFFFF;")
        skip = {s, t}
        obstacles = [C.rects[k] for k in all_ids if k not in skip]
        r = route(C.rects[s], C.rects[t], obstacles, used, PW, PH,
                  grid=10, margin=8, stub=10, turn_penalty=4, lane_penalty=14,
                  sides=FORCED.get((s, t)))
        C.edge(f"edge{i}", s, t, style, r.waypoints, label)
        report.append((f"edge{i}", s, t, kind, r.bends, r.fallback))

    # notes panel
    C.vertex("notes_box", "", f"rounded=0;html=1;fillColor=none;strokeColor=#BFBFBF;"
             f"strokeWidth=1;", 40, 1470, 1180, 130, collide=False)
    C.vertex("notes_t", "NOTES", txt(10, INK, bold=True), 52, 1476, 200, 16, collide=False)
    for i, n in enumerate(NOTES):
        C.vertex(f"note{i}", n, txt(9, INK), 52, 1496 + i * 20, 1150, 18, collide=False)

    # legend panel
    C.vertex("leg_box", "", f"rounded=0;html=1;fillColor=none;strokeColor=#BFBFBF;"
             f"strokeWidth=1;", 1250, 1470, 810, 130, collide=False)
    C.vertex("leg_t", "LEGEND", txt(10, INK, bold=True), 1262, 1476, 200, 16, collide=False)
    for i, (kind, lab) in enumerate(LEGEND):
        colour, wid, dash, _ = EDGE_KIND[kind]
        ly = 1496 + i * 18
        C.vertex(f"lgl{i}", "",
                 f"rounded=0;html=1;fillColor={colour};strokeColor=none;",
                 1262, ly + 7, 46, 3, collide=False)
        C.vertex(f"lgt{i}", lab, txt(9, INK), 1320, ly, 700, 18, collide=False)

    body = "\n".join(C.rows)
    return ('<mxfile host="app.diagrams.net" version="24.0.0">\n'
            '  <diagram name="HH-SDR three-plane + REDHAWK SCA" id="full01">\n'
            f'    <mxGraphModel dx="2100" dy="1500" grid="1" gridSize="10" guides="1" '
            f'tooltips="1" connect="1" arrows="1" fold="1" page="1" pageScale="1" '
            f'pageWidth="{PW}" pageHeight="{PH}" math="0" shadow="0">\n'
            '      <root>\n'
            '        <mxCell id="0"/>\n'
            '        <mxCell id="1" parent="0"/>\n'
            f'{body}\n'
            '      </root>\n'
            '    </mxGraphModel>\n'
            '  </diagram>\n'
            '</mxfile>\n'), report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    args = ap.parse_args()
    xml, report = build()
    with open(args.out, "w") as f:
        f.write(xml)
    print("wrote", args.out)
    fb = [r for r in report if r[5]]
    print(f"blocks: {len(CARDS)}  edges: {len(report)}  fallback(non-clean): {len(fb)}")
    for eid, s, t, kind, bends, fbk in report:
        if fbk:
            print(f"  {eid}: {s} -> {t} [{kind}] FALLBACK")


if __name__ == "__main__":
    main()
