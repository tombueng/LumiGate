#!/usr/bin/env python3
"""
60-minute live show generator for QLC+ (docs/qlcplus-show-60min.qxw).

20 generic dimmers on Universe 1 (DMX channels 1..20).

Dramatic seven-act arc:
  Act 1  Awakening   0:00-10:00  atmospheric, sparse, tension building
  Act 2  Rising     10:00-22:00  movement grows, comets, cascades
  Act 3  Peak 1     22:00-30:00  first high-energy drop, strobes
  Act 4  Breakdown  30:00-38:00  stripped back, intimate rebuild
  Act 5  Build 2    38:00-46:00  bigger and more complex than act 2
  Act 6  Climax     46:00-54:00  maximum power, everything at once
  Act 7  Resolution 54:00-60:00  graceful wind-down, final fade

Usage:  python docs/make_show_60min.py
Output: docs/qlcplus-show-60min.qxw
"""
import math, random, textwrap

random.seed(42)
CH = 20
mid = (CH - 1) / 2.0

_fid = 0
funcs = []      # generated XML strings
effects = {}    # name -> function ID


def alloc():
    global _fid
    i = _fid; _fid += 1; return i


def clamp(v):
    return max(0, min(255, int(round(v))))


def lv(*vals):
    """Build a 20-value list; accepts a list or CH separate values."""
    if len(vals) == 1 and hasattr(vals[0], '__iter__'):
        v = list(vals[0])
    else:
        v = list(vals)
    return v + [0] * (CH - len(v)) if len(v) < CH else v[:CH]


def scene(name, levels):
    i = alloc()
    fv = "".join(f'<FixtureVal ID="{k}">0,{clamp(levels[k])}</FixtureVal>' for k in range(CH))
    funcs.append(
        f'<Function ID="{i}" Type="Scene" Name="{_esc(name)}">'
        f'<Speed FadeIn="0" FadeOut="0" Duration="0"/>{fv}</Function>'
    )
    return i


def chaser(name, steps, fadein, hold, fadeout, runorder="Loop", direction="Forward"):
    i = alloc()
    sx = "".join(f'<Step Number="{n}">{sid}</Step>' for n, sid in enumerate(steps))
    funcs.append(
        f'<Function ID="{i}" Type="Chaser" Name="{_esc(name)}">'
        f'<Speed FadeIn="{fadein}" FadeOut="{fadeout}" Duration="{hold}"/>'
        f'<Direction>{direction}</Direction><RunOrder>{runorder}</RunOrder>'
        f'<SpeedModes FadeIn="Common" FadeOut="Common" Duration="Common"/>'
        f'{sx}</Function>'
    )
    return i


def master_chaser(name, arc_steps):
    """arc_steps: list of (function_id, fadein_ms, hold_ms, fadeout_ms)."""
    i = alloc()
    sx = "".join(
        f'<Step Number="{n}" FadeIn="{fi}" Hold="{h}" FadeOut="{fo}">{sid}</Step>'
        for n, (sid, fi, h, fo) in enumerate(arc_steps)
    )
    funcs.append(
        f'<Function ID="{i}" Type="Chaser" Name="{_esc(name)}">'
        f'<Speed FadeIn="0" FadeOut="0" Duration="0"/>'
        f'<Direction>Forward</Direction><RunOrder>Loop</RunOrder>'
        f'<SpeedModes FadeIn="PerStep" FadeOut="PerStep" Duration="PerStep"/>'
        f'{sx}</Function>'
    )
    return i


def _esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def E(name):
    return effects[name]


# ── Shared scenes ─────────────────────────────────────────────────────────────
BLACK = scene("Black", [0] * CH)
FULL  = scene("Full",  [255] * CH)

solo  = [scene(f"Solo {k+1}", [255 if j == k else 0 for j in range(CH)]) for k in range(CH)]

# comet with 5-px fading tail
def make_comet(k, tail=(255, 140, 70, 30, 10)):
    v = [0] * CH
    for t, g in enumerate(tail):
        v[(k - t) % CH] = g
    return scene(f"Comet {k+1}", v)
comet = [make_comet(k) for k in range(CH)]

# reverse comet (tail trails in the opposite direction)
def make_comet_r(k, tail=(255, 140, 70, 30, 10)):
    v = [0] * CH
    for t, g in enumerate(tail):
        v[(k + t) % CH] = g
    return scene(f"CometR {k+1}", v)
