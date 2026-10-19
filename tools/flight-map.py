#!/usr/bin/env python3
"""Live map of what the grid's Flights mode sees, next to a copy of the grid.

    python3 tools/flight-map.py [grid address]      (default 192.168.1.239)

Then open http://localhost:8765. Planes come from adsb.fi around the grid's
home, the one the grid is showing is highlighted, and the panel on the right
mirrors the LEDs. This script passes requests through to the grid and adsb.fi,
so the page avoids cross-origin blocks.

The map tiles are CARTO's, which need a key: put it in carto.key at the
project root (gitignored), or set CARTO_KEY.
"""
import http.server, json, os, sys, urllib.request

GRID = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.239"
PORT = 8765
RADIUS_NM = 25   # matches FLIGHT_RADIUS_NM in src/flights.h
KEY_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "carto.key")
CARTO_KEY = os.environ.get("CARTO_KEY") or (open(KEY_FILE).read().strip() if os.path.exists(KEY_FILE) else "")
if not CARTO_KEY:
    print("No CARTO key (carto.key or CARTO_KEY): the map will show 'API key required' tiles.")


def get(url, timeout=6):
    req = urllib.request.Request(url, headers={"User-Agent": "led-grid-map/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Grid Flight Map</title>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css">
<script src="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js"></script>
<style>
:root{--bg:#0d1117;--panel:#161b22;--line:#30363d;--text:#e6edf3;--dim:#8b949e;--hot:#ff9f1c}
*{box-sizing:border-box}html,body{margin:0;height:100%;background:var(--bg);color:var(--text);font:14px/1.4 -apple-system,system-ui,sans-serif}
#wrap{display:flex;height:100%}#map{flex:1}
aside{width:320px;background:var(--panel);border-left:1px solid var(--line);padding:16px;overflow:auto}
h2{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:18px 0 8px}h2:first-child{margin-top:0}
#grid{display:grid;grid-template-columns:repeat(16,1fr);gap:2px;background:#000;padding:6px;border-radius:6px}
#grid i{aspect-ratio:1;border-radius:50%;background:#111}
#now{font-size:15px}#now b{color:var(--hot)}
.row{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid var(--line);cursor:pointer}
.row.on{color:var(--hot)}.row span:last-child{color:var(--dim)}.row.on span:last-child{color:var(--hot)}
#status{color:var(--dim);font-size:12px;margin-top:12px}
.plane{transition:transform .8s linear}.lbl{background:none;border:none;box-shadow:none;color:#c9d1d9;font:11px monospace}
.lbl::before{display:none}
@media (max-width:700px){#wrap{flex-direction:column}aside{width:auto;height:45%;border-left:0;border-top:1px solid var(--line)}}
</style></head><body>
<div id="wrap"><div id="map"></div><aside>
<h2>Grid right now</h2><div id="grid"></div>
<h2>Showing</h2><div id="now">…</div>
<h2>In range</h2><div id="list"></div>
<div id="status"></div>
</aside></div>
<script>
const RADIUS_NM = %RADIUS%;
const map = L.map('map', {zoomControl: true});
L.tileLayer('https://basemaps.cartocdn.com/rastertiles/dark_all/{z}/{x}/{y}{r}.png?key=%CARTO_KEY%',
  {attribution: '&copy; OpenStreetMap &copy; CARTO', maxZoom: 18}).addTo(map);
const cells = [...Array(256)].map(() => document.getElementById('grid').appendChild(document.createElement('i')));
let home = null, homeLayer = null, markers = {}, link = null, showing = null;

function planeIcon(track, hot) {
  const c = hot ? '#ff9f1c' : '#58a6ff', s = hot ? 30 : 22;
  return L.divIcon({className: '', iconSize: [s, s], iconAnchor: [s/2, s/2], html:
    `<svg class="plane" width="${s}" height="${s}" viewBox="0 0 24 24" style="transform:rotate(${track||0}deg)">
     <path fill="${c}" stroke="#000" stroke-width=".8" d="M12 1.5c.9 0 1.5 1 1.5 2.2v5.6l8.5 5v2.2l-8.5-2.7v5l2.5 1.9v1.8L12 21.6l-4 .9v-1.8l2.5-1.9v-5L2 16.5v-2.2l8.5-5V3.7c0-1.2.6-2.2 1.5-2.2z"/></svg>`});
}
const miles = (a, b) => map.distance(a, b) / 1609.34;

async function grid() {
  try {
    const hex = await (await fetch('/grid/frame')).text();
    for (let i = 0; i < 256; i++) {
      const r = parseInt(hex.substr(i*6, 2), 16), g = parseInt(hex.substr(i*6+2, 2), 16), b = parseInt(hex.substr(i*6+4, 2), 16);
      const k = 255 / Math.max(r, g, b, 60);    // LEDs run dim; brighten for the screen
      cells[i].style.background = r+g+b < 6 ? '#111' : `rgb(${r*k|0},${g*k|0},${b*k|0})`;
    }
  } catch (e) {}
}

async function tick() {
  try {
    const st = await (await fetch('/grid/state')).json();
    const h = [st.home.lat, st.home.lon];
    if (!home || h[0] !== home[0] || h[1] !== home[1]) {
      home = h;
      if (homeLayer) homeLayer.remove();
      homeLayer = L.layerGroup([
        L.circle(home, {radius: RADIUS_NM * 1852, color: '#30363d', weight: 1, fill: false}),
        L.circleMarker(home, {radius: 6, color: '#fff', fillColor: '#3fb950', fillOpacity: 1}).bindTooltip('Grid home'),
      ]).addTo(map);
      map.fitBounds(L.latLng(home).toBounds(RADIUS_NM * 1852 * 2));
    }
    showing = st.flight && st.flight.callsign ? st.flight : null;
    const f = st.flight || {};
    document.getElementById('now').innerHTML = st.modes[st.mode] !== 'Flights'
      ? `Grid is on <b>${st.modes[st.mode]}</b>, not Flights`
      : showing ? `<b>${f.callsign}</b> ${f.route || ''}<br>${f.type || ''}<br>${f.alt.toLocaleString()} ft · ${f.miles} mi · ${f.mph} mph`
      : 'Clear skies';

    const pl = await (await fetch(`/planes?lat=${home[0]}&lon=${home[1]}`)).json();
    const seen = new Set(), rows = [];
    for (const a of pl.aircraft || []) {
      if (typeof a.alt_baro !== 'number' || a.lat == null) continue;
      const cs = (a.flight || a.r || a.hex).trim().toUpperCase();
      const hot = showing && cs === showing.callsign;
      const pos = [a.lat, a.lon];
      seen.add(a.hex);
      let m = markers[a.hex];
      if (!m) {
        m = markers[a.hex] = L.marker(pos).addTo(map)
          .bindTooltip('', {permanent: true, direction: 'right', offset: [10, 0], className: 'lbl'});
      }
      m.setLatLng(pos).setIcon(planeIcon(a.track, hot)).setZIndexOffset(hot ? 1000 : 0);
      m.setTooltipContent(`${cs} ${Math.round(a.alt_baro / 100)}`);
      rows.push({cs, hot, d: miles(home, pos), alt: a.alt_baro, pos});
      if (hot) {
        if (link) link.setLatLngs([home, pos]); else link = L.polyline([home, pos], {color: '#ff9f1c', dashArray: '4 6', weight: 2}).addTo(map);
      }
    }
    for (const k in markers) if (!seen.has(k)) { markers[k].remove(); delete markers[k]; }
    if (!rows.some(r => r.hot) && link) { link.remove(); link = null; }
    rows.sort((a, b) => a.d - b.d);
    const list = document.getElementById('list');
    list.innerHTML = rows.length ? '' : '<div class="row"><span>Nothing airborne</span></div>';
    for (const r of rows) {
      const el = document.createElement('div');
      el.className = 'row' + (r.hot ? ' on' : '');
      el.innerHTML = `<span>${r.cs}</span><span>${r.d.toFixed(1)} mi · ${r.alt.toLocaleString()} ft</span>`;
      el.onclick = () => map.panTo(r.pos);
      list.appendChild(el);
    }
    document.getElementById('status').textContent = `Updated ${new Date().toLocaleTimeString()} · grid ${st.wifi.ip}`;
  } catch (e) {
    document.getElementById('status').textContent = 'Lost contact: ' + e.message;
  }
}
tick(); setInterval(tick, 5000);
grid(); setInterval(grid, 400);
</script></body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, body, ctype="application/json", code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        try:
            if self.path == "/":
                self.reply(PAGE.replace("%RADIUS%", str(RADIUS_NM)).replace("%CARTO_KEY%", CARTO_KEY).encode(), "text/html; charset=utf-8")
            elif self.path == "/grid/state":
                self.reply(get(f"http://{GRID}/api/state"))
            elif self.path == "/grid/frame":
                self.reply(get(f"http://{GRID}/api/frame"), "text/plain")
            elif self.path.startswith("/planes?"):
                q = dict(p.split("=") for p in self.path.split("?", 1)[1].split("&"))
                lat, lon = float(q["lat"]), float(q["lon"])
                self.reply(get(f"https://opendata.adsb.fi/api/v2/lat/{lat:.4f}/lon/{lon:.4f}/dist/{RADIUS_NM}"))
            else:
                self.reply(b"not found", "text/plain", 404)
        except Exception as e:
            self.reply(json.dumps({"error": str(e)}).encode(), code=502)

    def log_message(self, *args):
        pass


print(f"Flight map for the grid at {GRID}: http://localhost:{PORT}")
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
