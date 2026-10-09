#!/usr/bin/env python3
"""
build_plain_bd.py - plain HH-SDR + REDHAWK SCA block diagram.

No logo, no title block, no numbering, no status tags, no corporate marks.
Colour used for ONE job only: separating the planes.

  control plane  blue
  SCA / CORBA    purple
  data plane     amber
  time plane     teal
  PL / RF        green

Connectors routed by the obstacle-avoiding router so no arrow crosses a block
or a label.

Usage: python3 build_plain_bd.py <out.drawio>
"""
import argparse, os, sys, html

SKILL = ("/home/ospl/.claude/skills/synced/"
         "f58a9748-1fab-44c0-84d9-2d64487b5b8e_79937046-a967-436a-80ee-ebea1dd77e5d/"
         "pro-draw-io-bd")
sys.path.insert(0, os.path.join(SKILL, "scripts"))
from osl_route import Rect, route

INK = "#1A1A1A"; WHITE = "#FFFFFF"

# plane: (stroke, fill, zone_fill)
PLANE = {
    "control": ("#2A4674", "#E7ECF3", "#F4F7FB"),
    "sca":     ("#6E4D8F", "#ECE5F3", "#F7F4FA"),
    "data":    ("#B07D2B", "#F3E9D6", "#FBF6EC"),
    "time":    ("#2C6E7F", "#DDEBEE", "#F0F6F7"),
    "pl":      ("#3E7A5E", "#DEEDE6", "#F0F7F4"),
}

PW, PH = 1400, 780

# (id, x, y, w, h, plane, title, subtitle)
CARDS = [
    # apps / routing
    ("app",   40,  70, 230, 70, "control", "Applications",        "voice · data · C2"),
    ("manet", 310,  70, 230, 70, "control", "MANET routing",      "hh_node_t"),

    # SCA layer
    ("cf",    600,  70, 240, 70, "sca", "REDHAWK Core Framework", "DomainMgr · DeviceMgr · ORB"),
    ("prof",  880,  70, 240, 70, "sca", "Domain profile",         "SPD · SCD · PRF · SAD · DCD"),
    ("res",   600, 190, 240, 70, "sca", "MANET_Node",             "CF::Resource"),
    ("dev",   880, 190, 240, 70, "sca", "Radio",                  "CF::Device"),
    ("orb",   600, 310, 520, 70, "sca", "ORB thread boundary",    "queue drained by hh_node_tick()"),

    # existing C control plane
    ("ip",     40, 430, 230, 70, "data",    "Linux IP stack",     "skb · DSCP"),
    ("librc", 310, 430, 230, 70, "control", "librc / radioctl",   "C API + CLI"),
    ("radiod",600, 430, 240, 70, "control", "radiod",             "PL owner · opens OpenCPI"),
    ("net0",  880, 430, 240, 70, "data",    "manet0 net_device",  "pdu_dma rings"),
    ("clk",  1160, 430, 200, 70, "time",    "Radio clock driver", "PHC · time regs"),

    # PL / RF
    ("seam",  310, 570, 230, 70, "pl",   "hh_radio_ops_t",        "hardware seam"),
    ("rcc",   600, 570, 240, 70, "pl",   "RCC workers",           "OpenCPI PS"),
    ("pl",    880, 570, 240, 70, "pl",   "PL fabric",             "mac · modem · dma"),
    ("rf",     40, 570, 230, 70, "pl",   "AD9361 + RF front-end", ""),
    ("tb",   1160, 570, 200, 70, "time", "PL time base",          "1PPS · frame / slot"),
]

# (src, tgt, plane, label)
EDGES = [
    ("app",   "manet",  "control", ""),
    ("manet", "librc",  "control", "rc_*"),
    ("librc", "radiod", "control", "ICD-2"),
    ("cf",    "prof",   "sca",     ""),
    ("cf",    "res",    "sca",     ""),
    ("prof",  "dev",    "sca",     ""),
    ("res",   "orb",    "sca",     "CORBA"),
    ("dev",   "orb",    "sca",     "CORBA"),
    ("orb",   "radiod", "control", "librc"),
    ("app",   "ip",     "data",    ""),
    ("ip",    "net0",   "data",    "skb"),
    ("net0",  "pl",     "data",    "DMA"),
    ("radiod","rcc",    "control", "OpenCPI"),
    ("rcc",   "pl",     "pl",      ""),
    ("seam",  "rf",     "pl",      ""),
    ("seam",  "rcc",    "pl",      ""),
    ("clk",   "tb",     "time",    ""),
    ("tb",    "pl",     "time",    "tick"),
]

