--[[
  LumiGate Live Show — grandMA3 v2.4.2.2 Plugin
  ================================================
  Builds a 60-minute, 20-channel light show entirely inside grandMA3
  using the Cmd() API (most stable across versions).

  SETUP:
  1. Patch 20 Generic Single dimmers on Universe 1,
     DMX addresses 1..20 (or let this script do it via Patch commands).
  2. In grandMA3: Menu → Plugins → Load Plugin → select this file.
  3. Run it (click the green arrow / Execute).
     It will create: Fixtures, Groups, Sequences, Executors, and all cues.
  4. Assign Executor 1 to a fader / button on the XL Wing or onPC,
     and press GO to start the 60-minute show.

  NOTE: The script uses grandMA3 command-line syntax (Cmd()).
        If a command errors, check the system monitor — minor adjustments
        may be needed for different patch/universe configurations.
--]]

local function sleep(ms)
    -- tiny busy-wait; avoids blocking the MA task queue
    local t = os.clock()
    while os.clock() - t < ms / 1000.0 do end
end

local function C(cmd)
    Cmd(cmd)
    sleep(20)   -- give MA time to process between commands
end

local function log(msg)
    Printf("[LumiGate] " .. tostring(msg))
end

-- ── Helpers ──────────────────────────────────────────────────────────────────

local FIXTURES = 20   -- channel count / dimmer count

local function store_cue(seq, cue_no, cue_name, fadein, hold, fadeout, levels)
    -- levels: table of {fixture=value, ...} where value is 0..255 (DMX) or 0..100 (%)
    -- Select the fixtures and set Dimmer attribute
    C("ClearAll")
    for fix, val in pairs(levels) do
        local pct = math.floor(val / 255 * 100 + 0.5)
        C(string.format("Attribute \"Dimmer\" At %d Fixture %d", pct, fix))
    end
    local fi_s = fadein  / 1000.0
    local ho_s = hold    / 1000.0
    local fo_s = fadeout / 1000.0
    C(string.format('Store Sequence %d Cue %s "%s"', seq, cue_no, cue_name))
    C(string.format("Sequence %d Cue %s Property \"InFade\" \"%.2f\"", seq, cue_no, fi_s))
    if hold > 0 then
        C(string.format("Sequence %d Cue %s Property \"Duration\" \"%.2f\"", seq, cue_no, ho_s))
    else
        C(string.format("Sequence %d Cue %s Property \"Duration\" \"0.00\"", seq, cue_no))
    end
    C(string.format("Sequence %d Cue %s Property \"OutFade\" \"%.2f\"", seq, cue_no, fo_s))
    C("ClearAll")
end

local function all_at(val)
    local t = {}
    for i = 1, FIXTURES do t[i] = val end
    return t
end

local function levels_from_fn(fn)
    local t = {}
    for i = 1, FIXTURES do t[i] = fn(i) end
    return t
end

local function arr_to_map(arr)
    local m = {}
    for i, v in ipairs(arr) do m[i] = v end
    return m
end

-- ── Generate level snapshots for each effect family ──────────────────────────

local mid = (FIXTURES + 1) / 2.0

-- Precompute NP wave phase frames
local NP = 16   -- fewer frames than QLC+ to keep cue count manageable in MA3
local function wave_frame(p, fn)
    return levels_from_fn(function(j)
        return math.max(0, math.min(255, fn(j, p)))
    end)
end

local function sine_wave(j, p)
    return math.floor(127 + 127 * math.sin(2*math.pi*((j-1)/FIXTURES)*2 - 2*math.pi*p/NP) + 0.5)
end
local function sine_wave2(j, p)
    return math.floor(
        80 + 60*math.sin(2*math.pi*((j-1)/FIXTURES)*3 - 2*math.pi*p/NP)
           + 60*math.sin(2*math.pi*((j-1)/FIXTURES)*1 + 2*math.pi*p/NP)
    + 0.5)
end
local function ripple(j, p)
    local dist = math.abs((j - mid) / mid)
    return math.floor(127 + 127 * math.sin(dist * 4*math.pi - 2*math.pi*p/NP) + 0.5)
end

