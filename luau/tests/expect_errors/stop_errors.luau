--!strict
-- m.stop() runs every cleanup callback even when one fails, and reports the failure.
-- This test logs one ERROR on purpose, so it is not in the --expect-clean loop over tests\*.luau.
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\expect_errors\stop_errors.luau 500 --expect-log "PASS stop_errors"

local script = require("@gta6/script")
local game = require("@gta6/game")

local m = game.mod("stop_errors")
local a, b = m.toggle(47), m.toggle(52)
local cleaned: { string } = {}
a.onDisable(function()
	error("boom")
end)
b.onDisable(function() table.insert(cleaned, "b") end)
m.onStop(function() table.insert(cleaned, "stop") end)
local ticks = 0
m.every(16, function() ticks += 1 end)

script.register("stop_errors.check", function()
	game.wait(48)
	a.set(true)
	b.set(true)
	local errors = m.stop()
	assert(#errors == 1 and string.find(errors[1], "boom", 1, true) ~= nil, "one failure reported")
	assert(#cleaned == 2 and cleaned[1] == "b" and cleaned[2] == "stop", "later cleanups still ran")
	assert(not a.isOn() and not b.isOn(), "both toggles off")
	local after = ticks
	game.wait(100)
	assert(ticks == after, "every() was cancelled")
	script.log("info", "PASS stop_errors")
end)
