--!strict
-- @gta6/game helper test against the runner's mock world (sdk/docs/script_layer.md, "Mock world").
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\game_test.luau 3000 --press 51@480 --expect-clean --expect-log "PASS game_test"
-- Covers: entity wrappers, offsets, clock wrap, wanted level, findGround timeout and success,
-- Vehicle.spawn + putInto + teleport with vehicle, toggles, every(), onKey and m.stop().

local script = require("@gta6/script")
local game = require("@gta6/game")

local function near(a: number, b: number): boolean return math.abs(a - b) < 1e-3 end

-- ------------------------------------------------------------------ top level (no waits)
local ped = game.player.ped()
assert(ped:isValid() and ped:exists(), "player ped valid")
assert(ped:equals(game.player.ped()), "two reads of the player ped compare equal")
assert(not game.Entity.new(buffer.create(16)):isValid(), "all-zero handle is not valid")
assert(ped:health() == 200 and ped:maxHealth() == 200, "ped health 200/200")
ped:setHealth(500)
assert(ped:health() == 200, "setHealth clamps to max")
ped:setHealth(120)
assert(ped:health() == 120, "setHealth")
ped:heal()
assert(ped:health() == 200, "heal")
ped:setHeading(90)
local o = ped:offset(0, 5, 1) -- heading 90: forward is (-1, 0, 0)
assert(near(o.x, -1005) and near(o.y, -2000) and near(o.z, 16), `offset at heading 90: {o}`)
assert(near(ped:distanceTo(vector.create(-1000, -1990, 15)), 10), "distanceTo vector")
ped:setVelocity(vector.create(3, 4, 0))
assert(near(ped:speed(), 5), "speed is the velocity length")
ped:setHeading(0)

game.clock.setTime(23, 30, 0)
game.clock.add(1, 0, 0)
local h, mm = game.clock.time()
assert(h == 0 and mm == 30, `clock wraps at 24 h (got {h}:{mm})`)
game.clock.setTime(12)

assert(game.player.wantedLevel() == 0 and game.player.maxWanted() == 5, "wanted 0/5")
assert(game.world.waypoint() == nil, "no waypoint")
assert(game.world.groundZ(0, 0) == nil, "no ground before collision is requested")
assert(game.models.hashOf("adder") == game.joaat("adder"), "hashOf(name) is joaat")

-- ------------------------------------------------------------------ mod structure
local m = game.mod("game_test")
local t = m.toggle(51)
local enabled, disabled, ticks, stops = 0, 0, 0, 0
t.onEnable(function() enabled += 1 end)
t.onDisable(function() disabled += 1 end)
m.every(100, function() ticks += 1 end, t)
m.onStop(function() stops += 1 end)

-- A waiting key handler must not delay a later binding on the same press (both bound to 51).
local m2 = game.mod("game_test_keys")
local slowRuns, slowDone = 0, false
m2.onKey(51, function()
	slowRuns += 1
	game.wait(500)
	slowDone = true
end)
local t2 = m2.toggle(51)
local t2At = -1
t2.onEnable(function() t2At = game.now() end)

-- stop() cancels a script suspended in a model request: no vehicle, and the request is released.
local m3 = game.mod("game_test_cancel")
local spawned = false
m3.loop(function()
	local v = game.Vehicle.spawn("game_test_model", vector.create(0, 0, 0), 0)
	spawned = v ~= nil
	game.wait(100000)
end, "spawner")

script.register("game_test.check", function()
	-- m3's spawner is waiting for its model (the mock loads models after 50 ms): stop it now
	game.wait(16)
	m3.stop()
	game.wait(200)
	assert(not spawned, "a cancelled script must not resume and spawn")

	-- findGround: times out when the probe starts below the ground, succeeds once collision loads
	local t0 = game.now()
	assert(game.world.findGround(10, 10, 50, 5) == nil, "probe below ground times out")
	assert(game.now() - t0 >= 50, "findGround waited for its timeout")
	t0 = game.now()
	local z = game.world.findGround(20, 20) -- a fresh cell: (10, 10) was already requested above
	assert(z == 12, `ground at 12 (got {z})`)
	assert(game.now() - t0 >= 100, "collision loads after 100 ms")

	-- spawn, seat, teleport with the vehicle
	local veh = game.Vehicle.spawn("adder", ped:offset(0, 5, 0), 0)
	assert(veh ~= nil and veh:exists(), "vehicle spawned")
	assert(veh:health() == 1000 and veh:maxHealth() == 1000, "vehicle health 1000/1000")
	ped:putInto(veh)
	local inVeh = game.player.vehicle()
	assert(inVeh ~= nil and inVeh:equals(veh), "player is in the spawned vehicle")
	game.player.teleport(vector.create(1, 2, 3))
	local vp, pp = veh:position(), ped:position()
	assert(near(vp.x, 1) and near(vp.y, 2) and near(vp.z, 3), "teleport moved the vehicle")
	assert(near(pp.x, 1) and near(pp.y, 2) and near(pp.z, 3), "the ped moved with the vehicle")

	-- the toggle is flipped on by --press 51@480
	while game.now() < 900 do game.wait(0) end
	assert(t.isOn() and enabled == 1, "toggle on after the key press")
	assert(t2.isOn() and t2At == 480, `a waiting handler did not delay the toggle (flipped at {t2At})`)
	assert(slowRuns == 1 and not slowDone, "the waiting handler runs in its own script")
	assert(ticks >= 3, `every(100) ran while on (ran {ticks})`)
	m.stop()
	assert(m.stopped and not t.isOn() and disabled == 1 and stops == 1, "stop: toggle off, onStop ran once")
	m.stop()
	assert(stops == 1, "stop is idempotent")
	local after = ticks
	game.wait(300)
	assert(ticks == after, "every() loop ended after stop")
	script.log("info", "PASS game_test")
end)
