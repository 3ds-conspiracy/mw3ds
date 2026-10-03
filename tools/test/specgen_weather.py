"""Weather and daylight oracle, from OpenMW (mwworld/weather.cpp, mwscript/skyextensions.cpp; spec/weather.md).
Writes tools/test/cases/openmw-spec-weather.txt. Values come from the converted data (out/world/game*.json: each weather's
four colours per time of day, fog depths, the regions' chances) and the timing constants OpenMW reads from the
Morrowind.ini fallbacks (build/openmw-full/files/openmw.cfg), which are the vanilla Morrowind.ini values.

Most of the EXPECT kinds and setup tokens below do not exist in the engine yet: spec/findings/weather.md,
"Hooks needed". Only `weathernow`, `hour`, HOUR, SETGLOBAL, GOTO and SLEEP exist today.

    python tools/test/specgen_weather.py [--seed N] [--count N]
"""
import argparse
import random
import re
import sys
from pathlib import Path

sys.path[:0] = [str(Path(__file__).parent), str(Path(__file__).resolve().parents[1] / "convert")]
from mwfiles import MORROWIND_DIR                  # noqa: E402
import specdata                                  # noqa: E402
from openmw_specgen import Data, Test, TESTS       # noqa: E402,F401  (Test writes into TESTS)

ROOT = Path(__file__).resolve().parents[2]
CFG = ROOT / "build" / "openmw-full" / "files" / "openmw.cfg"
WEATHERS = ["Clear", "Cloudy", "Foggy", "Overcast", "Rain", "Thunderstorm", "Ashstorm", "Blight", "Snow", "Blizzard"]
KINDS = ["sky", "fog", "ambient", "sun"]
# a real exterior cell in a region, and an interior, for the tests that need to stand somewhere
EXTERIORS = {"bitter coast region": "Seyda_Neen", "west gash region": "Balmora", "ashlands region": "Ald-ruhn"}
INTERIOR = "Balmora,_Guild_of_Mages"


def sp(name):
    """A region name inside a test token: '%' stands for a space (the steps are split at spaces)."""
    return name.replace(" ", "%")


def load_cfg():
    fb = {}
    for line in CFG.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"fallback=([^,]+),(.*)$", line.strip())
        if m:
            fb[m.group(1)] = m.group(2).strip()
    # the cfg lacks some sections (Ashstorm, Blight): the game's own Morrowind.ini has every one
    ini = MORROWIND_DIR / "Morrowind.ini"
    if ini.exists():
        sec = None
        for line in ini.read_text(encoding="latin-1").splitlines():
            line = line.strip()
            m = re.match(r"\[Weather (\w+)\]$", line)
            if m:
                sec = m.group(1)
            elif line.startswith("["):
                sec = None
            elif sec and "=" in line:
                k, v = line.split("=", 1)
                fb.setdefault(f"Weather_{sec}_{k.strip().replace(' ', '_')}", v.split(";")[0].strip())
    return fb


