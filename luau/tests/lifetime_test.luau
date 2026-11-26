--!strict
-- Entity lifetime helpers over the in/out Entity* code 'X' (G-084): Entity:delete() and Entity:release().
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\lifetime_test.luau 3000 --press 51@480 --expect-clean --expect-log "PASS lifetime_test"
-- Mock semantics (sdk/docs/script_layer.md, "Mock world"): DELETE_* removes a script-created entity and clears
-- the handle; *_AS_NO_LONGER_NEEDED clears the handle of a script-created entity and leaves it in the world.

local script = require("@gta6/script")
local game = require("@gta6/game")

local function copy(h: buffer): buffer
	local c = buffer.create(16)
	buffer.copy(c, 0, h, 0, 16)
	return c
end

script.register("lifetime_test.check", function()
	local ped = game.player.ped()

	-- release: the handle is cleared, the vehicle stays in the world
	local a = game.Vehicle.spawn("lifetime_a", ped:offset(0, 5, 0), 0)
	assert(a ~= nil and a:exists(), "vehicle a spawned")
	local aKeep = game.Vehicle.new(copy(a.handle))
	a:release()
	assert(not a:isValid(), "release cleared the handle")
	assert(aKeep:exists(), "a released vehicle stays in the world")

	-- delete: the handle is cleared and the vehicle is gone
	local b = game.Vehicle.spawn("lifetime_b", ped:offset(0, 10, 0), 0)
	assert(b ~= nil and b:exists(), "vehicle b spawned")
	local bKeep = game.Vehicle.new(copy(b.handle))
	b:delete()
	assert(not b:isValid(), "delete cleared the handle")
	assert(not bKeep:exists(), "the deleted vehicle is gone")

	-- a released entity is no longer this script's: delete clears the handle but leaves it
	local aAgain = game.Vehicle.new(copy(aKeep.handle))
	aAgain:delete()
	assert(not aAgain:isValid() and aKeep:exists(), "delete of a released vehicle only clears the handle")

	-- the player ped was not created by the script: release keeps the handle, delete only clears it
	local p = game.Ped.new(copy(ped.handle))
	p:release()
	assert(p:isValid() and p:equals(ped), "release of a non-script entity keeps the handle")
	p:delete()
	assert(not p:isValid() and ped:exists(), "delete of the player ped only clears the handle")
	script.log("info", "PASS lifetime_test")
end)
