--!strict
-- Luau host test against the mock runtime (g6_luau_runner fake world, see g6_luau_runner.cpp).
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\host_test.luau 3000 --press 51@480 --expect-clean --expect-log "PASS host_test"
-- Covers: 16-byte entity buffer round trip, packed Vector3 return and parameter ('w': one 16-byte
-- block in two slots), the 16-byte aligned Vector3* out-param, wait() scheduling, one-tick control
-- presses, and the error paths of native.call.

local script = require("@gta6/script")
local native = require("@gta6/native")
local N = require("@gta6/natives")

local PRESS_AT = 480  -- must match --press 51@480

local function u64lo(h: buffer): number return buffer.readu32(h, 0) + buffer.readu32(h, 4) * 2 ^ 32 end
local function u64hi(h: buffer): number return buffer.readu32(h, 8) + buffer.readu32(h, 12) * 2 ^ 32 end
local function eq3(v: vector, x: number, y: number, z: number): boolean return v.x == x and v.y == y and v.z == z end
local function expectError(what: string, needle: string, f: () -> ())
	local ok, err = pcall(f)
	assert(not ok, what .. ": expected an error")
	assert(string.find(tostring(err), needle, 1, true), what .. ": unexpected error: " .. tostring(err))
end

-- ------------------------------------------------------------------ entity buffers (top level: no waits needed)
local ped = N.PLAYER.PLAYER_PED_ID()
assert(typeof(ped) == "buffer" and buffer.len(ped) == 16, "PLAYER_PED_ID must return a 16-byte buffer")
assert(u64lo(ped) == 0x100 and u64hi(ped) == 0, "player handle lo/hi")
assert(N.ENTITY.DOES_ENTITY_EXIST(ped) == true, "round trip: returned buffer passed back")
assert(N.ENTITY.DOES_ENTITY_EXIST(0x100) == true, "a number is accepted as lo")

local forged = buffer.create(16)
buffer.writeu32(forged, 0, 0x100)
buffer.writeu32(forged, 8, 0xABCD)
assert(N.ENTITY.DOES_ENTITY_EXIST(forged) == false, "hi half must reach the handler (lo matches, hi differs)")
buffer.writeu32(forged, 8, 0)
assert(N.ENTITY.DOES_ENTITY_EXIST(forged) == true, "same 16 bytes as the player handle")

expectError("short buffer", "must be 16 bytes", function() N.ENTITY.DOES_ENTITY_EXIST(buffer.create(8)) end)
expectError("not a native", "expected a native handle", function() native.call(buffer.create(16) :: any) end)
expectError("unregistered native", "not present in this game build", function() N.ENTITY.GET_ENTITY_ALPHA(ped) end)

-- ------------------------------------------------------------------ packed Vector3 return and parameter
local p = N.ENTITY.GET_ENTITY_COORDS(ped, true)
assert(typeof(p) == "vector", "GET_ENTITY_COORDS returns a vector")
assert(eq3(p, -1000, -2000, 15), `player coords {p}`)
-- SET_ENTITY_COORDS is 'ewbbbbf>n': entity in slots 0-1, the packed position in 2-3 (x,y in slot 2, z in the
-- low half of slot 3), four BOOLs in 4-7 and a float in 8. The runner's handler reads the position from 2-3.
N.ENTITY.SET_ENTITY_COORDS(ped, vector.create(1.5, -2.25, 30.125), false, false, false, false)
p = N.ENTITY.GET_ENTITY_COORDS(ped, true)
assert(eq3(p, 1.5, -2.25, 30.125), `moved coords {p} (packed param in, packed return out)`)
N.ENTITY.SET_ENTITY_COORDS(ped, vector.create(-1000, -2000, 15), false, false, false, false, 0)
assert(eq3(N.ENTITY.GET_ENTITY_COORDS(ped, true), -1000, -2000, 15), "coords restored")
local off = N.ENTITY.GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(ped, vector.create(0, 2, 1))
assert(eq3(off, -1000, -1998, 16), `offset from entity {off} (heading 0: +y forward)`)

