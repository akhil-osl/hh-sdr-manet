#!/usr/bin/env python3
"""
build_redhawk_bd.py - HH-SDR PS/PL Three-Plane Bridge WITH REDHAWK SCA 2.2.2 integrated.

Derived from the pro-draw-io-bd reference builder. Content is taken from:
  * the supplied architecture baseline drawing (OSL-628-BD-101 Rev 00), and
  * what actually exists on `main` of hh-sdr-manet (verified by reading the code).

Nothing unspecified is invented: blocks that do not exist yet are drawn in the
"TO BUILD" / "BLOCKED" treatment and carry their U-number where one applies.

REDHAWK placement decision (user-confirmed): REDHAWK sits ABOVE radiod. The CF
reaches hardware only through librc/ICD-2, so Note 1 (single PL owner, never a
second ACI instance) is preserved.
"""
import argparse, base64, os, sys, html

SKILL = ("/home/ospl/.claude/skills/synced/"
         "f58a9748-1fab-44c0-84d9-2d64487b5b8e_79937046-a967-436a-80ee-ebea1dd77e5d/"
         "pro-draw-io-bd")
sys.path.insert(0, os.path.join(SKILL, "scripts"))
from osl_route import Rect, route

# ----------------------------------------------------------------- brand / domains
INK = "#1A1A1A"; WHITE = "#FFFFFF"; HAIR = "#BFBFBF"
BLUE = "#2A4674"; BLUE_D = "#1B2E4A"; RED = "#B83732"; GREY_D = "#4D4E4C"
AMBER = "#B07D2B"; AMBER_D = "#855E1F"; AMBER_PALE = "#F3E9D6"

DOMAIN = {   # name: (stroke_dark, fill, chip_pale, chip_text)
    "app":      ("#44505E", "#5E6B7A", "#E7EBF0", "#44505E"),
    "sca":      ("#503669", "#6E4D8F", "#ECE5F3", "#503669"),
    "control":  ("#1B2E4A", "#2A4674", "#E7ECF3", "#2A4674"),
    "data":     ("#855E1F", "#B07D2B", "#F3E9D6", "#855E1F"),
    "time":     ("#1F4E5B", "#2C6E7F", "#DDEBEE", "#1F4E5B"),
    "pl":       ("#2C5A45", "#3E7A5E", "#DEEDE6", "#2C5A45"),
    "rf":       ("#912B27", "#B83732", "#F7E3E2", "#912B27"),
}
PW, PH = 1654, 1169

BD_NAME   = "HH-SDR PS/PL Architecture with REDHAWK SCA 2.2.2 Integration"
BD_NUMBER = "OSL-628-BD-102"          # placeholder: derived from baseline BD-101
PROGRAMME = "IWO:628 HH-SDR  ·  THREE-PLANE BRIDGE + SCA CONTROL"
SUBSYS    = ("Control plane (REDHAWK CF / CORBA → librc → radiod)  ·  "
             "Data plane (PDU-over-DMA)  ·  Time plane (1PPS / PHC)")

