--!strict
-- Held presses through game.mod (Luau twin of TestModHotkeys in sdk/tests/python/test_hotkeys.py).
-- Run (build.cmd test and CTest use exactly these flags):
--   g6_luau_runner sdk\luau\lib sdk\luau\tests\scenarios\hotkeys_hold.luau 2000 --press INPUT_CONTEXT@96
--     --press INPUT_CONTEXT@400+800 --press INPUT_SPRINT@1504+300 --press INPUT_CONTEXT@1520+100
--     --press INPUT_JUMP@0+600 --expect-clean --expect-log "PASS hotkeys_hold"

local script = require("@gta6/script")
local game = require("@gta6/game")

local Control = game.Control
local m = game.mod("hk")
local log: { string } = {}
local function note(what: string)
	return function()
		table.insert(log, `{what}@{game.now()}`)
	end
end

m.onKey(Control.CONTEXT, note("press"))
m.onKey(Control.CONTEXT, note("chord"), { mods = Control.SPRINT })
m.onRelease(Control.CONTEXT, note("tap"), { maxMs = 200 })
m.onHold(Control.CONTEXT, 500, note("hold"))

-- a waiting hold handler runs as its own script; repeats while it still runs are skipped
local jumps: { number } = {}
m.onHold(Control.JUMP, 100, function()
	table.insert(jumps, game.now())
	game.wait(250)
end, { repeatMs = 100 })

script.register("hotkeys_hold.check", function()
	while game.now() < 1900 do
		game.wait(0)
	end
	local got = table.concat(log, " ")
	local want = "press@96 tap@112 press@400 hold@912 chord@1520"
	assert(got == want, `got [{got}], want [{want}]`)
	local j = table.concat(jumps, " ")
	assert(j == "112 448", `waiting hold handler ran at [{j}], want [112 448]`)
	m.stop()
	script.log("info", "PASS hotkeys_hold")
end)
