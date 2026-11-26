--!strict
-- @gta6/hotkeys: the state machine on fake input (same cases as sdk/tests/python/test_hotkeys.py), then its
-- wiring into game.mod against the runner's mock world.
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\hotkeys_test.luau 3000 --press 51@480 --expect-clean --expect-log "PASS hotkeys_test"
-- Held presses (+HOLD) are covered by the hotkeys scenario in build.cmd / CMakeLists.txt (hotkeys_hold.luau).

local script = require("@gta6/script")
local native = require("@gta6/native")
local hotkeys = require("@gta6/hotkeys")
local game = require("@gta6/game")

local K, S, C = 0x1000, 0x2000, 0x3000
local FRAME = 16

-- frames: list of lists of actions held on each tick (pad 0 unless given). Returns "time:name" strings.
local function run(hk: hotkeys.Hotkeys, frames: { { number } }, pad: number?): { string }
	local p = pad or 0
	local prev: { [number]: boolean } = {}
	local held: { [number]: boolean } = {}
	local function read(query: hotkeys.Query, action: number, qpad: number): boolean
		if qpad ~= p then
			return false
		end
		if query == "pressed" then
			return held[action] == true
		elseif query == "justPressed" then
			return held[action] == true and not prev[action]
		end
		return prev[action] == true and not held[action]
	end
	local out: { string } = {}
	for i, down in frames do
		prev = held
		held = {}
		for _, a in down do
			held[a] = true
		end
		for _, b in hk.poll(read, (i - 1) * FRAME) do
			table.insert(out, `{(i - 1) * FRAME}:{b.fn}`)
		end
	end
	return out
end

local function same(got: { string }, want: { string }, what: string)
	local g, w = table.concat(got, " "), table.concat(want, " ")
	assert(g == w, `{what}: got [{g}], want [{w}]`)
end

local function rep(n: number, v: { number }): { { number } }
	local t = {}
	for i = 1, n do
		t[i] = v
	end
	return t
end
local function cat(...: { { number } }): { { number } }
	local out = {}
	for _, part in { ... } do
		for _, f in part do
			table.insert(out, f)
		end
	end
	return out
end

-- press, release and tap
do
	local hk = hotkeys.new()
	hk.add("press", K, "press")
	hk.add("release", K, "release")
	hk.add("release", K, "tap", nil, nil, 100)
	same(run(hk, { {}, { K }, { K }, {} }), { "16:press", "48:release", "48:tap" }, "short press")
	same(run(hk, cat({ {}, { K } }, rep(10, { K }), { {} })), { "16:press", "192:release" }, "long press")
end

-- hold once and with repeat
do
	local hk = hotkeys.new()
	hk.add("hold", K, "hold", nil, nil, 100)
	hk.add("hold", K, "repeat", nil, nil, 48, 32)
	same(run(hk, cat({ {}, { K } }, rep(9, { K }), { {}, { K }, {} })),
		{ "64:repeat", "96:repeat", "128:hold", "128:repeat", "160:repeat" }, "hold")
end

-- chords: most specific wins and consumes the press
do
	local hk = hotkeys.new()
	hk.add("press", K, "plain")
	hk.add("press", K, "sprint", nil, S)
	hk.add("press", K, "sprint+crouch", nil, { S, C })
	hk.add("release", K, "tap", nil, nil, 500)
	hk.add("hold", K, "hold", nil, nil, 32)
	same(run(hk, { {}, { K }, {} }), { "16:plain", "32:tap" }, "plain")
	same(run(hk, { { S }, { S, K }, { S, K }, { S, K }, { S } }), { "16:sprint" }, "chord")
	same(run(hk, { { S, C }, { S, C, K }, {} }), { "16:sprint+crouch" }, "two modifiers")
	same(run(hk, { {}, { K }, { K, S }, { K, S }, { S } }), { "16:plain", "48:hold", "64:tap" }, "modifier after key")