class Clock:
    """OpenMW's WeatherManager constructor: the day's four boundaries and each channel's lead / lag times."""

    def __init__(self, fb):
        f = lambda k: float(fb[k])
        self.sunrise, self.sunset = f("Weather_Sunrise_Time"), f("Weather_Sunset_Time")
        self.sunrise_dur, self.sunset_dur = f("Weather_Sunrise_Duration"), f("Weather_Sunset_Duration")
        self.night_start = self.sunset + self.sunset_dur
        self.night_end = self.sunrise
        self.day_start = self.sunrise + self.sunrise_dur
        self.day_end = self.sunset
        self.set = {k: tuple(f(f"Weather_{k.capitalize()}_{p}") for p in
                             ("Pre-Sunrise_Time", "Post-Sunrise_Time", "Pre-Sunset_Time", "Post-Sunset_Time"))
                    for k in KINDS}
        self.hours_between = f("Weather_Hours_Between_Weather_Changes")

    def tod(self, vals, h, kind):
        """TimeOfDayInterpolator::getValue. vals = (sunrise, day, sunset, night), numbers or tuples."""
        pre_sr, post_sr, pre_ss, post_ss = self.set[kind]
        sr, d, ss, n = vals
        lerp = lambda x, y, a: tuple(xi * (1 - a) + yi * a for xi, yi in zip(x, y)) if isinstance(x, tuple) else x * (1 - a) + y * a
        if h < self.night_end - pre_sr or h > self.night_start + post_ss:
            return n
        if self.night_end - pre_sr <= h <= self.day_start + post_sr:
            dur = self.day_start + post_sr - self.night_end + pre_sr
            mid = self.night_end - pre_sr + dur / 2
            if h <= mid:
                return lerp(sr, n, (mid - h) / dur * 2 if dur > 0 else 0.0)
            return lerp(sr, d, (h - mid) / dur * 2 if dur > 0 else 1.0)
        if self.day_start + post_sr < h < self.day_end - pre_ss:
            return d
        dur = self.night_start + post_ss - self.day_end + pre_ss
        mid = self.day_end - pre_ss + dur / 2
        if h <= mid:
            return lerp(ss, d, (mid - h) / dur * 2 if dur > 0 else 0.0)
        return lerp(ss, n, (h - mid) / dur * 2 if dur > 0 else 1.0)

    def is_day(self, h):                           # the night / day mode switch and torches use this one
        return self.sunrise <= h <= self.night_start

    def sun_enabled(self, h):
        return not (h >= self.night_start or h <= self.sunrise)

    def glare_fade(self, h):
        peak = self.sunrise + (self.night_start - self.sunrise) / 2
        if h < self.sunrise or h > self.night_start:
            return 0.0
        if h < peak:
            return 1 - (peak - h) / (peak - self.sunrise)
        return 1 - (h - peak) / (self.night_start - peak)


def weather_values(g, w, h, clock):
    """What a lone (not transitioning) weather w gives at hour h: four colours (r, g, b) and the fog depth."""
    wt = g["weather_types"][w]
    out = {k: clock.tod(tuple(tuple(c) for c in wt[k]), h, k) for k in KINDS}
    day, night = wt["fog_depth"]
    # OpenMW passes the day depth for sunrise, day and sunset, the night depth for night
    out["depth"] = clock.tod((day, day, day, night), h, "fog")
    return out


def blend(a, b, t):
    return tuple(x * (1 - t) + y * t for x, y in zip(a, b))


def around(t, kind, v, eps=0.012):
    t.expect(kind, v - eps, v + eps)


