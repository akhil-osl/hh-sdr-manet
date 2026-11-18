#!/usr/bin/env python3
"""Web page showing the Docker mesh topology as OLSRd2 sees it.

Reads each node's OLSRd2 links (`nhdpinfo link`) and kernel routes once a
second, and serves them on http://<host>:<port>/ for any number of browsers.
Read-only: the page cannot change the mesh.
"""
import argparse
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from maoi_bridge import DOCKER, LINK_STATES, NET_PREFIX, PREFIX, nodes, telnet

state = {"time": None, "nodes": [], "links": [], "events": []}
lock = threading.Lock()


def node_name(ip):
    return chr(64 + int(ip.rsplit(".", 1)[1]) - 10)


def read_node(name):
    links = telnet(name, "nhdpinfo link")
    r = subprocess.run([DOCKER, "exec", PREFIX + name, "ip", "-j", "route"],
                       capture_output=True, text=True, timeout=5)
    routes = []
    if r.returncode == 0:
        for rt in json.loads(r.stdout or "[]"):
            if rt.get("dst", "").startswith(NET_PREFIX) and "gateway" in rt:
                routes.append({"dst": node_name(rt["dst"]), "via": node_name(rt["gateway"])})
    neighbors = {}
    for f in links or []:
        if len(f) > 1 and f[1].startswith(NET_PREFIX):
            neighbors[node_name(f[1])] = next((LINK_STATES[t] for t in f if t in LINK_STATES), "degraded")
    return {"name": name, "ip": NET_PREFIX + str(ord(name) - 54), "olsrd2": links is not None,
            "neighbors": neighbors, "routes": sorted(routes, key=lambda x: x["dst"])}


def snapshot():
    ns = [read_node(n) for n in nodes()]
    seen = {n["name"]: n["neighbors"] for n in ns}
    links = {}
    for a, nb in seen.items():
        for b, st in nb.items():
            key = tuple(sorted((a, b)))
            both = seen.get(b, {}).get(a) == "good" and st == "good"
            links[key] = "good" if both else "degraded"
    return ns, [{"a": a, "b": b, "state": s} for (a, b), s in sorted(links.items())]


def poll(interval):
    prev = {}
    while True:
        try:
            ns, links = snapshot()
        except Exception as e:
            ns, links = [], []
            print("poll failed:", e, flush=True)
        now = time.strftime("%H:%M:%S", time.gmtime())
        cur = {(l["a"], l["b"]): l["state"] for l in links}
        events = []
        for k in sorted(set(prev) | set(cur)):
            if prev.get(k) != cur.get(k):
                events.append(f"{now} UTC link {k[0]}-{k[1]}: {prev.get(k, 'none')} -> {cur.get(k, 'none')}")
        prev = cur
        with lock:
            state.update(time=now, nodes=ns, links=links,
                         events=(events[::-1] + state["events"])[:50])
        for e in events:
            print(e, flush=True)
        time.sleep(interval)


PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mesh Topology</title>
<style>
:root{--bg:#f6f8fa;--fg:#102a43;--muted:#6e7f91;--card:#fff;--line:#d9e2ec;--good:#2f9e44;--bad:#e8590c;--off:#adb5bd}
@media (prefers-color-scheme:dark){:root{--bg:#16191d;--fg:#e6edf3;--muted:#8b98a5;--card:#1f2328;--line:#30363d}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 2px} .muted{color:var(--muted);font-size:13px}
.grid{display:grid;grid-template-columns:minmax(0,3fr) minmax(0,2fr);gap:16px;margin-top:12px}
@media (max-width:800px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
svg{width:100%;height:auto;display:block} table{width:100%;border-collapse:collapse;font-size:14px}
td,th{text-align:left;padding:4px 6px;border-bottom:1px solid var(--line)}
.ev{font:13px ui-monospace,monospace;max-height:220px;overflow:auto;white-space:pre}
.key span{display:inline-block;margin-right:14px}
.sw{display:inline-block;width:18px;height:4px;vertical-align:middle;margin-right:4px}
</style></head><body><main>
<h1>Mesh topology</h1>
<div class="muted">Links as OLSRd2 sees them, routes from each node's kernel table. Updated <span id="t">-</span> UTC.</div>
<div class="grid">
 <div class="card"><svg id="g" viewBox="0 0 600 420"></svg>
  <div class="muted key"><span><i class="sw" style="background:var(--good)"></i>symmetric both ways</span>
  <span><i class="sw" style="background:var(--bad)"></i>one-sided or not symmetric</span></div></div>
 <div class="card"><b>Routes</b><table id="r"></table></div>
</div>
<div class="card" style="margin-top:16px"><b>Link changes</b><div class="ev" id="e"></div></div>
</main>
<script>
const NS="http://www.w3.org/2000/svg";
function el(t,a){const e=document.createElementNS(NS,t);for(const k in a)e.setAttribute(k,a[k]);return e}
async function tick(){
 let d; try{d=await (await fetch('api/topology')).json()}catch(e){return}
 document.getElementById('t').textContent=d.time||'-';
 const g=document.getElementById('g'); g.innerHTML='';
 const n=d.nodes.length, pos={};
 d.nodes.forEach((x,i)=>{const a=-Math.PI/2+2*Math.PI*i/Math.max(n,1);
  pos[x.name]=n<=3?[100+200*i,210+(i%2?-90:60)]:[300+160*Math.cos(a),210+160*Math.sin(a)]});
 const css=getComputedStyle(document.documentElement);
 d.links.forEach(l=>{const p=pos[l.a],q=pos[l.b]; if(!p||!q)return;
  g.appendChild(el('line',{x1:p[0],y1:p[1],x2:q[0],y2:q[1],'stroke-width':5,'stroke-linecap':'round',
   stroke:css.getPropertyValue(l.state=='good'?'--good':'--bad')}))});
 d.nodes.forEach(x=>{const [cx,cy]=pos[x.name]; const ok=x.olsrd2&&Object.keys(x.neighbors).length;
  g.appendChild(el('circle',{cx,cy,r:30,fill:css.getPropertyValue('--card'),
   stroke:css.getPropertyValue(ok?'--good':'--off'),'stroke-width':4}));
  const t=el('text',{x:cx,y:cy+7,'text-anchor':'middle','font-size':22,'font-weight':700,fill:css.getPropertyValue('--fg')});
  t.textContent=x.name; g.appendChild(t);
  const s=el('text',{x:cx,y:cy+50,'text-anchor':'middle','font-size':13,fill:css.getPropertyValue('--muted')});
  s.textContent=x.ip+(x.olsrd2?'':' (OLSRd2 down)'); g.appendChild(s)});
 const r=document.getElementById('r');
 r.innerHTML='<tr><th>Node</th><th>Destination</th><th>Next hop</th><th>Path</th></tr>'+
  d.nodes.flatMap(x=>x.routes.map(rt=>`<tr><td>${x.name}</td><td>${rt.dst}</td><td>${rt.via}</td><td>${rt.via==rt.dst?'direct':'relayed'}</td></tr>`)).join('');
 document.getElementById('e').textContent=d.events.join('\\n')||'No changes yet.';
}
tick(); setInterval(tick,1000);
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body, ctype = PAGE.encode(), "text/html; charset=utf-8"
        elif self.path == "/api/topology":
            with lock:
                body, ctype = json.dumps(state).encode(), "application/json"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1", help="0.0.0.0 to allow other machines")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--interval", type=float, default=1.0)
    args = ap.parse_args()
    threading.Thread(target=poll, args=(args.interval,), daemon=True).start()
    print(f"topology page on http://{args.host}:{args.port}/", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