end

-- busy bindings, pads, keys and validation
do
	local hk = hotkeys.new()
	local a = hk.add("press", K, "a")
	local b = hk.add("press", K, "b", nil, S)
	a.busy = true
	same(run(hk, { {}, { K } }), {}, "busy")
	b.busy = true
	same(run(hk, { { S }, { S, K } }), {}, "busy chord still outranks")
	local hp = hotkeys.new()
	hp.add("press", K, "pad2", 2)
	same(run(hp, { {}, { K } }, 0), {}, "other pad")
	same(run(hp, { {}, { K } }, 2), { "16:pad2" }, "pad 2")

	assert(hotkeys.actionKey("INPUT_CONTEXT") == game.Control.CONTEXT, "name -> key")
	assert(hotkeys.actionKey(-1) == 0xFFFFFFFF, "negative -> u32")
	local m = hotkeys.modifierKeys({ "INPUT_SPRINT", game.Control.SPRINT })
	assert(#m == 1 and m[1] == native.joaat("INPUT_SPRINT"), "modifiers are distinct keys")
	local hv = hotkeys.new()
	assert(not pcall(hv.add, "hold", K, "x"), "hold needs ms")
	assert(not pcall(hv.add, "press", K, "x", nil, K), "own modifier")
	assert(not pcall(hv.add, "hold", K, "x", nil, nil, 10, 0), "repeatMs > 0")
	assert(not pcall(hv.add, "tap" :: any, K, "x"), "unknown kind")
	assert(#hv.bindings == 0, "rejected bindings are not added")
	assert(not pcall(hotkeys.block, {}), "empty block")
	assert(hotkeys.block(K).active(), "block without when is always on")
	assert(not hotkeys.block({ K }, 0, function() return false end).active(), "block when=false")
end

-- ------------------------------------------------------------------ wiring (--press 51@480, one tick)
local m = game.mod("hk")
local log: { string } = {}
local function note(what: string)
	return function()
		table.insert(log, `{what}@{game.now()}`)
	end
end
m.onKey(51, note("press"))
m.onKey(51, note("chord"), { mods = game.Control.SPRINT }) -- SPRINT is never held here
m.onRelease(51, note("tap"), { maxMs = 100 })
m.onHold(51, 0, note("hold0"))
m.onHold(51, 200, note("hold200")) -- the press lasts one tick
local menu = m.toggle(game.Control.CONTEXT)
m.block({ game.Control.ATTACK, "INPUT_AIM" }, { when = menu })
m.block(game.Control.JUMP)

local disabled: { [number]: number } = {}
local controlsAny = game.controls :: any
local realDisable = controlsAny.disable
controlsAny.disable = function(a: number, pad: number?)
	disabled[a] = (disabled[a] or 0) + 1
	realDisable(a, pad)
end

script.register("hotkeys_test.check", function()
	while game.now() < 800 do
		game.wait(0)
	end
	table.sort(log)
	same(log, { "hold0@480", "press@480", "tap@496" }, "wiring")
	assert((disabled[game.Control.JUMP] or 0) >= 40, "an always-on block disables every tick")
	assert(disabled[game.Control.ATTACK] == nil and disabled[game.Control.AIM] == nil, "toggle off: not blocked")
	menu.set(true)
	local before = disabled[game.Control.JUMP]
	game.wait(160)
	local n = disabled[game.Control.ATTACK] or 0
	assert(n >= 9 and n <= 11 and disabled[game.Control.AIM] == n, `toggle on: blocked every tick ({n})`)
	assert(disabled[game.Control.JUMP] - before == n, "both blocks run on the same ticks")
	m.stop()
	local stopped = disabled[game.Control.JUMP]
	game.wait(160)
	assert(disabled[game.Control.JUMP] == stopped, "no disables after stop")
	controlsAny.disable = realDisable
	script.log("info", "PASS hotkeys_test")
end)