# status: "built" | "tobuild" | "blocked"
# card: (id, x, y, w, h, domain, status, title, [chip, ...])
CARDS = [
    # ---- Row A: applications + SCA domain (y=200) --------------------------
    ("a1",   60, 200, 250, 140, "app",  "built",   "APPLICATIONS",
     ["Voice (MELPe) · Data / C2 / HMI", "IP sockets on manet0"]),
    ("a2",  350, 200, 250, 140, "app",  "built",   "MANET ROUTING",
     ["hh_node_t · DV self-healing", "OLSRv2 deferred — U-09"]),
    ("a3",  630, 200, 300, 140, "sca",  "tobuild", "REDHAWK CORE FRAMEWORK",
     ["DomainManager · DeviceManager", "ApplicationFactory · omniORB"]),
    ("a4",  960, 200, 300, 140, "sca",  "tobuild", "DOMAIN PROFILE  ($SDRROOT)",
     ["SPD · SCD · PRF · SAD · DCD", "PRF = 23 props from resource.c"]),
    ("a5", 1300, 200, 290, 140, "app",  "built",   "TEST AUTOMATION",
     ["ATP / BIT · iperf3 · MGEN", "radioctl CLI"]),

    # ---- Row B: the SCA servant layer (y=360) ------------------------------
    ("b1",  630, 380, 300, 140, "sca",  "tobuild", "MANET_Node  —  CF::Resource",
     ["C++ servant : Resource_impl", "LifeCycle · PropertySet · Port"]),
    ("b2",  960, 380, 300, 140, "sca",  "blocked", "Radio  —  CF::Device",
     ["DeviceManager-owned", "BLOCKED — U-03 / U-04"]),

    # ---- Row C: the ORB/thread boundary (y=520) ----------------------------
    ("c1",  630, 560, 640, 100, "sca",  "tobuild", "ORB THREAD → CONTROL-LOOP BOUNDARY",
     ["omniORB dispatch threads → bounded request queue",
      "drained by hh_node_tick() — keeps the no-locks invariant · U-11 / U-12"]),

    # ---- Row D: the existing C control plane (y=660) -----------------------
    ("d1",   60, 700, 250, 140, "control", "built", "LINUX IP STACK",
     ["skb priority / DSCP", "→ bearer class"]),
    ("d2",  350, 700, 250, 140, "control", "built", "librc / radioctl",
     ["C API + CLI", "ASCII key=value today · U-01"]),
    ("d3",  630, 700, 300, 140, "control", "built", "radiod  —  PL OWNER",
     ["Only process that opens OpenCPI", "Lifecycle · events · fault registry"]),
    ("d4", 1300, 700, 290, 140, "time", "tobuild", "RADIO CLOCK DRIVER",
     ["/dev/ptpN (PHC) + time regs", "Time plane absent — U-07"]),
    ("d5",  960, 700, 300, 140, "data", "built", "manet0 net_device",
     ["pdu_dma rings · TLAST", "zero-copy per-class queues"]),

    # ---- Row E: OpenCPI + PL (y=850) ---------------------------------------
    ("e1",  630, 880, 300, 130, "pl", "blocked", "RCC WORKERS (PS)",
     ["waveform_ctrl · drc · mac_ps", "ad9361_config_proxy · telemetry"]),
    ("e2",  960, 880, 300, 130, "pl", "blocked", "PL FABRIC (FPGA)",
     ["mac_pl · modem · fh_controller", "pdu_dma · rf_ctrl_fsm · time base"]),
    ("e3",   60, 880, 250, 130, "rf", "blocked", "AD9361 RFIC + RF FRONT-END",
     ["LVDS DDR 122.88 MHz · SPI", "PA / LNA / T-R switch"]),
    ("e4", 1300, 880, 290, 130, "time", "blocked", "GNSS 1PPS / PL TIME BASE",
     ["frame / slot / hop / ns", "1PPS disciplined counters"]),
    ("e5",  350, 880, 250, 130, "pl", "built", "hh_radio_ops_t  (8 fns)",
     ["The ONE hardware seam", "hw_adapter → NOT_IMPLEMENTED"]),
]

# banners: (id, x, w, domain, text)
BANNERS = [
    ("bn1",  60, 540, "app",     "LAYER 01 — APPLICATIONS & ROUTING"),
    ("bn2", 630, 640, "sca",     "LAYER 02 — SCA / REDHAWK CONTROL  (NEW)"),
    ("bn3", 1300, 290, "app",    "LAYER 03 — TEST"),
]

# edges: (src, tgt, kind)  kind: primary | control | corba | data | time
EDGES = [
    ("a1", "a2", "primary"),      # apps -> MANET routing (IP)
    ("a3", "a4", "corba"),        # CF reads the domain profile
    ("a3", "b1", "corba"),        # DomainManager deploys the Resource
    ("a4", "b2", "corba"),        # DCD -> Device
    ("b1", "c1", "corba"),        # servant -> ORB boundary
    ("b2", "c1", "corba"),        # device -> ORB boundary
    ("c1", "d3", "control"),      # boundary -> radiod  (via librc, ICD-2)
    ("a2", "d2", "control"),      # MANET routing -> librc (rc_* API)
    ("d2", "d3", "control"),      # librc -> radiod  (ICD-2 TLV / UNIX socket)
    ("a5", "d2", "control"),      # test automation -> radioctl
    ("a1", "d1", "data"),         # apps -> IP stack
    ("d1", "d5", "data"),         # IP stack -> manet0 (skb)
    ("d3", "e1", "control"),      # radiod -> OpenCPI ACI -> RCC workers
    ("e1", "e2", "control"),      # workers -> PL properties
    ("e5", "e3", "primary"),      # radio seam -> RFIC (short, local)
    ("d5", "e2", "data"),         # manet0 DMA -> PL (ICD-1)
    ("e4", "e2", "time"),         # time base -> PL fabric (slot/frame tick)
    ("e5", "e1", "control"),      # radio seam -> workers
    ("d4", "e4", "time"),         # clock driver -> PL time base
]

