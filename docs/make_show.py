#!/usr/bin/env python3
"""
Generate a fancy ~30-minute QLC+ light show (.qxw) for LumiGate.

20 generic dimmers on Universe 1 (DMX channels 1..20). Produces a rich library
of building-block scenes and many effect chasers (smooth sine waves, comets with
fading tails, sparkle/twinkle, VU bounce, symmetric center-out, crossfades
between looks, strobes, blinks, build-ups, wipes, runs), then sequences them into
a dynamic 30-minute master "GREAT SHOW" arc (intro -> build -> peak -> groove ->
outro). A Virtual Console gives one-click access.

Run:  python docs/make_show.py   ->  writes docs/qlcplus-show.qxw
"""
import math, random

CH = 20
random.seed(7)

_fid = 0
funcs = []      # XML strings, in ID order
effects = []    # (id, label, default_seconds) building blocks for the master arc


def alloc():
    global _fid
    i = _fid
    _fid += 1
    return i


def clamp(v):
    return max(0, min(255, int(round(v))))


def scene(name, levels):
    """levels: iterable of 20 channel values."""
    i = alloc()
    fv = "".join(f'<FixtureVal ID="{k}">0,{clamp(levels[k])}</FixtureVal>' for k in range(CH))
    funcs.append(f'<Function ID="{i}" Type="Scene" Name="{_esc(name)}">'
                 f'<Speed FadeIn="0" FadeOut="0" Duration="0"/>{fv}</Function>')
    return i


def chaser(name, steps, fadein, hold, fadeout, runorder="Loop", direction="Forward"):
    i = alloc()
    sx = "".join(f'<Step Number="{n}">{sid}</Step>' for n, sid in enumerate(steps))
    funcs.append(f'<Function ID="{i}" Type="Chaser" Name="{_esc(name)}">'
                 f'<Speed FadeIn="{fadein}" FadeOut="{fadeout}" Duration="{hold}"/>'
                 f'<Direction>{direction}</Direction><RunOrder>{runorder}</RunOrder>'
                 f'<SpeedModes FadeIn="Common" FadeOut="Common" Duration="Common"/>'
                 f'{sx}</Function>')
    return i


def _esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ── Building-block scenes ────────────────────────────────────────────────────
BLACK = scene("Blackout", [0] * CH)
FULL  = scene("Full",     [255] * CH)

# one-hot solos and comets (a head with a fading tail), forward set
solo   = [scene(f"Solo {k+1}",  [255 if j == k else 0 for j in range(CH)]) for k in range(CH)]
comet  = []
for k in range(CH):
    lv = [0] * CH
    for t, g in enumerate((255, 150, 80, 35, 12)):   # head + 4-px tail
        lv[(k - t) % CH] = g
    comet.append(scene(f"Comet {k+1}", lv))