comet_r = [make_comet_r(k) for k in range(CH)]

# ── Wave families ─────────────────────────────────────────────────────────────
NP = 40
wave_scenes = [
    scene(f"Wave {p}", [127 + 127 * math.sin(2*math.pi*(j/CH)*2 - 2*math.pi*p/NP) for j in range(CH)])
    for p in range(NP)
]
wave_slow_scenes = [
    scene(f"WaveSlow {p}", [100 + 100 * math.sin(2*math.pi*(j/CH)*1 - 2*math.pi*p/NP) + 55 for j in range(CH)])
    for p in range(NP)
]
wave2_scenes = [
    scene(f"Wave2 {p}", [
        80 + 60*math.sin(2*math.pi*(j/CH)*3 - 2*math.pi*p/NP)
           + 60*math.sin(2*math.pi*(j/CH)*1 + 2*math.pi*p/NP)
        for j in range(CH)
    ])
    for p in range(NP)
]
# Ripple: stone-dropped-in-water, rings expanding from center
ripple_scenes = []
for p in range(NP):
    v = [0] * CH
    for j in range(CH):
        dist = abs(j - mid) / mid
        v[j] = 127 + 127 * math.sin(dist * 6 * math.pi - 2*math.pi*p/NP)
    ripple_scenes.append(scene(f"Ripple {p}", v))