CARD_TITLE_H = 40

def esc(s):
    return html.escape(str(s), quote=True)


class Cells:
    def __init__(self):
        self.rows = []
        self.rects = {}
        self.ids = set()

    def _claim(self, cid):
        # mxCell ids must be unique or draw.io rejects the file on open.
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

    def image(self, cid, x, y, w, h, uri):
        self._claim(cid)
        style = (f"shape=image;verticalLabelPosition=bottom;labelBackgroundColor=#ffffff;"
                 f"verticalAlign=top;aspect=fixed;imageAspect=0;image={uri};whiteSpace=wrap;")
        self.rows.append(
            f'        <mxCell id="{esc(cid)}" value="" style="{style}" vertex="1" '
            f'parent="1"><mxGeometry x="{x}" y="{y}" width="{w}" height="{h}" as="geometry"/></mxCell>')

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


# ----------------------------------------------------------------- styles
def txt(fs, col=INK, bold=False, ital=False, align="left", va="middle"):
    return (f"text;html=1;strokeColor=none;fillColor=none;fontColor={col};fontFamily=Arial;"
            f"fontSize={fs};fontStyle={(1 if bold else 0)|(2 if ital else 0)};"
            f"align={align};verticalAlign={va};")


def cell_box(fill, stroke, col=INK, fs=10, bold=False, ital=False, align="left",
             sl=6, va="middle", rounded=False, arc=0, shadow=False):
    return (f"rounded={1 if rounded else 0};{('arcSize=%d;' % arc) if rounded else ''}"
            f"whiteSpace=wrap;html=1;fillColor={fill};strokeColor={stroke};fontColor={col};"
            f"fontFamily=Arial;fontSize={fs};fontStyle={(1 if bold else 0)|(2 if ital else 0)};"
            f"align={align};verticalAlign={va};spacingLeft={sl};strokeWidth=1;"
            f"{'shadow=1;' if shadow else ''}")


def card_style(stroke, status):
    dash = "dashed=1;dashPattern=8 4;" if status in ("tobuild", "blocked") else ""
    sw = 2 if status == "blocked" else 1.5
    return (f"rounded=1;arcSize=8;whiteSpace=wrap;html=1;fillColor={WHITE};strokeColor={stroke};"
            f"fontColor={INK};fontFamily=Arial;fontSize=12;fontStyle=1;verticalAlign=top;"
            f"align=center;spacingTop=8;spacingLeft=22;spacingRight=8;shadow=1;"
            f"strokeWidth={sw};{dash}")


def accent_style(fill):
    return f"rounded=1;arcSize=40;html=1;fillColor={fill};strokeColor=none;"


def badge_style(fill):
    return (f"ellipse;whiteSpace=wrap;html=1;fillColor={fill};strokeColor={WHITE};fontColor={WHITE};"
            f"fontFamily=Arial;fontSize=11;fontStyle=1;verticalAlign=middle;align=center;"
            f"shadow=1;strokeWidth=2;")


def chip_style(pale, txtcol, stroke):
    return (f"rounded=1;arcSize=40;whiteSpace=wrap;html=1;fillColor={pale};strokeColor={stroke};"
            f"fontColor={txtcol};fontFamily=Arial;fontSize=9;verticalAlign=middle;align=center;"
            f"strokeWidth=1;")


def banner_style(fill, stroke):
    return (f"rounded=1;arcSize=14;whiteSpace=wrap;html=1;fillColor={fill};strokeColor={stroke};"
            f"fontColor={WHITE};fontFamily=Arial;fontSize=12;fontStyle=1;verticalAlign=middle;"
            f"align=center;shadow=1;strokeWidth=1;")