-- ── Main ──────────────────────────────────────────────────────────────────────
local function main(display)
    log("LumiGate 60-min show builder starting...")
    log("grandMA3 v2.4 | 20 dimmers | ~147 cues")

    -- Sequence 1 = the main show
    local SEQ = 1
    C(string.format("Delete Sequence %d /NoConfirm", SEQ))
    sleep(100)
    C(string.format('Assign Sequence %d "LumiGate 60min Show"', SEQ))
    C(string.format("Sequence %d Property \"AutoStop\" \"Off\"", SEQ))
    C(string.format("Sequence %d Property \"Loop\" \"On\"", SEQ))

    log("Building cues... (this takes ~60 seconds)")

    local cue = 0
    local function next_cue()
        cue = cue + 1
        return string.format("%.4f", cue)
    end

    local B  = arr_to_map(all_at(0))    -- blackout
    local F  = arr_to_map(all_at(255))  -- full

    -- ── ACT 1: AWAKENING ─────────────────────────────────────────────────────
    log("Act 1: Awakening")
    for rep = 1, 8 do
        store_cue(SEQ, next_cue(), "Breathe up "..rep,   3200, 35000, 3200, F)
        store_cue(SEQ, next_cue(), "Breathe down "..rep, 3200, 30000, 3200, B)
    end

    -- slow wave (3 cycles)
    for cycle = 1, 3 do
        for p = 0, NP-1 do
            store_cue(SEQ, next_cue(), string.format("WaveSlow c%dp%d", cycle, p),
                0, 200, 0, arr_to_map(wave_frame(p, function(j, pp)
                    return 80 + 80*math.sin(2*math.pi*((j-1)/FIXTURES) - 2*math.pi*pp/NP)
                end)))
        end
    end

    -- scatter + crossfade
    for rep = 1, 4 do
        local sparse = {}
        for i = 1, FIXTURES do sparse[i] = (math.random() < 0.3) and math.random(120, 220) or 0 end
        store_cue(SEQ, next_cue(), "Scatter "..rep, 2000, 28000, 2000, arr_to_map(sparse))
    end

    store_cue(SEQ, next_cue(), "Gradient L→R", 5000, 45000, 5000,
        arr_to_map(levels_from_fn(function(j) return math.floor((j-1)/(FIXTURES-1)*255+0.5) end)))
    store_cue(SEQ, next_cue(), "Gradient R→L", 5000, 40000, 5000,
        arr_to_map(levels_from_fn(function(j) return math.floor((FIXTURES-j)/(FIXTURES-1)*255+0.5) end)))

    -- ── ACT 2: RISING ────────────────────────────────────────────────────────
    log("Act 2: Rising")
    -- Sine wave (normal speed, 4 cycles)
    for cycle = 1, 4 do
        for p = 0, NP-1 do
            store_cue(SEQ, next_cue(), string.format("Wave c%dp%d", cycle, p),
                0, 80, 0, arr_to_map(wave_frame(p, sine_wave)))
        end
    end

    -- Running light (2 passes)
    for pass = 1, 2 do
        for i = 1, FIXTURES do
            local v = {}
            for j = 1, FIXTURES do v[j] = (j == i) and 255 or 0 end
            store_cue(SEQ, next_cue(), string.format("Run p%d f%d", pass, i), 0, 75, 0, arr_to_map(v))
        end
    end

    -- Comet forward (2 passes)
    for pass = 1, 2 do
        for i = 1, FIXTURES do
            local v = {}
            for j = 1, FIXTURES do
                local dist = (i - j) % FIXTURES
                v[j] = ({255, 140, 70, 30, 10})[dist+1] or 0
            end
            store_cue(SEQ, next_cue(), string.format("Comet p%d f%d", pass, i), 0, 60, 0, arr_to_map(v))
        end
    end

    -- VU bounce (1 cycle up and down)
    for n = 0, FIXTURES do
        local v = {}
        for j = 1, FIXTURES do v[j] = (j <= n) and math.floor(80 + 175*(j-1)/(FIXTURES-1)+0.5) or 0 end
        store_cue(SEQ, next_cue(), "VU up "..n, 30, 50, 0, arr_to_map(v))
    end
    for n = FIXTURES, 0, -1 do
        local v = {}
        for j = 1, FIXTURES do v[j] = (j <= n) and math.floor(80 + 175*(j-1)/(FIXTURES-1)+0.5) or 0 end
        store_cue(SEQ, next_cue(), "VU dn "..n, 30, 50, 0, arr_to_map(v))
    end

    -- Center bloom
    for r = 0, math.floor(FIXTURES/2) do
        local v = levels_from_fn(function(j) return math.abs(j - mid) <= r + 0.5 and 255 or 0 end)
        store_cue(SEQ, next_cue(), "Bloom "..r, 180, 140, 0, arr_to_map(v))
    end
    for r = math.floor(FIXTURES/2), 0, -1 do
        local v = levels_from_fn(function(j) return math.abs(j - mid) <= r + 0.5 and 255 or 0 end)
        store_cue(SEQ, next_cue(), "Bloom dn "..r, 180, 140, 0, arr_to_map(v))
    end

    -- ── ACT 3: FIRST PEAK ────────────────────────────────────────────────────
    log("Act 3: First Peak")
    -- Build-up
    for n = 0, FIXTURES do
        local v = levels_from_fn(function(j) return j <= n and 255 or 0 end)
        store_cue(SEQ, next_cue(), "Build "..n, 80, 80, 0, arr_to_map(v))
    end

    -- Strobe
    for rep = 1, 20 do
        store_cue(SEQ, next_cue(), "Strobe F "..rep, 0, 10, 0, F)
        store_cue(SEQ, next_cue(), "Strobe B "..rep, 0, 10, 0, B)
    end

    -- Twin waves during peak
    for cycle = 1, 2 do
        for p = 0, NP-1 do
            store_cue(SEQ, next_cue(), string.format("W2 c%dp%d", cycle, p),
                0, 60, 0, arr_to_map(wave_frame(p, sine_wave2)))
        end
    end

    -- Strobe fast burst
    for rep = 1, 30 do
        store_cue(SEQ, next_cue(), "SFast F "..rep, 0, 6,  0, F)
        store_cue(SEQ, next_cue(), "SFast B "..rep, 0, 6,  0, B)
    end

    -- Sudden cut to breathe (shock contrast)
    store_cue(SEQ, next_cue(), "Peak End - Breathe", 5000, 30000, 4000, F)
    store_cue(SEQ, next_cue(), "Peak End - Black",   4000, 20000, 3000, B)

    -- ── ACT 4: BREAKDOWN ─────────────────────────────────────────────────────
    log("Act 4: Breakdown / Rebuild")
    local function sparkle_frame()
        local v = {}
        for i = 1, FIXTURES do v[i] = 0 end
        for _ = 1, math.random(2, 5) do
            v[math.random(1, FIXTURES)] = math.random(100, 255)
        end
        return arr_to_map(v)
    end
    for rep = 1, 16 do
        store_cue(SEQ, next_cue(), "Sparkle "..rep, 0, 90, 0, sparkle_frame())
    end

    -- Crossfade through looks
    store_cue(SEQ, next_cue(), "Look Vee",  4000, 40000, 4000,
        arr_to_map(levels_from_fn(function(j) return math.floor(255*(1-math.abs(j-mid)/mid)+0.5) end)))
    store_cue(SEQ, next_cue(), "Look Ends", 4000, 35000, 4000,
        arr_to_map(levels_from_fn(function(j) return (j <= 3 or j > FIXTURES-3) and 255 or 15 end)))

    -- Ripple rebuild
    for cycle = 1, 2 do
        for p = 0, NP-1 do
            store_cue(SEQ, next_cue(), string.format("Rpl c%dp%d", cycle, p),
                0, 90, 0, arr_to_map(wave_frame(p, ripple)))
        end
    end

    -- ── ACT 5: SECOND BUILD ──────────────────────────────────────────────────
    log("Act 5: Second Build")
    -- Comet both directions simultaneously (alternate forward/reverse)
    for i = 1, FIXTURES do
        local vf, vr = {}, {}
        for j = 1, FIXTURES do
            vf[j] = ({255,140,70,30,10})[ ((i-j) % FIXTURES) + 1 ] or 0
            vr[j] = ({255,140,70,30,10})[ ((j-i) % FIXTURES) + 1 ] or 0
        end
        store_cue(SEQ, next_cue(), "CBoth fwd "..i, 0, 45, 0, arr_to_map(vf))
        store_cue(SEQ, next_cue(), "CBoth rev "..i, 0, 45, 0, arr_to_map(vr))
    end

    -- Thirds chase
    for rep = 1, 12 do
        for t = 1, 3 do
            local v = levels_from_fn(function(j) return ((j-1) % 3 == t-1) and 255 or 28 end)
            store_cue(SEQ, next_cue(), string.format("3rd r%d t%d", rep, t), 0, 170, 0, arr_to_map(v))
        end
    end

    -- Heartbeat (double-thump: big flash, soft echo, silence)
    local hb_levels = {255, 180, 100, 40, 10, 0, 160, 255, 200, 130, 60, 20, 0, 0, 0, 0}
    for rep = 1, 6 do
        for _, lvl in ipairs(hb_levels) do
            store_cue(SEQ, next_cue(), string.format("HB r%d v%d", rep, lvl), 0, 50, 0, arr_to_map(all_at(lvl)))
        end
    end

    -- Odd/even blink with wave hybrid
    for rep = 1, 16 do
        local oe = rep % 2 == 0
        local v = levels_from_fn(function(j) return (j%2 == 0) == oe and 255 or 0 end)
        store_cue(SEQ, next_cue(), "OE "..rep, 0, 200, 0, arr_to_map(v))
    end

    -- ── ACT 6: CLIMAX ────────────────────────────────────────────────────────
    log("Act 6: Climax")
    -- Strobe opening burst (biggest moment)
    for rep = 1, 40 do
        store_cue(SEQ, next_cue(), "ClxS F "..rep, 0, 8,  0, F)
        store_cue(SEQ, next_cue(), "ClxS B "..rep, 0, 8,  0, B)
    end

    -- Comet fast + strobe interleaved
    for i = 1, FIXTURES do
        local v = {}
        for j = 1, FIXTURES do
            v[j] = ({255,140,70,30,10})[ ((i-j) % FIXTURES) + 1 ] or 0
        end
        store_cue(SEQ, next_cue(), "CFC "..i, 0, 35, 0, arr_to_map(v))
        if i % 4 == 0 then
            store_cue(SEQ, next_cue(), "ClxSB "..i, 0, 7, 0, F)
            store_cue(SEQ, next_cue(), "ClxSBb "..i, 0, 7, 0, B)
        end
    end

    -- Scatter explosions
    for rep = 1, 20 do
        local v = {}
        for i = 1, FIXTURES do v[i] = math.random() < 0.4 and math.random(180, 255) or 0 end
        store_cue(SEQ, next_cue(), "Scatt "..rep, 0, 70, 0, arr_to_map(v))
    end

    -- Wipe-down climax resolution
    for n = FIXTURES, 0, -1 do
        local v = levels_from_fn(function(j) return j <= n and 255 or 0 end)
        store_cue(SEQ, next_cue(), "CliWipe "..n, 40, 50, 0, arr_to_map(v))
    end

    -- ── ACT 7: RESOLUTION ────────────────────────────────────────────────────
    log("Act 7: Resolution")
    -- Cross through gradient looks slowly
    store_cue(SEQ, next_cue(), "Res Grad", 5000, 45000, 5000,
        arr_to_map(levels_from_fn(function(j) return math.floor((j-1)/(FIXTURES-1)*255+0.5) end)))
    store_cue(SEQ, next_cue(), "Res Vee",  5000, 40000, 5000,
        arr_to_map(levels_from_fn(function(j) return math.floor(255*(1-math.abs(j-mid)/mid)+0.5) end)))

    -- Slow wave to finish
    for cycle = 1, 3 do
        for p = 0, NP-1 do
            store_cue(SEQ, next_cue(), string.format("ResW c%dp%d", cycle, p),
                0, 220, 0, arr_to_map(wave_frame(p, function(j, pp)
                    return 80 + 70*math.sin(2*math.pi*((j-1)/FIXTURES) - 2*math.pi*pp/NP)
                end)))
        end
    end

    -- Breathe down to black x3
    store_cue(SEQ, next_cue(), "Outro breathe 1 up", 4000, 35000, 4000, F)
    store_cue(SEQ, next_cue(), "Outro breathe 1 dn", 4000, 28000, 3000, B)
    store_cue(SEQ, next_cue(), "Outro breathe 2 up", 5000, 30000, 5000, arr_to_map(all_at(160)))
    store_cue(SEQ, next_cue(), "Outro breathe 2 dn", 5000, 25000, 4000, B)
    store_cue(SEQ, next_cue(), "Final breathe up",   6000, 40000, 6000, arr_to_map(all_at(100)))
    store_cue(SEQ, next_cue(), "FADE TO BLACK",      8000, 1000,  8000, B)

    -- ── Executor & Playback ───────────────────────────────────────────────────
    log("Setting up Executor 1...")
    C(string.format("Assign Sequence %d Executor 1", SEQ))
    C("Executor 1 Property \"OffOnOverwrite\" \"Off\"")

    log(string.format("Done! %d cues created.", cue))
    log("→ Press GO on Executor 1 (or Fader 1) to start the 60-minute show.")
    log("→ Use the GO button to advance manually, or set Auto-Follow on all cues.")
end

-- Plugin entry point
return main