-- ------------------------------------------------------------------ scheduling
local order: { string } = {}

script.register("sched", function()
	local t0 = script.now()
	assert(t0 == N.MISC.GET_GAME_TIMER(), "script.now() matches GET_GAME_TIMER")
	table.insert(order, "sched@" .. t0)
	script.wait(0)
	local t1 = script.now()
	assert(t1 - t0 == 16, `wait(0) resumes on the next tick (got +{t1 - t0})`)
	table.insert(order, "sched@" .. t1)
	script.wait(100)
	local t2 = script.now()
	assert(t2 - t1 == 112, `wait(100) resumes on the first tick at or after +100 (got +{t2 - t1})`)
	script.wait(-5)  -- negative waits clamp to 0
	assert(script.now() - t2 == 16, "wait(-5) behaves as wait(0)")

	-- model streaming + vehicle: CREATE_VEHICLE returns the null handle until the model has loaded
	local model = native.joaat("adder")
	N.STREAMING.REQUEST_MODEL(model)
	local early = N.VEHICLE.CREATE_VEHICLE(model, 0, 0, 0, 0, false, false, false)
	assert(buffer.len(early) == 16 and u64lo(early) == 0 and u64hi(early) == 0, "null handle while not loaded")
	local polls = 0
	while not N.STREAMING.HAS_MODEL_LOADED("adder") do  -- string hash argument -> joaat
		polls += 1
		script.wait(0)
	end
	assert(polls >= 3 and polls <= 4, `model loads after 50 ms (polled {polls} ticks)`)
	local veh = N.VEHICLE.CREATE_VEHICLE(model, 7.5, 8.25, -9.0, 90, false, false, false)
	assert(u64lo(veh) == 0x101 and u64hi(veh) == 0, "first spawned handle is 0x101")
	assert(N.ENTITY.DOES_ENTITY_EXIST(veh), "spawned vehicle exists")
	assert(eq3(N.ENTITY.GET_ENTITY_COORDS(veh, true), 7.5, 8.25, -9.0), "vehicle coords round trip")

	-- Vector3* out-param: the host passes a 16-byte aligned buffer (the runner logs an ERROR otherwise,
	-- which --expect-clean turns into a failure). 'wFV>b': packed coord, float*, Vector3*.
	N.STREAMING.REQUEST_COLLISION_AT_COORD(vector.create(3, 4, 0))
	local found, gz, normal = N.MISC.GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD(vector.create(3, 4, 100))
	assert(found == false, "no ground before collision loads")
	script.wait(100)
	found, gz, normal = N.MISC.GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD(vector.create(3, 4, 100))
	assert(found == true and gz == 12 and eq3(normal, 0, 0, 1), `ground probe {found} {gz} {normal}`)
	table.insert(order, "sched:done")
end)

script.register("other", function()
	table.insert(order, "other@" .. script.now())
	script.wait(0)
	table.insert(order, "other@" .. script.now())
end)

script.register("press", function()
	local seen: { number } = {}
	local start = script.now()
	while script.now() - start < 800 do
		if N.PAD.IS_CONTROL_JUST_PRESSED(0, 51) then table.insert(seen, script.now()) end
		assert(not N.PAD.IS_CONTROL_JUST_PRESSED(0, 52), "only control 51 is pressed")
		assert(not N.PAD.IS_CONTROL_JUST_PRESSED(1, 51), "only pad 0 is pressed")
		script.wait(0)
	end
	assert(#seen == 1 and seen[1] == PRESS_AT, `control 51 just-pressed exactly once at {PRESS_AT} (saw {#seen})`)

	local expected = { "sched@0", "other@0", "sched@16", "other@16" }
	for i, e in expected do
		assert(order[i] == e, `tick order [{i}]: expected {e}, got {tostring(order[i])}`)
	end
	assert(table.find(order, "sched:done"), "sched script finished")
	script.log("info", "PASS host_test")
end)