def hour_pool(clock, rng, n):
    """Hours to sample: every boundary of every channel, their middles, and random ones."""
    pts = {0.0, 3.0, 12.0, 23.5}
    for k in KINDS:
        pre_sr, post_sr, pre_ss, post_ss = clock.set[k]
        a, b = clock.night_end - pre_sr, clock.day_start + post_sr
        c, d = clock.day_end - pre_ss, clock.night_start + post_ss
        pts |= {a - 0.1, a, (a + b) / 2, b, b + 0.1, c - 0.1, c, (c + d) / 2, d, d + 0.1}
    pts |= {clock.sunrise, clock.sunset, clock.night_start, clock.day_start}
    pts = sorted(round(p, 3) for p in pts if 0 <= p < 24)
    rng.shuffle(pts)
    return pts[:n] + [round(rng.uniform(0, 24), 2) for _ in range(max(2, n // 4))]


def gen(d, rng, count):
    g = specdata.load("game.json")
    fb = load_cfg()
    clock = Clock(fb)
    fnum = lambda w, key: float(fb[f"Weather_{WEATHERS[w]}_{key}"])
    storm_speed = d.f("fstromwindspeed", 0.7)
    t = Test()
    t.step("GOTO:Seyda_Neen", 2)
    t.step("SETGLOBAL:timescale:0")                 # the hour stays where the test puts it
    t.wait(1)

    # ---- 1. colours and fog depth by hour, for one weather forced on the region (no transition)
    for w in range(10):
        t.step(f"SETWEATHER:{w}")
        for h in hour_pool(clock, rng, count):
            t.step(f"HOUR:{h}")
            v = weather_values(g, w, h, clock)
            t.expect(f"weathernow", w, w)
            kind = rng.choice(KINDS)
            ch = rng.choice("rgb")
            around(t, f"wcolor:{kind},{ch}", v[kind]["rgb".index(ch)])
            around(t, "wfogdepth", v["depth"], 0.01)
            if rng.random() < 0.3:                  # all of the ambient and sun terms of one hour (they light the world)
                for kk in ("ambient", "sun", "sky", "fog"):
                    for c in range(3):
                        around(t, f"wcolor:{kk},{'rgb'[c]}", v[kk][c])

    # ---- 2. storms, precipitation, the rain loop, glare (per weather, hour-independent)
    for w in range(10):
        t.step(f"SETWEATHER:{w}")
        t.step("HOUR:12")
        wind = fnum(w, "Wind_Speed")
        t.expect("wstorm", int(wind > storm_speed), int(wind > storm_speed))
        rain = fb[f"Weather_{WEATHERS[w]}_Using_Precip"] == "1" if f"Weather_{WEATHERS[w]}_Using_Precip" in fb else False
        particle = WEATHERS[w] in ("Ashstorm", "Blight", "Snow", "Blizzard")
        precip = (rain or particle) and WEATHERS[w] != "Ashstorm"     # an ash storm does not count as precipitation
        t.expect("wprecip", int(precip), int(precip))
        around(t, "wsunvis", fnum(w, "Glare_View"), 0.01)

    # ---- 3. the base wind a weather aims for: min(8 * wind speed, 70) and what the gust can't leave
    for w in range(10):
        t.step(f"SETWEATHER:{w}")
        target = min(8 * fnum(w, "Wind_Speed"), 70.0)
        t.step("HOUR:12")
        t.wait(2)
        t.expect("wwind", 0.5 * target - 0.001, 2.0 * target + 0.001) if target > 0 else t.expect("wwind", 0, 0)

    # ---- 4. region chances (the tables ChangeWeather / ModRegion work on); a missing column is 0
    for rid, reg in g["regions"].items():
        ch = list(reg["chances"]) + [0] * (10 - len(reg["chances"]))
        for w in range(10):
            t.expect(f"regionchance:{sp(rid)},{w}", ch[w], ch[w])

    # ---- 5. what the hour does besides colour: the sun, torches, glare fade, night / day mode
    for w in (0, 1, 4, 6):
        t.step(f"SETWEATHER:{w}")
        for h in sorted({round(rng.uniform(0, 24), 2) for _ in range(count // 2)}
                        | {clock.sunrise, clock.night_start, clock.sunrise + 0.01, clock.night_start - 0.01,
                           clock.night_start + 0.01, clock.sunrise - 0.01}):
            t.step(f"HOUR:{h}")
            t.expect("wsunon", int(clock.sun_enabled(h)), int(clock.sun_enabled(h)))
            around(t, "wglarefade", clock.glare_fade(h), 0.01)
            dark = h < clock.sunrise or h > clock.night_start
            rainy = (WEATHERS[w] in ("Rain", "Thunderstorm", "Snow", "Blizzard", "Blight"))
            t.expect("wtorches", int(dark and not rainy), int(dark and not rainy))

    # ---- 6. a transition: ChangeWeather on the current region starts it at once; it runs at the NEW weather's delta
    #         (per real second); GetCurrentWeather answers the OUTGOING weather until it ends
    region, cell = "bitter coast region", EXTERIORS["bitter coast region"]
    for a, b in ((0, 4), (4, 5), (5, 0), (0, 9), (1, 6)):
        delta = fnum(b, "Transition_Delta")
        t.step(f"GOTO:{cell}", 1)
        t.step("HOUR:12")
        t.step(f"SETWEATHER:{a}")
        t.step(f"CHANGEWEATHER:{sp(region)}:{b}", 0.2)
        t.expect("weathernow", a, a)
        t.expect("wnext", b, b)
        t.expect("wtrans", 0.0, 0.1)                # the blend factor toward b (1 - OpenMW's mTransitionFactor)
        wait = max(1.0, min(0.5 / delta, 40.0))
        t.wait(wait)
        lo_f, hi_f = max(0, (wait - 2) * delta), min(1, (wait + 3) * delta)
        t.expect("wtrans", lo_f, hi_f)
        for kind in ("sky", "ambient"):
            va, vb = weather_values(g, a, 12, clock), weather_values(g, b, 12, clock)
            ch = rng.choice("rgb")
            i = "rgb".index(ch)
            e = sorted([blend(va[kind], vb[kind], lo_f)[i], blend(va[kind], vb[kind], hi_f)[i]])
            t.expect(f"wcolor:{kind},{ch}", e[0] - 0.012, e[1] + 0.012)
        t.wait(1 / delta + 2)
        t.expect("weathernow", b, b)
        t.expect("wnext", -1, -1)

    # ---- 7. queued: a second ChangeWeather during a transition waits; a third replaces it; the target itself is ignored
    t.step(f"SETWEATHER:0")
    t.step(f"CHANGEWEATHER:{sp(region)}:4", 0.2)
    t.step(f"CHANGEWEATHER:{sp(region)}:6", 0.2)
    t.step(f"CHANGEWEATHER:{sp(region)}:8", 0.2)
    t.expect("wnext", 4, 4)
    t.expect("wqueued", 8, 8)
    t.step(f"CHANGEWEATHER:{sp(region)}:4", 0.2)         # the same as the one in transition: no change to the queue
    t.expect("wqueued", 8, 8)
    t.wait(1 / fnum(4, "Transition_Delta") + 2)
    t.expect("weathernow", 4, 4)
    t.expect("wnext", 8, 8)                          # the queued one has begun
    t.wait(1 / fnum(8, "Transition_Delta") + 2)
    t.expect("weathernow", 8, 8)
    t.expect("wnext", -1, -1)

    # ---- 8. a transition fast-forwards when time is skipped (sleep / wait / travel)
    t.step("SETWEATHER:0")
    t.step(f"CHANGEWEATHER:{sp(region)}:5", 0.2)
    t.step("SLEEP:1:2", 5)
    t.expect("weathernow", 5, 5)
    t.expect("wnext", -1, -1)

    # ---- 9. ChangeWeather: an unknown weather number or region does nothing; the weather is not permanent
    t.step("SETWEATHER:0")
    t.step(f"CHANGEWEATHER:{sp(region)}:10", 0.2)
    t.step(f"CHANGEWEATHER:{sp(region)}:-1", 0.2)
    t.step("CHANGEWEATHER:no%such%region:4", 0.2)
    t.expect("wnext", -1, -1)
    t.expect("weathernow", 0, 0)
    # a region that is not current only stores it: the player is in bitter coast, so west gash waits
    t.step("CHANGEWEATHER:west%gash%region:6", 0.2)
    t.expect("wnext", -1, -1)
    t.expect("regionweather:west%gash%region", 6, 6)

    # ---- 10. ModRegion: the chances are replaced (missing ones become 0, each clamped to 0..100); the current weather
    #          stays while the region still allows it, else a new one is chosen from the new chances
    t.step(f"SETWEATHER:1")
    t.step(f"MODREGION:{sp(region)}:0,100,0,0,0,0,0,0,0,0", 0.2)    # 1 (cloudy) still 100: no change
    t.expect("regionchance:bitter%coast%region,1", 100, 100)
    t.expect("regionchance:bitter%coast%region,0", 0, 0)
    t.expect("wnext", -1, -1)
    t.step(f"MODREGION:{sp(region)}:0,0,0,0,100", 0.2)              # only rain, a short list: the rest 0
    t.expect("regionchance:bitter%coast%region,4", 100, 100)
    t.expect("regionchance:bitter%coast%region,9", 0, 0)
    t.expect("wnext", 4, 4)                                     # cloudy is gone: it rolls rain (the only choice)
    t.step(f"MODREGION:{sp(region)}:150,-5,0,0,0,0,0,0,0,0", 0.2)   # clamped to 100 and 0
    t.expect("regionchance:bitter%coast%region,0", 100, 100)
    t.expect("regionchance:bitter%coast%region,1", 0, 0)
    t.wait(1 / fnum(4, "Transition_Delta") + 1 / fnum(0, "Transition_Delta") + 4)   # rain ends, then the queued clear
    t.expect("weathernow", 0, 0)

    # ---- 11. the transition ending and a region weather that leaves exactly one possibility (a roll that cannot vary)
    for rid, reg in g["regions"].items():
        for w in (0, 3, 7):
            t.step(f"MODREGION:{sp(rid)}:" + ",".join("100" if i == w else "0" for i in range(10)), 0.2)
            t.expect(f"regionchance:{sp(rid)},{w}", 100, 100)
    # a region that was fed 0 everywhere: nothing matches, the roll falls back to Clear
    t.step(f"GOTO:{cell}", 1)
    t.step("SETWEATHER:5")
    t.step(f"MODREGION:{sp(region)}:0,0,0,0,0,0,0,0,0,0", 0.2)
    t.expect("wnext", 0, 0)

    # ---- 12. interiors: the sky is off, no wind, no ambient / rain loops; the transition still runs
    t.step(f"GOTO:{cell}", 1)
    t.step("SETWEATHER:0")
    t.step(f"GOTO:{INTERIOR}", 1.5)
    t.expect("wskyon", 0, 0)
    t.expect("wwind", 0, 0)
    t.expect("wloop", 0, 0)
    t.step(f"CHANGEWEATHER:{sp(region)}:4", 0.2)         # the region the player left is still the current one
    t.wait(1 / fnum(4, "Transition_Delta") + 2)
    t.expect("weathernow", 4, 4)                     # it advanced while indoors
    t.expect("wskyon", 0, 0)
    t.step("HOUR:12")
    t.expect("wnightday", 1 if fnum(4, "Glare_View") >= 0.5 else 0, 1 if fnum(4, "Glare_View") >= 0.5 else 0)
    t.step("SETWEATHER:0")                           # clear weather (glare 1) by day: the interior day mode
    t.expect("wnightday", 1, 1)
    t.step("HOUR:23")
    t.expect("wnightday", 0, 0)
    t.step(f"GOTO:{cell}", 1.5)
    t.step("HOUR:23")
    t.expect("wnightday", 2, 2)                      # exterior night
    t.expect("wskyon", 1, 1)

    # ---- 13. leaving a region by travel: the new region's weather applies at once with no transition
    t.step(f"GOTO:{cell}", 1)
    t.step("SETWEATHER:3")
    t.step("MODREGION:west%gash%region:100,0,0,0,0,0,0,0,0,0", 0.2)
    t.step(f"GOTO:{EXTERIORS['west gash region']}", 2)
    t.expect("weathernow", 0, 0)
    t.expect("wnext", -1, -1)

    # ---- 14. day length: game hours pass at timescale per real second / 3600 (30: a day in 48 real minutes)
    t.step("SETGLOBAL:timescale:30")
    t.step("HOUR:10")
    t.step("SNAP:hour", 0.1)
    t.wait(60)
    t.step("EXPECT:hour:ge:@0.45", 0.1)             # 60 s * 30 / 3600 = 0.5 h
    t.step("EXPECT:hour:le:@0.56", 0.1)
    t.step("SETGLOBAL:timescale:0")
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=20260929)
    ap.add_argument("--count", type=int, default=6)
    args = ap.parse_args()
    n = gen(Data(), random.Random(args.seed), args.count).save("openmw-spec-weather.txt")
    print(f"openmw-spec-weather: {n} steps")


if __name__ == "__main__":
    main()