# (x, y, w, h, plane, label)  - background bands, not obstacles
ZONES = [
    (580,  40, 560, 360, "sca", "SCA 2.2.2 / REDHAWK"),
    ( 20, 545, 1360, 120, "pl", "PL / RF — contracts undefined"),
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


def build():
    C = Cells()

    # zones first (behind everything, never obstacles)
    for i, (x, y, w, h, plane, lab) in enumerate(ZONES):
        stroke, _, zfill = PLANE[plane]
        C.vertex(f"zone{i}", "", f"rounded=0;html=1;fillColor={zfill};strokeColor={stroke};"
                 f"strokeWidth=1;dashed=1;", x, y, w, h, collide=False)
        C.vertex(f"zonelbl{i}", lab,
                 f"text;html=1;strokeColor=none;fillColor=none;fontColor={stroke};"
                 f"fontFamily=Arial;fontSize=10;align=left;verticalAlign=middle;",
                 x + 10, y - 20, 400, 20, collide=False)

    # cards: title bold, subtitle plain, one box
    for cid, x, y, w, h, plane, title, sub in CARDS:
        stroke, fill, _ = PLANE[plane]
        if sub:
            val = f"<b>{title}</b><br><font color=\"#4D4E4C\">{sub}</font>"
        else:
            val = f"<b>{title}</b>"
        C.vertex(cid, val,
                 f"rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor={stroke};"
                 f"fontColor={INK};fontFamily=Arial;fontSize=12;align=center;"
                 f"verticalAlign=middle;strokeWidth=1.5;", x, y, w, h)

    # routing
    all_ids = list(C.rects.keys())
    used = {}

    def estyle(plane):
        stroke, _, _ = PLANE[plane]
        dash = "dashed=1;dashPattern=6 4;" if plane in ("sca", "time") else ""
        return (f"edgeStyle=orthogonalEdgeStyle;rounded=0;html=1;jettySize=auto;{dash}"
                f"strokeColor={stroke};strokeWidth=2;endArrow=classic;endFill=1;"
                f"fontFamily=Arial;fontSize=10;fontColor={INK};"
                f"labelBackgroundColor=#FFFFFF;")

    # (exit, entry) side pairs forced where a default mid-height entry would
    # cross the target's centred label.
    FORCED = {("dev", "orb"): [("S", "N")], ("orb", "radiod"): [("S", "N")],
              ("ip", "net0"): [("S", "W")]}

    report = []
    for i, (s, t, plane, label) in enumerate(EDGES, start=1):
        skip = {s, t}
        obstacles = [C.rects[k] for k in all_ids if k not in skip]
        r = route(C.rects[s], C.rects[t], obstacles, used, PW, PH,
                  grid=10, margin=8, stub=10, turn_penalty=4, lane_penalty=12,
                  sides=FORCED.get((s, t)))
        C.edge(f"edge{i}", s, t, estyle(plane), r.waypoints, label)
        report.append((f"edge{i}", s, t, r.bends, r.fallback))

    # legend: one swatch per plane, bottom-left, out of the routing area
    order = [("control", "control plane"), ("sca", "SCA / CORBA"),
             ("data", "data plane"), ("time", "time plane"), ("pl", "PL / RF")]
    for i, (plane, lab) in enumerate(order):
        stroke, fill, _ = PLANE[plane]
        lx = 40 + i * 230
        C.vertex(f"lgs{i}", "", f"rounded=0;html=1;fillColor={fill};strokeColor={stroke};"
                 f"strokeWidth=1.5;", lx, 700, 26, 16, collide=False)
        C.vertex(f"lgt{i}", lab,
                 f"text;html=1;strokeColor=none;fillColor=none;fontColor={INK};"
                 f"fontFamily=Arial;fontSize=10;align=left;verticalAlign=middle;",
                 lx + 34, 698, 180, 20, collide=False)

    body = "\n".join(C.rows)
    return ('<mxfile host="app.diagrams.net" version="24.0.0">\n'
            '  <diagram name="HH-SDR + REDHAWK SCA" id="plain01">\n'
            f'    <mxGraphModel dx="1400" dy="900" grid="1" gridSize="10" guides="1" '
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
    fb = [r for r in report if r[4]]
    print(f"edges: {len(report)}  fallback(non-clean): {len(fb)}")
    for eid, s, t, bends, fbk in report:
        print(f"  {eid}: {s:>7} -> {t:<7} bends={bends} {'FALLBACK' if fbk else ''}")


if __name__ == "__main__":
    main()
