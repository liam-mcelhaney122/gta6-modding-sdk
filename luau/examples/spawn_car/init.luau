--!strict
-- Example mod: press INPUT_CONTEXT (51, "E") to spawn a car in front of the player,
-- and log the player's position every 2 seconds.
--
-- Uses only the generated typed wrappers. All natives here are UNVERIFIED for
-- GTA VI (GTA V hashes); on a real runtime this needs allow_unverified until
-- re-db verifies them. Runs offline today with g6_luau_runner (mock natives).

local script = require("@gta6/script")
local native = require("@gta6/native")
local N = require("@gta6/natives")

local CONTROL_CONTEXT = 51

-- Entity handles are opaque 16-byte buffers; print one as a single 128-bit hex
-- number (the same text the Python example prints). Display only: never build one.
local function handleHex(h: buffer): string
	local s = string.format("%08x%08x%08x%08x", buffer.readu32(h, 12), buffer.readu32(h, 8), buffer.readu32(h, 4), buffer.readu32(h, 0))
	return "0x" .. (string.match(s, "^0*(.-.)$") or s)
end

local function spawnVehicle(modelName: string)
	local model = native.joaat(modelName)
	N.STREAMING.REQUEST_MODEL(model)
	local deadline = script.now() + 2000
	while not N.STREAMING.HAS_MODEL_LOADED(model) do
		if script.now() > deadline then
			script.log("warn", "model " .. modelName .. " did not load")
			return
		end
		script.wait(0)
	end
	local ped = N.PLAYER.PLAYER_PED_ID()
	local pos = N.ENTITY.GET_ENTITY_COORDS(ped, true)
	local veh = N.VEHICLE.CREATE_VEHICLE(model, pos.x + 3, pos.y, pos.z, 0, false, false, false)
	N.STREAMING.SET_MODEL_AS_NO_LONGER_NEEDED(model)
	script.log("info", `spawned {modelName} as handle {handleHex(veh)}`)
end

script.register("spawn_car", function()
	while true do
		if N.PAD.IS_CONTROL_JUST_PRESSED(0, CONTROL_CONTEXT) then
			spawnVehicle("adder")
		end
		script.wait(0)
	end
end)

script.register("where_am_i", function()
	while true do
		local p = N.ENTITY.GET_ENTITY_COORDS(N.PLAYER.PLAYER_PED_ID(), true)
		script.log("info", string.format("player at %.1f %.1f %.1f", p.x, p.y, p.z))
		script.wait(2000)
	end
end)