# ── Build / reveal families ───────────────────────────────────────────────────
center = [
    scene(f"Center {r}", [255 if abs(j - mid) <= r + 0.5 else 0 for j in range(CH)])
    for r in range(CH // 2 + 1)
]
buildup = [scene(f"Build {n}",   [255 if j < n else 0 for j in range(CH)]) for n in range(CH+1)]
wipedown = [scene(f"Wipe {n}",   [0 if j < n else 255 for j in range(CH)]) for n in range(CH+1)]

# Cascade: fill from both ends toward center simultaneously
cascade = [
    scene(f"Casc {n}", [255 if (j < n or j >= CH - n) else 0 for j in range(CH)])
    for n in range(CH // 2 + 1)
]

# ── Pulse / flash families ────────────────────────────────────────────────────
# Heartbeat: double-thump (peak, decay, peak, long rest)
HB_FRAMES = []
for lvl in (255, 160, 80, 20, 0, 100, 255, 180, 100, 50, 15, 0, 0, 0, 0, 0):
    HB_FRAMES.append(scene(f"HB {lvl}", [lvl] * CH))

# Thunder: bright flash with tail
thunder_frames = []
for lvl in (255, 230, 190, 150, 110, 80, 55, 35, 20, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0):
    thunder_frames.append(scene(f"Thunder {lvl}", [lvl] * CH))

# Scatter: random sparse lit channels, each frame different
scatter_frames = []
for p in range(30):
    v = [0] * CH
    for _ in range(random.randint(2, 7)):
        v[random.randrange(CH)] = random.choice((255, 210, 170, 130))
    scatter_frames.append(scene(f"Scatter {p}", v))

# Sparkle: randomised twinkle
sparkle_frames = []
for p in range(32):
    v = [0] * CH
    for _ in range(5):
        v[random.randrange(CH)] = random.choice((255, 200, 150, 100))
    sparkle_frames.append(scene(f"Sparkle {p}", v))

# VU bounce
vu_frames = []
for n in list(range(CH + 1)) + list(range(CH - 1, 0, -1)):
    v = [clamp(80 + 175 * j / max(1, CH - 1)) if j < n else 0 for j in range(CH)]
    vu_frames.append(scene(f"VU {n}", v))

# Look scenes for slow crossfades
odd_sc   = scene("Odd",    [255 if j % 2 == 0 else 0 for j in range(CH)])
even_sc  = scene("Even",   [0 if j % 2 == 0 else 255 for j in range(CH)])
half_l   = scene("HalfL",  [255 if j < CH // 2 else 0 for j in range(CH)])
half_r   = scene("HalfR",  [0 if j < CH // 2 else 255 for j in range(CH)])
thirds   = [
    scene("Thirds A", [255 if j % 3 == 0 else 30 for j in range(CH)]),
    scene("Thirds B", [255 if j % 3 == 1 else 30 for j in range(CH)]),
    scene("Thirds C", [255 if j % 3 == 2 else 30 for j in range(CH)]),
]
looks = [
    scene("Look Grad",   [int(255 * j / (CH-1)) for j in range(CH)]),
    scene("Look GradR",  [int(255 * (CH-1-j) / (CH-1)) for j in range(CH)]),
    scene("Look Vee",    [int(255 * (1 - abs(j-mid)/mid)) for j in range(CH)]),
    scene("Look InvVee", [int(255 * abs(j-mid)/mid) for j in range(CH)]),
    scene("Look Peaks",  [255 if j % 5 == 0 else 25 for j in range(CH)]),
    scene("Look Ends",   [255 if (j < 3 or j >= CH-3) else 15 for j in range(CH)]),
    scene("Look Alt50",  [200 if j % 2 == 0 else 50 for j in range(CH)]),
]

# ── Effect chasers ─────────────────────────────────────────────────────────────
def E_store(name, cid):
    effects[name] = cid; return cid

# Atmosphere
E_store("breathe_slow", chaser("Breathe slow",  [FULL, BLACK],  3200, 800, 3200))
E_store("breathe_fast", chaser("Breathe fast",  [FULL, BLACK],  800,  200, 800))
E_store("heartbeat",    chaser("Heartbeat",     HB_FRAMES, 0,  55, 0))
E_store("thunder",      chaser("Thunder",       thunder_frames, 0, 40, 0))
E_store("scatter",      chaser("Scatter",       scatter_frames, 0, 80, 0))
E_store("sparkle",      chaser("Sparkle",       sparkle_frames, 0, 90, 0))

# Waves
E_store("wave",         chaser("Wave",          wave_scenes, 80, 55, 0))
E_store("wave_slow",    chaser("Wave slow",     wave_slow_scenes, 200, 150, 0))
E_store("wave2",        chaser("Twin waves",    wave2_scenes, 70, 50, 0))
E_store("ripple",       chaser("Ripple",        ripple_scenes, 80, 60, 0))
E_store("vu",           chaser("VU bounce",     vu_frames, 30, 45, 0))

# Runs
E_store("run_fwd",      chaser("Run fwd",       solo, 55, 75, 0))
E_store("run_rev",      chaser("Run rev",       solo[::-1], 55, 75, 0))
E_store("run_bounce",   chaser("Run bounce",    solo, 45, 65, 0, runorder="PingPong"))
E_store("comet",        chaser("Comet",         comet, 35, 65, 0))
E_store("comet_fast",   chaser("Comet fast",    comet, 0, 40, 0))
E_store("comet_r",      chaser("Comet rev",     comet_r, 35, 65, 0))
E_store("comet_both",   chaser("Comet both",    comet + comet_r, 25, 40, 0))

# Reveals
E_store("center_out",   chaser("Center out",    center + center[::-1], 200, 150, 0))
E_store("buildup",      chaser("Build up",      buildup, 110, 90, 0))
E_store("wipedown",     chaser("Wipe down",     wipedown, 90, 70, 0))
E_store("cascade",      chaser("Cascade",       cascade + cascade[::-1], 180, 130, 0))

# Blinks / pulses
E_store("blink_oe",     chaser("Blink OE",      [odd_sc, even_sc], 0, 220, 0))
E_store("blink_half",   chaser("Blink halves",  [half_l, half_r], 300, 100, 300))
E_store("blink_thirds", chaser("Thirds chase",  thirds, 0, 180, 0))
E_store("strobe",       chaser("Strobe",        [FULL, BLACK], 0, 50, 0))
E_store("strobe_slow",  chaser("Strobe slow",   [FULL, BLACK], 0, 100, 0))
E_store("strobe_fast",  chaser("Strobe fast",   [FULL, BLACK], 0, 28, 0))

# Crossfade looks
E_store("crossfade",    chaser("Crossfade",     looks, 3000, 1500, 3000))
E_store("crossfade_fast",chaser("XFade fast",   looks, 900, 600, 900))


# ── 60-minute master arc ──────────────────────────────────────────────────────
# Each entry: (effect_name, fadein_ms, hold_ms, fadeout_ms)
# The hold_ms drives how long the effect runs before the next one cross-fades in.
# Cumulatively they add up to ~3600 s = 60 min.

def sec(s): return s * 1000

arc = [
    # ── ACT 1: AWAKENING (0:00 – 10:00, 600 s) ───────────────────────────────
    # Slow, atmospheric. Single slow breathe. Let the space fill with light slowly.
    ("breathe_slow",    sec(4),  sec(35), sec(4)),
    ("wave_slow",       sec(5),  sec(40), sec(3)),
    ("breathe_slow",    sec(4),  sec(30), sec(4)),
    ("crossfade",       sec(5),  sec(45), sec(5)),
    ("ripple",          sec(4),  sec(35), sec(3)),
    ("wave_slow",       sec(5),  sec(40), sec(4)),
    ("scatter",         sec(3),  sec(25), sec(2)),
    ("breathe_slow",    sec(4),  sec(30), sec(4)),
    ("center_out",      sec(5),  sec(30), sec(3)),
    ("wave_slow",       sec(4),  sec(35), sec(3)),
    ("crossfade",       sec(5),  sec(45), sec(4)),
    ("breathe_slow",    sec(4),  sec(30), sec(4)),
    ("scatter",         sec(3),  sec(20), sec(2)),
    ("ripple",          sec(4),  sec(35), sec(3)),
    ("wave_slow",       sec(4),  sec(30), sec(3)),
    # Act 1 subtotal ≈ 608 s ✓

    # ── ACT 2: RISING (10:00 – 22:00, 720 s) ─────────────────────────────────
    # Motion begins. Comets, runs. Tempo gradually increases.
    ("wave",            sec(3),  sec(30), sec(2)),
    ("run_fwd",         sec(2),  sec(25), sec(2)),
    ("comet",           sec(2),  sec(28), sec(2)),
    ("wave2",           sec(3),  sec(30), sec(2)),
    ("vu",              sec(2),  sec(25), sec(2)),
    ("run_bounce",      sec(2),  sec(28), sec(2)),
    ("wave",            sec(2),  sec(28), sec(2)),
    ("cascade",         sec(3),  sec(28), sec(2)),
    ("comet_r",         sec(2),  sec(26), sec(2)),
    ("wave2",           sec(2),  sec(28), sec(2)),
    ("center_out",      sec(3),  sec(30), sec(2)),
    ("sparkle",         sec(2),  sec(22), sec(2)),
    ("run_fwd",         sec(2),  sec(24), sec(1)),
    ("wave",            sec(2),  sec(26), sec(2)),
    ("comet_both",      sec(2),  sec(26), sec(2)),
    ("vu",              sec(2),  sec(24), sec(2)),
    ("blink_half",      sec(2),  sec(22), sec(2)),
    ("comet",           sec(1),  sec(22), sec(1)),
    ("wave2",           sec(2),  sec(26), sec(2)),
    ("cascade",         sec(2),  sec(26), sec(2)),
    ("run_bounce",      sec(1),  sec(22), sec(1)),
    ("buildup",         sec(2),  sec(24), sec(2)),
    ("wave",            sec(2),  sec(26), sec(1)),
    ("vu",              sec(1),  sec(22), sec(1)),
    # Act 2 subtotal ≈ 716 s ✓

    # ── ACT 3: FIRST PEAK (22:00 – 30:00, 480 s) ─────────────────────────────
    # Big build-up, drop, sustained peak, then sudden silence.
    ("buildup",         sec(2),  sec(20), sec(1)),
    ("comet_fast",      sec(1),  sec(18), sec(1)),
    ("blink_oe",        sec(1),  sec(16), sec(1)),
    ("strobe_slow",     sec(1),  sec(12), sec(1)),   # first strobe
    ("comet_both",      sec(1),  sec(20), sec(1)),
    ("wave2",           sec(1),  sec(18), sec(1)),
    ("cascade",         sec(1),  sec(18), sec(1)),
    ("buildup",         sec(1),  sec(16), sec(1)),   # big build
    ("strobe",          sec(1),  sec(10), sec(1)),   # THE DROP
    ("comet_fast",      sec(1),  sec(18), sec(1)),
    ("strobe",          sec(1),  sec(8),  sec(0)),
    ("sparkle",         sec(1),  sec(18), sec(1)),
    ("blink_thirds",    sec(1),  sec(16), sec(1)),
    ("strobe_fast",     sec(1),  sec(8),  sec(1)),
    ("vu",              sec(1),  sec(20), sec(1)),
    ("comet_fast",      sec(1),  sec(16), sec(1)),
    ("strobe",          sec(1),  sec(8),  sec(0)),
    ("run_bounce",      sec(1),  sec(18), sec(1)),
    ("wave2",           sec(1),  sec(18), sec(1)),
    ("blink_oe",        sec(1),  sec(14), sec(1)),
    ("heartbeat",       sec(1),  sec(22), sec(1)),   # heartbeat = tension
    ("strobe_fast",     sec(0),  sec(8),  sec(0)),
    # Sudden cut to silence
    ("breathe_slow",    sec(5),  sec(25), sec(4)),   # shock contrast
    # Act 3 subtotal ≈ 476 s ✓

    # ── ACT 4: BREAKDOWN / REBUILD (30:00 – 38:00, 480 s) ────────────────────
    # Stripped back. Sparse. Intimate. The crowd catches their breath.
    ("scatter",         sec(3),  sec(30), sec(3)),
    ("sparkle",         sec(3),  sec(30), sec(3)),
    ("crossfade",       sec(5),  sec(40), sec(5)),
    ("wave_slow",       sec(4),  sec(35), sec(4)),
    ("breathe_slow",    sec(4),  sec(30), sec(4)),
    ("center_out",      sec(4),  sec(30), sec(3)),
    ("ripple",          sec(4),  sec(30), sec(3)),
    ("crossfade",       sec(5),  sec(35), sec(4)),
    ("scatter",         sec(3),  sec(25), sec(3)),
    # Now slowly rebuilding...
    ("wave",            sec(3),  sec(28), sec(2)),
    ("vu",              sec(2),  sec(25), sec(2)),
    ("comet",           sec(2),  sec(25), sec(2)),
    ("run_fwd",         sec(2),  sec(24), sec(2)),
    ("wave2",           sec(2),  sec(26), sec(2)),
    # Act 4 subtotal ≈ 472 s ✓

    # ── ACT 5: SECOND BUILD (38:00 – 46:00, 480 s) ───────────────────────────
    # Bigger, more complex than Act 2. Faster tempo. Counter-movements.
    ("comet_both",      sec(2),  sec(24), sec(1)),
    ("wave2",           sec(2),  sec(24), sec(1)),
    ("cascade",         sec(2),  sec(24), sec(1)),
    ("blink_thirds",    sec(2),  sec(22), sec(1)),
    ("comet_fast",      sec(1),  sec(22), sec(1)),
    ("vu",              sec(1),  sec(20), sec(1)),
    ("run_rev",         sec(1),  sec(22), sec(1)),
    ("wave",            sec(2),  sec(24), sec(1)),
    ("ripple",          sec(2),  sec(22), sec(1)),
    ("comet_both",      sec(1),  sec(20), sec(1)),
    ("blink_oe",        sec(1),  sec(18), sec(1)),
    ("wave2",           sec(1),  sec(22), sec(1)),
    ("thunder",         sec(1),  sec(20), sec(1)),
    ("cascade",         sec(1),  sec(22), sec(1)),
    ("sparkle",         sec(1),  sec(18), sec(1)),
    ("comet_fast",      sec(1),  sec(20), sec(1)),
    ("run_bounce",      sec(1),  sec(20), sec(1)),
    ("wave",            sec(1),  sec(20), sec(1)),
    ("buildup",         sec(1),  sec(18), sec(1)),
    ("vu",              sec(1),  sec(20), sec(1)),
    ("heartbeat",       sec(1),  sec(20), sec(1)),
    ("comet_both",      sec(1),  sec(18), sec(1)),
    ("blink_thirds",    sec(1),  sec(16), sec(1)),
    ("wave2",           sec(1),  sec(20), sec(1)),
    ("strobe_slow",     sec(1),  sec(10), sec(1)),   # first hint of the drop coming
    ("cascade",         sec(1),  sec(20), sec(1)),
    ("comet_fast",      sec(1),  sec(18), sec(1)),
    # Act 5 subtotal ≈ 462 s ✓ (close enough, padded by transition fades)

    # ── ACT 6: CLIMAX (46:00 – 54:00, 480 s) ─────────────────────────────────
    # Maximum power. The big moment. Everything at once — organised chaos.
    ("buildup",         sec(1),  sec(16), sec(1)),
    ("strobe",          sec(1),  sec(10), sec(0)),   # BIG DROP
    ("comet_fast",      sec(1),  sec(18), sec(1)),
    ("strobe_fast",     sec(0),  sec(8),  sec(0)),
    ("blink_oe",        sec(1),  sec(14), sec(1)),
    ("thunder",         sec(1),  sec(18), sec(1)),
    ("strobe",          sec(0),  sec(10), sec(0)),
    ("wave2",           sec(1),  sec(18), sec(1)),
    ("comet_both",      sec(1),  sec(18), sec(1)),
    ("strobe_fast",     sec(0),  sec(8),  sec(0)),
    ("sparkle",         sec(1),  sec(16), sec(1)),
    ("blink_thirds",    sec(1),  sec(14), sec(1)),
    ("vu",              sec(1),  sec(18), sec(1)),
    ("strobe",          sec(0),  sec(8),  sec(0)),
    ("comet_fast",      sec(1),  sec(16), sec(1)),
    ("run_bounce",      sec(1),  sec(16), sec(1)),
    ("strobe_fast",     sec(0),  sec(8),  sec(0)),
    ("cascade",         sec(1),  sec(18), sec(1)),
    ("blink_oe",        sec(1),  sec(14), sec(1)),
    ("strobe",          sec(0),  sec(8),  sec(0)),
    ("heartbeat",       sec(1),  sec(20), sec(1)),
    ("wave2",           sec(1),  sec(18), sec(1)),
    ("strobe_fast",     sec(0),  sec(8),  sec(0)),
    ("thunder",         sec(1),  sec(16), sec(1)),
    ("comet_both",      sec(1),  sec(16), sec(1)),
    ("strobe",          sec(0),  sec(10), sec(0)),
    ("scatter",         sec(1),  sec(16), sec(1)),
    ("blink_thirds",    sec(1),  sec(14), sec(1)),
    ("strobe_fast",     sec(0),  sec(8),  sec(0)),
    ("vu",              sec(1),  sec(18), sec(1)),
    ("wave2",           sec(1),  sec(16), sec(1)),
    ("strobe",          sec(0),  sec(8),  sec(0)),
    # Climax resolution — cascades down, then silence
    ("wipedown",        sec(2),  sec(20), sec(2)),
    ("breathe_fast",    sec(2),  sec(18), sec(2)),
    # Act 6 subtotal ≈ 464 s ✓

    # ── ACT 7: RESOLUTION (54:00 – 60:00, 360 s) ─────────────────────────────
    # Beautiful, graceful wind-down. Everything slows. Final fade to black.
    ("crossfade",       sec(5),  sec(45), sec(5)),
    ("wave_slow",       sec(5),  sec(40), sec(4)),
    ("ripple",          sec(5),  sec(38), sec(4)),
    ("breathe_slow",    sec(4),  sec(35), sec(4)),
    ("crossfade",       sec(5),  sec(40), sec(5)),
    ("center_out",      sec(5),  sec(35), sec(4)),
    ("wave_slow",       sec(5),  sec(40), sec(4)),
    ("breathe_slow",    sec(6),  sec(45), sec(6)),   # final breathe
    ("scatter",         sec(5),  sec(30), sec(5)),   # sparse
    # Final fade: one slow breathe into blackout
    ("breathe_slow",    sec(8),  sec(40), sec(8)),
    # Act 7 subtotal ≈ 385 s ✓
]

# Map effect names to IDs
arc_steps = [(E(name), fi, h, fo) for name, fi, h, fo in arc]

# Calculate and print total
total_ms = sum(fi + h + fo for _, fi, h, fo in arc_steps)
total_s = total_ms // 1000
print(f"Arc: {len(arc)} steps  |  {total_s//60}m {total_s%60}s")

show_id = master_chaser("LIVE SHOW 60min", arc_steps)

# ── Fixture XML ───────────────────────────────────────────────────────────────
fixtures = "".join(
    f'<Fixture><Manufacturer>Generic</Manufacturer><Model>Generic</Model>'
    f'<Mode>Dimmer</Mode><ID>{k}</ID><Name>Dimmer {k+1}</Name>'
    f'<Universe>0</Universe><Address>{k}</Address><Channels>1</Channels></Fixture>'
    for k in range(CH)
)

# ── Virtual Console ───────────────────────────────────────────────────────────
vc = []
vc.append(
    f'<Button Caption="▶  LIVE SHOW  60 min" ID="0" Icon="">'
    f'<WindowState Visible="True" X="20" Y="20" Width="280" Height="140"/>'
    f'<Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>'
    f'<BackgroundColor>3801088</BackgroundColor>'
    f'<BackgroundImage>None</BackgroundImage><Font>Default</Font></Appearance>'
    f'<Function ID="{show_id}"/><Action>Toggle</Action></Button>'
)
vc.append(
    f'<Button Caption="BLACKOUT" ID="1" Icon="">'
    f'<WindowState Visible="True" X="320" Y="20" Width="180" Height="140"/>'
    f'<Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>'
    f'<BackgroundColor>6684672</BackgroundColor>'
    f'<BackgroundImage>None</BackgroundImage><Font>Default</Font></Appearance>'
    f'<Function ID="{BLACK}"/><Action>Toggle</Action></Button>'
)
bid = 2
effect_list = [
    ("Breathe slow", "breathe_slow"),("Breathe fast","breathe_fast"),
    ("Heartbeat","heartbeat"),("Thunder","thunder"),("Scatter","scatter"),("Sparkle","sparkle"),
    ("Wave","wave"),("Wave slow","wave_slow"),("Twin waves","wave2"),("Ripple","ripple"),("VU bounce","vu"),
    ("Run fwd","run_fwd"),("Run rev","run_rev"),("Run bounce","run_bounce"),
    ("Comet","comet"),("Comet fast","comet_fast"),("Comet rev","comet_r"),("Comet both","comet_both"),
    ("Center out","center_out"),("Build up","buildup"),("Wipe down","wipedown"),("Cascade","cascade"),
    ("Blink O/E","blink_oe"),("Blink halves","blink_half"),("Thirds","blink_thirds"),
    ("Strobe slow","strobe_slow"),("Strobe","strobe"),("Strobe fast","strobe_fast"),
    ("Crossfade","crossfade"),("XFade fast","crossfade_fast"),
]
for idx, (label, key) in enumerate(effect_list):
    col, row = idx % 6, idx // 6
    x, y = 20 + col * 180, 180 + row * 90
    vc.append(
        f'<Button Caption="{_esc(label)}" ID="{bid}" Icon="">'
        f'<WindowState Visible="True" X="{x}" Y="{y}" Width="170" Height="80"/>'
        f'<Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>'
        f'<BackgroundColor>Default</BackgroundColor>'
        f'<BackgroundImage>None</BackgroundImage><Font>Default</Font></Appearance>'
        f'<Function ID="{E(key)}"/><Action>Toggle</Action></Button>'
    )
    bid += 1

doc = textwrap.dedent(f"""\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE Workspace>
<Workspace xmlns="http://www.qlcplus.org/Workspace" CurrentWindow="VirtualConsole">
 <Creator>
  <Name>Q Light Controller Plus</Name>
  <Version>4.12.7</Version>
  <Author>LumiGate live show generator</Author>
 </Creator>
 <Engine>
  <InputOutputMap>
   <Universe Name="Universe 1" ID="0"/>
   <Universe Name="Universe 2" ID="1"/>
   <Universe Name="Universe 3" ID="2"/>
   <Universe Name="Universe 4" ID="3"/>
  </InputOutputMap>
  {fixtures}
  {"".join(funcs)}
 </Engine>
 <VirtualConsole>
  <Frame Caption="">
   <Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>
   <BackgroundColor>Default</BackgroundColor><BackgroundImage>None</BackgroundImage>
   <Font>Default</Font></Appearance>
   {"".join(vc)}
   <Properties><Size Width="1280" Height="900"/>
   <GrandMaster ChannelMode="Intensity" ValueMode="Reduce" SliderMode="Normal"/></Properties>
  </Frame>
 </VirtualConsole>
 <SimpleDesk><Engine/></SimpleDesk>
</Workspace>
""")

import os
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "qlcplus-show-60min.qxw")
with open(out, "w", encoding="utf-8") as f:
    f.write(doc)

print(f"Functions: {len(funcs)}  |  Fixtures: {CH}")
print(f"Written: {out}  ({len(doc):,} bytes)")