STATUS_TAG = {
    "built":   ("BUILT",    "#DEEDE6", "#2C5A45"),
    "tobuild": ("TO BUILD", AMBER_PALE, AMBER_D),
    "blocked": ("BLOCKED",  "#F7E3E2", "#912B27"),
}


def build(logo_path):
    C = Cells()
    C.vertex("rule_top", "", f"rounded=0;html=1;fillColor={BLUE};strokeColor=none;",
             0, 0, 1650, 10, collide=False)
    C.vertex("hdr_rule", "", f"rounded=0;html=1;fillColor={RED};strokeColor=none;",
             0, 100, 1650, 10, collide=False)
    C.vertex("border", "", f"rounded=0;html=1;fillColor=none;strokeColor={HAIR};strokeWidth=1.5;",
             20, 20, 1610, 1140, collide=False)

    # --- header ---------------------------------------------------------------
    uri = ""
    if logo_path and os.path.exists(logo_path):
        with open(logo_path, "rb") as f:
            uri = "data:image/png," + base64.b64encode(f.read()).decode("ascii")
    if uri:
        C.image("osl_logo", 50, 30, 150, 60, uri)
    else:
        C.vertex("osl_logo", "OSL", cell_box(BLUE, BLUE_D, WHITE, 16, True, align="center",
                 rounded=True, arc=18, shadow=True), 50, 30, 60, 50, collide=False)
    C.vertex("logo_strap", "Fast | Precise | Customized", txt(10, GREY_D),
             220, 40, 300, 20, collide=False)
    C.vertex("logo_iso", "An AS9100D, ISO 27001:2013 &amp; ISO 9001:2015 Certified Company",
             txt(9, GREY_D, ital=True), 220, 60, 330, 20, collide=False)
    C.vertex("h_prog", PROGRAMME, txt(12, BLUE, bold=True), 560, 20, 600, 20, collide=False)
    C.vertex("h_name", BD_NAME, txt(16, INK, bold=True), 560, 40, 600, 30, collide=False)
    C.vertex("h_sub", SUBSYS, txt(9, GREY_D), 560, 70, 600, 20, collide=False)
    C.vertex("h_bdno", f"BLOCK DIAGRAM No.&#160;&#160; {BD_NUMBER}",
             cell_box(BLUE, BLUE_D, WHITE, 11, True, sl=8), 1180, 20, 440, 30, collide=False)
    C.vertex("h_cls", "CLASSIFICATION&#160;&#160; «set per programme»",
             cell_box(WHITE, HAIR, INK, 10, sl=8), 1180, 50, 440, 20, collide=False)
    C.vertex("h_rev", "REVISION&#160; A", cell_box(WHITE, HAIR, INK, 10),
             1180, 70, 150, 20, collide=False)
    C.vertex("h_date", "DATE&#160; 2026-09-23", cell_box(WHITE, HAIR, INK, 10),
             1330, 70, 150, 20, collide=False)
    C.vertex("h_sheet", "SHEET&#160; 1 of 1", cell_box(WHITE, HAIR, INK, 10),
             1480, 70, 140, 20, collide=False)

    # --- zones (backdrops, NOT obstacles) -------------------------------------
    C.vertex("ps_bg", "", f"rounded=1;arcSize=2;html=1;fillColor=none;strokeColor={BLUE};"
             f"strokeWidth=1.5;dashed=1;", 40, 180, 1570, 680, collide=False)
    C.vertex("ps_lbl", "PROCESSING SYSTEM — PetaLinux, dual Cortex-A9   (single PL owner: radiod)",
             txt(9, BLUE, ital=True, align="left"), 60, 114, 700, 20, collide=False)

    C.vertex("sca_bg", "", f"rounded=1;arcSize=3;html=1;fillColor=#ECE5F3;strokeColor=#6E4D8F;"
             f"strokeWidth=2;dashed=1;", 620, 370, 660, 300, collide=False)
    C.vertex("sca_lbl", "SCA 2.2.2 / REDHAWK CF  —  NEW LAYER",
             txt(10, "#503669", bold=True, ital=True, align="right"),
             950, 350, 320, 20)

    C.vertex("pl_bg", "", f"rounded=1;arcSize=2;html=1;fillColor=#DEEDE6;strokeColor=#2C5A45;"
             f"strokeWidth=1.5;dashed=1;", 40, 870, 1570, 150, collide=False)
    C.vertex("pl_lbl", "PL (FPGA FABRIC) + RF/HW  —  contracts undefined (U-03/U-04)",
             txt(10, "#2C5A45", ital=True, align="right"), 950, 840, 660, 20)

    # --- banners --------------------------------------------------------------
    for bid, x, w, dom, text in BANNERS:
        st, fill, _, _ = DOMAIN[dom]
        C.vertex(bid, text, banner_style(fill, st), x, 140, w, 30)

    # --- cards ----------------------------------------------------------------
    num = 1
    for cid, x, y, w, h, dom, status, title, chips in CARDS:
        st, fill, pale, ptext = DOMAIN[dom]
        C.vertex(cid, title, card_style(st, status), x, y, w, h)
        C.vertex(cid + "_ac", "", accent_style(fill), x, y + 30, 10, h - 60)
        C.vertex(cid + "_bg", f"{num:02d}", badge_style(fill), x - 10, y - 10, 30, 30)
        # status tag, top-right of the card
        tag, tfill, tcol = STATUS_TAG[status]
        ty = (y + h - 30) if h >= 140 else (y + 10)
        C.vertex(cid + "_tg", tag, chip_style(tfill, tcol, tcol), x + w - 80, ty, 70, 20)
        # chips
        cy = y + CARD_TITLE_H + 10
        for i, ch in enumerate(chips):
            C.vertex(f"{cid}_c{i}", ch, chip_style(pale, ptext, st),
                     x + 10, cy + i * 30, w - 20, 20)
        num += 1

    # --- key note band --------------------------------------------------------
    C.vertex("note_band",
             "NOTE 1 PRESERVED — REDHAWK reaches hardware ONLY through librc / ICD-2 into radiod. "
             "radiod remains the single OpenCPI ACI instance; the CF never opens OpenCPI itself.",
             cell_box("#ECE5F3", "#6E4D8F", "#503669", 10, bold=True, align="center", sl=0),
             60, 1020, 1530, 24)

    # --- footer title block ---------------------------------------------------
    def row(cid, x, y, w, val):
        C.vertex(cid, val, cell_box(WHITE, HAIR, INK, 9, align="left"),
                 x, y, w, 30, collide=False)

    row("tb_dno", 40, 1080, 330, f"DRAWING No.&#160; {BD_NUMBER}")
    row("tb_ttl", 370, 1080, 620, "TITLE&#160; " + BD_NAME)
    row("tb_sz", 990, 1080, 80, "SIZE&#160; A3")
    row("tb_sc", 1070, 1080, 200, "SCALE&#160; NTS")
    row("tb_st", 1270, 1080, 340, "STATUS&#160; CONCEPT — FOR REVIEW")
    row("tb_pb", 40, 1110, 390, "PREPARED BY&#160; «Name» · 2026-09-23")
    row("tb_cb", 430, 1110, 390, "CHECKED BY&#160; «Name»")
    row("tb_ab", 820, 1110, 390, "APPROVED BY&#160; «Name»")
    row("tb_cl", 1210, 1110, 400, "CLASSIFICATION&#160; «set per programme»")

    C.vertex("tb_mat",
             "CONCEPT STAGE  ·  Blocks marked TO BUILD / BLOCKED do not exist. "
             "BLOCKED items await the contracts recorded in unknown.md (U-01, U-03, U-04, U-07, U-09, U-11, U-12).",
             cell_box("#F7E3E2", RED, "#912B27", 9, ital=True, align="center", sl=0),
             40, 1145, 1570, 20, collide=False)

    # --- legend ---------------------------------------------------------------
    LEGEND = [("Control — rc_* / ICD-2", RED),
              ("CORBA / GIOP  (NEW)", "#6E4D8F"),
              ("Data — PDU / skb / DMA", AMBER_D),
              ("Time — 1PPS / PHC", "#1F4E5B")]
    C.vertex("lg_ttl", "LEGEND", txt(9, INK, bold=True), 60, 1050, 60, 20, collide=False)
    for i, (lab, col) in enumerate(LEGEND):
        lx = 130 + i * 360
        C.vertex(f"lg_s{i}", "", f"rounded=0;html=1;fillColor={col};strokeColor=none;",
                 lx, 1055, 30, 10, collide=False)
        C.vertex(f"lg_t{i}", lab, txt(9, INK), lx + 40, 1050, 250, 20, collide=False)

    # --- ROUTING --------------------------------------------------------------
    all_ids = list(C.rects.keys())

    def own_ids(base):
        return {i for i in all_ids if i == base or i.startswith(base + "_")}

    used = {}

    def estyle(color, width, dashed=False, arrow="classic", fill=1):
        return (f"edgeStyle=orthogonalEdgeStyle;rounded=0;html=1;jettySize=auto;"
                f"{'dashed=1;dashPattern=6 4;' if dashed else ''}"
                f"strokeColor={color};strokeWidth={width};endArrow={arrow};endFill={fill};"
                f"fontFamily=Arial;fontSize=9;fontColor={INK};shadow=0;"
                f"labelBackgroundColor=#FFFFFF;")

    STYLES = {
        "primary": estyle(RED, 3),
        "control": estyle(RED, 2),
        "corba":   estyle("#6E4D8F", 2.5, dashed=True),
        "data":    estyle(AMBER_D, 2.5),
        "time":    estyle("#1F4E5B", 1.5, dashed=True, arrow="open", fill=0),
    }
    LABELS = {
        ("a2", "d2"): "rc_* API",
        ("d2", "d3"): "ICD-2 TLV",
        ("c1", "d3"): "librc",
        ("d3", "e1"): "OpenCPI ACI",
        ("d5", "e2"): "ICD-1 DMA",
        ("a3", "b1"): "deploy",
        ("b1", "c1"): "CORBA",
        ("b2", "c1"): "CORBA",
    }

    # Edges forced onto a specific (exit, entry) side pair to keep them out of a
    # neighbour's lane. e10 must enter d2 from the NORTH or it shares e9's track.
    FORCED_SIDES = {("a5", "d2"): [("W", "N")]}

    report = []
    for i, (s, t, kind) in enumerate(EDGES, start=1):
        skip = own_ids(s) | own_ids(t)
        obstacles = [C.rects[k] for k in all_ids if k not in skip]
        r = route(C.rects[s], C.rects[t], obstacles, used, PW, PH,
                  grid=10, margin=8, stub=10, turn_penalty=4, lane_penalty=14,
                  sides=FORCED_SIDES.get((s, t)))
        C.edge(f"edge{i}", s, t, STYLES[kind], r.waypoints, LABELS.get((s, t), ""))
        report.append((f"edge{i}", s, t, kind, r.bends, r.fallback))

    body = "\n".join(C.rows)
    xml = ('<mxfile host="app.diagrams.net" version="24.0.0">\n'
           f'  <diagram name="HH-SDR + REDHAWK SCA" id="oslbd0102">\n'
           f'    <mxGraphModel dx="1600" dy="1100" grid="1" gridSize="10" guides="1" '
           f'tooltips="1" connect="1" arrows="1" fold="1" page="1" pageScale="1" '
           f'pageWidth="{PW}" pageHeight="{PH}" math="0" shadow="0">\n'
           f'      <root>\n'
           f'        <mxCell id="0"/>\n'
           f'        <mxCell id="1" parent="0"/>\n'
           f'{body}\n'
           f'      </root>\n'
           f'    </mxGraphModel>\n'
           f'  </diagram>\n'
           f'</mxfile>\n')
    return xml, report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--logo", default=os.path.join(SKILL, "assets", "osl_logo.png"))
    args = ap.parse_args()
    xml, report = build(args.logo)
    with open(args.out, "w") as f:
        f.write(xml)
    print("wrote", args.out)
    fb = [r for r in report if r[5]]
    print(f"edges: {len(report)}  fallback(non-clean): {len(fb)}")
    for eid, s, t, kind, bends, fbk in report:
        print(f"  {eid}: {s:>4} -> {t:<4} [{kind:<8}] bends={bends} {'FALLBACK' if fbk else ''}")


if __name__ == "__main__":
    main()
