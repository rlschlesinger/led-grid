#!/usr/bin/env python3
"""Refresh the team list built into the web page from ESPN.

    python3 tools/teams.py

ESPN's team lists can't be fetched from the page itself (no CORS headers),
so the page carries a copy: league, ESPN team id, abbreviation, name, and two
colours already adjusted for LEDs. It rewrites the TEAMS line in
web/index.html. Teams rarely change; rerun after a relocation or rebrand.
"""
import json, os, urllib.request

LEAGUES = [("nhl", "hockey"), ("nba", "basketball"), ("nfl", "football"), ("mlb", "baseball")]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, "web", "index.html")


def rgb(h):
    h = (h or "000000").lstrip("#")
    return [int(h[i:i + 2], 16) for i in (0, 2, 4)] if len(h) == 6 else [0, 0, 0]


def led(c, peak=230):
    """Brighten a colour so its strongest channel reaches `peak`: team navies
    and maroons are far too dark to read on an LED panel as they are."""
    m = max(c)
    return [255, 255, 255] if m == 0 else [round(v * peak / m) for v in c]


def hexs(c):
    return "".join(f"{v:02x}" for v in c)


def dark(c):
    return max(c) < 0x30


def near(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) < 90


teams = []
for league, sport in LEAGUES:
    url = f"https://site.api.espn.com/apis/site/v2/sports/{sport}/{league}/teams"
    data = json.load(urllib.request.urlopen(url, timeout=20))
    for entry in data["sports"][0]["leagues"][0]["teams"]:
        t = entry["team"]
        p, a = rgb(t.get("color")), rgb(t.get("alternateColor"))
        if dark(p) and not dark(a):          # black primaries: lead with the alternate
            p, a = a, p
        c1 = led(p)
        c2 = [255, 255, 255] if dark(a) or near(led(a), c1) else led(a)
        teams.append([league, int(t["id"]), t["abbreviation"], t["shortDisplayName"], hexs(c1), hexs(c2)])
    print(f"{league}: {sum(1 for x in teams if x[0] == league)} teams")

line = "const TEAMS = " + json.dumps(teams, separators=(",", ":")) + ";   // tools/teams.py"
html = open(PAGE).read()
start = html.index("const TEAMS = ")
end = html.index("\n", start)
open(PAGE, "w").write(html[:start] + line + html[end:])
print(f"wrote {len(teams)} teams into web/index.html ({len(line)} bytes)")