# symmetric center-out steps (mirror pairs light up from the middle)
center = []
mid = (CH - 1) / 2.0
for r in range(CH // 2 + 1):
    lv = [0] * CH
    for j in range(CH):
        if abs(j - mid) <= r + 0.5:
            lv[j] = 255
    center.append(scene(f"CenterOut {r}", lv))

# build-up: cumulative fill left->right
buildup = []
for n in range(CH + 1):
    buildup.append(scene(f"Build {n}", [255 if j < n else 0 for j in range(CH)]))

# moving sine wave (smooth flow): NP phase frames
NP = 36
wave = []
for p in range(NP):
    ph = 2 * math.pi * p / NP
    lv = [127 + 127 * math.sin(2 * math.pi * (j / CH) * 2 - ph) for j in range(CH)]
    wave.append(scene(f"Wave {p}", lv))

# twin counter-rotating waves
wave2 = []
for p in range(NP):
    ph = 2 * math.pi * p / NP
    lv = [110 + 70 * math.sin(2 * math.pi * (j / CH) * 3 - ph)
              + 70 * math.sin(2 * math.pi * (j / CH) * 1 + ph) for j in range(CH)]
    wave2.append(scene(f"Wave2 {p}", lv))

# sparkle / twinkle frames (random few channels lit)
sparkle = []
for p in range(24):
    lv = [0] * CH
    for _ in range(5):
        lv[random.randrange(CH)] = random.choice((255, 200, 160))
    sparkle.append(scene(f"Sparkle {p}", lv))

# VU meter bounce: a bar that rises and falls
vu = []
for n in list(range(CH + 1)) + list(range(CH - 1, 0, -1)):
    lv = [0] * CH
    for j in range(n):
        lv[j] = clamp(80 + 175 * j / max(1, CH - 1))   # green->hot gradient feel
    vu.append(scene(f"VU {n}", lv))

# look scenes for slow crossfades
looks = [
    scene("Look Gradient",   [255 * j / (CH - 1) for j in range(CH)]),
    scene("Look GradientRev",[255 * (CH - 1 - j) / (CH - 1) for j in range(CH)]),
    scene("Look Vee",        [255 * (1 - abs(j - mid) / mid) for j in range(CH)]),
    scene("Look Peaks",      [255 if j % 4 == 0 else 30 for j in range(CH)]),
    scene("Look Alt",        [255 if j % 2 == 0 else 50 for j in range(CH)]),
    scene("Look Ends",       [255 if (j < 3 or j >= CH - 3) else 20 for j in range(CH)]),
]

odd  = scene("Odd",  [255 if j % 2 == 0 else 0 for j in range(CH)])
even = scene("Even", [0 if j % 2 == 0 else 255 for j in range(CH)])
half_l = scene("Half L", [255 if j < CH // 2 else 0 for j in range(CH)])
half_r = scene("Half R", [0 if j < CH // 2 else 255 for j in range(CH)])

# ── Effect chasers (the building blocks the master arc sequences) ─────────────
def add_effect(label, cid, seconds):
    effects.append((cid, label, seconds))
    return cid

add_effect("Breathe slow",  chaser("Breathe slow", [FULL, BLACK], 2600, 600, 2600), 22)
add_effect("Crossfade looks", chaser("Crossfade looks", looks, 3200, 1200, 3200), 40)
add_effect("Wave flow",     chaser("Wave flow", wave, 90, 70, 0), 26)
add_effect("Twin waves",    chaser("Twin waves", wave2, 80, 60, 0), 26)
add_effect("Center bloom",  chaser("Center bloom", center + center[::-1], 220, 160, 0), 18)
add_effect("Build up",      chaser("Build up", buildup, 130, 110, 0), 16)
add_effect("Wipe down",     chaser("Wipe down", buildup[::-1], 90, 80, 0), 12)
add_effect("Run forward",   chaser("Run forward", solo, 60, 80, 0), 14)
add_effect("Run bounce",    chaser("Run bounce", solo, 50, 70, 0, runorder="PingPong"), 14)
add_effect("Comet",         chaser("Comet", comet, 40, 70, 0), 16)
add_effect("Comet fast",    chaser("Comet fast", comet, 0, 45, 0), 14)
add_effect("Sparkle",       chaser("Sparkle", sparkle, 0, 90, 0), 16)
add_effect("VU bounce",     chaser("VU bounce", vu, 30, 55, 0), 16)
add_effect("Blink O/E",     chaser("Blink odd/even", [odd, even], 0, 230, 0), 12)
add_effect("Halves",        chaser("Halves L/R", [half_l, half_r], 260, 120, 260), 14)
add_effect("Strobe",        chaser("Strobe", [FULL, BLACK], 0, 55, 0), 8)
add_effect("Strobe ends",   chaser("Strobe ends", [looks[5], BLACK], 0, 70, 0), 8)

E = {label: cid for cid, label, _ in effects}

# ── Master 30-minute arc ─────────────────────────────────────────────────────
# A list of (effect label, seconds). Designed to flow intro->build->peak->groove
# ->outro. Total is padded/trimmed to ~1800 s (30 min).
arc = [
    # Intro — slow & atmospheric
    ("Breathe slow", 45), ("Crossfade looks", 60), ("Wave flow", 40),
    ("Center bloom", 30), ("Twin waves", 40), ("Crossfade looks", 50),
    # Build — movement grows
    ("Build up", 28), ("Run forward", 26), ("Wave flow", 30), ("Comet", 28),
    ("VU bounce", 28), ("Run bounce", 26), ("Twin waves", 30), ("Halves", 22),
    ("Wipe down", 20), ("Center bloom", 24),
    # Peak — high energy
    ("Blink O/E", 18), ("Comet fast", 22), ("Strobe", 10), ("Run forward", 18),
    ("Sparkle", 22), ("Strobe ends", 10), ("VU bounce", 22), ("Comet fast", 20),
    ("Blink O/E", 16), ("Wave flow", 22), ("Strobe", 8), ("Run bounce", 20),
    ("Sparkle", 22), ("Comet", 20), ("Strobe", 8), ("Twin waves", 24),
    # Groove — mid tempo, varied
    ("Crossfade looks", 40), ("Center bloom", 26), ("Build up", 24),
    ("Halves", 22), ("Wave flow", 30), ("VU bounce", 24), ("Run forward", 22),
    ("Comet", 22), ("Twin waves", 30), ("Sparkle", 20),
    # Second peak
    ("Strobe", 10), ("Comet fast", 22), ("Blink O/E", 16), ("Sparkle", 22),
    ("Run bounce", 20), ("Strobe ends", 10), ("VU bounce", 22), ("Wave flow", 24),
    # Outro — wind down
    ("Crossfade looks", 55), ("Center bloom", 30), ("Twin waves", 45),
    ("Wave flow", 40), ("Breathe slow", 70),
]

# pad to ~1800 s by repeating the groove/peak section
target = 1800
def total(seq):
    return sum(s for _, s in seq)
groovepeak = [("Crossfade looks", 40), ("Comet", 22), ("Sparkle", 22),
              ("Wave flow", 28), ("Run bounce", 22), ("VU bounce", 24),
              ("Twin waves", 28), ("Strobe", 9), ("Blink O/E", 16),
              ("Center bloom", 26)]
gi = 0
while total(arc) < target - 20:
    arc.insert(len(arc) - 5, groovepeak[gi % len(groovepeak)])
    gi += 1

master_id = alloc()
msteps = "".join(
    f'<Step Number="{n}" FadeIn="0" Hold="{sec*1000}" FadeOut="0">{E[label]}</Step>'
    for n, (label, sec) in enumerate(arc))
funcs.append(
    f'<Function ID="{master_id}" Type="Chaser" Name="GREAT SHOW (30 min)">'
    f'<Speed FadeIn="0" FadeOut="0" Duration="0"/>'
    f'<Direction>Forward</Direction><RunOrder>Loop</RunOrder>'
    f'<SpeedModes FadeIn="PerStep" FadeOut="PerStep" Duration="PerStep"/>'
    f'{msteps}</Function>')

# ── Assemble workspace ───────────────────────────────────────────────────────
fixtures = "".join(
    f'<Fixture><Manufacturer>Generic</Manufacturer><Model>Generic</Model>'
    f'<Mode>Dimmer</Mode><ID>{k}</ID><Name>Dimmer {k+1}</Name>'
    f'<Universe>0</Universe><Address>{k}</Address><Channels>1</Channels></Fixture>'
    for k in range(CH))

# Virtual console: GO master + a grid of effect buttons
vc_buttons = []
vc_buttons.append(
    '<Button Caption="GREAT SHOW" ID="0" Icon="">'
    '<WindowState Visible="True" X="20" Y="20" Width="200" Height="120"/>'
    '<Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>'
    '<BackgroundColor>4287317</BackgroundColor><BackgroundImage>None</BackgroundImage>'
    '<Font>Default</Font></Appearance>'
    f'<Function ID="{master_id}"/><Action>Toggle</Action></Button>')
vc_buttons.append(
    '<Button Caption="BLACKOUT" ID="1" Icon="">'
    '<WindowState Visible="True" X="240" Y="20" Width="160" Height="120"/>'
    '<Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>'
    '<BackgroundColor>6684672</BackgroundColor><BackgroundImage>None</BackgroundImage>'
    '<Font>Default</Font></Appearance>'
    f'<Function ID="{BLACK}"/><Action>Toggle</Action></Button>')
bid = 2
x0, y0 = 20, 160
for idx, (cid, label, _s) in enumerate(effects):
    col = idx % 6
    row = idx // 6
    x = x0 + col * 175
    y = y0 + row * 90
    vc_buttons.append(
        f'<Button Caption="{_esc(label)}" ID="{bid}" Icon="">'
        f'<WindowState Visible="True" X="{x}" Y="{y}" Width="165" Height="80"/>'
        f'<Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>'
        f'<BackgroundColor>Default</BackgroundColor><BackgroundImage>None</BackgroundImage>'
        f'<Font>Default</Font></Appearance>'
        f'<Function ID="{cid}"/><Action>Toggle</Action></Button>')
    bid += 1

doc = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE Workspace>
<Workspace xmlns="http://www.qlcplus.org/Workspace" CurrentWindow="VirtualConsole">
 <Creator>
  <Name>Q Light Controller Plus</Name>
  <Version>4.12.7</Version>
  <Author>LumiGate show generator</Author>
 </Creator>
 <Engine>
  <InputOutputMap>
   <Universe Name="Universe 1" ID="0"/>
   <Universe Name="Universe 2" ID="1"/>
   <Universe Name="Universe 3" ID="2"/>
   <Universe Name="Universe 4" ID="3"/>
  </InputOutputMap>
  {fixtures}
  {''.join(funcs)}
 </Engine>
 <VirtualConsole>
  <Frame Caption="">
   <Appearance><FrameStyle>None</FrameStyle><ForegroundColor>Default</ForegroundColor>
   <BackgroundColor>Default</BackgroundColor><BackgroundImage>None</BackgroundImage><Font>Default</Font></Appearance>
   {''.join(vc_buttons)}
   <Properties><Size Width="1280" Height="800"/>
   <GrandMaster ChannelMode="Intensity" ValueMode="Reduce" SliderMode="Normal"/></Properties>
  </Frame>
 </VirtualConsole>
 <SimpleDesk><Engine/></SimpleDesk>
</Workspace>
'''

import os
out = os.path.join(os.path.dirname(__file__), "qlcplus-show.qxw")
with open(out, "w", encoding="utf-8") as f:
    f.write(doc)

print(f"scenes+chasers: {len(funcs)} functions, {CH} fixtures")
print(f"arc steps: {len(arc)}, total runtime: {total(arc)//60}m {total(arc)%60}s")
print(f"wrote {out} ({len(doc)} bytes)")
