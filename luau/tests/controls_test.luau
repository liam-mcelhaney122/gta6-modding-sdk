--!strict
-- @gta6/controls (generated) and the game.controls helpers against the runner's mock world.
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\controls_test.luau 3000 --press 51@480 --expect-clean --expect-log "PASS controls_test"
-- (The shared --press 51@480 is a raw integer: it is not an action of this build, which the test checks.)

local script = require("@gta6/script")
local native = require("@gta6/native")
local controls = require("@gta6/controls")
local game = require("@gta6/game")

-- top level (no waits)
assert(controls.Control.JUMP == 0xD9D0E1C0, "Control.JUMP is joaat(\"INPUT_JUMP\"), the RDR2 key")
assert(controls.Control.CONTEXT == native.joaat("INPUT_CONTEXT"), "Control member = joaat of its INPUT_ name")
assert(game.Control == controls.Control and game.ControlGroup == controls.ControlGroup, "game re-exports the tables")
assert(controls.ControlGroup.PLAYER_CONTROL == 0 and controls.ControlGroup.FRONTEND_CONTROL == 2, "groups")
assert(#controls.ACTION_HASHES == controls.ACTION_COUNT, "one key per action index")
assert(controls.actionName("input_context") == "INPUT_CONTEXT", "name lookup by string (case-insensitive)")
assert(controls.actionName(controls.Control.CONTEXT) == "INPUT_CONTEXT", "name lookup by key")
assert(controls.actionIndex(controls.ACTION_HASHES[1]) == 0, "index 0 is the first table entry")
assert(controls.actionIndex("INPUT_SPECIAL_ABILITY_SECONDARY") == nil, "a GTA V-only name is not an action")
assert(controls.actionIndex(51) == nil, "a GTA V control index is not an action key")
assert(controls.GTA5_INDEX_TO_CONTROL[51] == controls.Control.CONTEXT, "GTA V index 51 maps to INPUT_CONTEXT")
-- Control's type has no COUNT member, which is what this checks at run time: look the key up through an
-- untyped view (a typed `Control.COUNT` is rejected by the analyzer, correctly).
local untypedControl: any = controls.Control
assert(untypedControl.COUNT == nil, "INPUT_COUNT is the enum sentinel, not a Control member")

script.register("controls_test.check", function()
	local seen = 0
	while game.now() < 600 do
		if game.controls.justPressed(51) then
			seen += 1
		end
		assert(not game.controls.justPressed(controls.Control.CONTEXT), "only the raw 51 is pressed")
		game.wait(0)
	end
	assert(seen == 1, `raw press seen once (saw {seen})`)
	script.log("info", "PASS controls_test")
end)
