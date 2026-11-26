--!strict
-- Script String ('S') and text ('T') arguments and ui.objective on the mock world (G-120; Luau twin of
-- sdk/tests/python/test_script_objects.py). The mock world logs what its handlers read from the objects.
-- Run (build.cmd test and CTest use exactly these flags):
--   g6_luau_runner sdk\luau\lib sdk\luau\tests\scenarios\objective.luau 1500 --expect-clean
--     --expect-log "busyspinner: Saving..." --expect-log "objective shown: Lose the cops (500 ms)"
--     --expect-log "objective shown: Second (-1 ms)" --expect-log "objective cleared" --expect-log "PASS objective"

local script = require("@gta6/script")
local ui = require("@gta6/ui")
local N = require("@gta6/natives")

script.register("objective", function()
	-- 'S': a script String built from the text (the mock world logs the text it read at +0x28)
	N.HUD._BUSYSPINNER_SET_TEXT("Saving...")
	-- 'T' through the generated binding, then ui.objective's SET -> PRINT -> CLEAR after 500 ms
	N.UILOG._UILOG_SET_CACHED_OBJECTIVE("unused")
	ui.objective("Lose the cops", 500)
	script.wait(100)
	local t0 = script.now()
	script.wait(600) -- the clear fires on the first tick at or after 500 ms
	assert(script.now() - t0 >= 600, "waited")
	-- no timeout: shown, never cleared by the helper; ui.clearObjective clears it
	ui.objective("Second", -1)
	script.wait(200)
	ui.clearObjective()
	-- a non-string argument is a Luau error, not a native call
	local ok = pcall(function()
		(N.UILOG._UILOG_SET_CACHED_OBJECTIVE :: any)({})
	end)
	assert(not ok, "a table is refused")
	script.log("info", "PASS objective")
end)
