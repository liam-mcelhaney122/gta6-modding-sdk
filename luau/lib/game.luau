--!nonstrict
-- @gta6/game: the script layer helpers (Luau twin of sdk/python/gta6mod/game). Design: sdk/docs/script_layer.md.
--
--   local game = require("@gta6/game")
--   local m = game.mod("god_mode")
--   local god = m.toggle(game.GTA5Control.DETONATE)
--   god.onEnable(function() game.player.ped():setInvincible(true) end)
--   m.every(250, function() ... end, god)
--
-- Helpers call natives through @gta6/names only, so status checks and marshalling are the raw
-- wrappers' own. Helpers that wait (models.request, Vehicle.spawn, world.findGround) block the calling
-- script with script.wait; call them from a script, not from top-level mod code.

local script = require("@gta6/script")
local native = require("@gta6/native")
local names = require("@gta6/names")

local game = {}
game.names = names

-- ------------------------------------------------------------------ core
function game.wait(ms: number?)
	script.wait(ms or 0)
end

function game.now(): number
	return script.now()
end

-- level: "debug" | "info" | "warn" | "error" (case-insensitive); source: log source, default the running script
function game.log(msg: string, level: string?, source: string?)
	script.log(string.lower(level or "info"), tostring(msg), source)
end

function game.joaat(s: string): number
	return native.joaat(s)
end

-- ------------------------------------------------------------------ entities
-- A wrapper holds the 16-byte handle buffer exactly as a native returned it. It never builds or
-- changes a handle. A handle whose low 8 bytes are 0 means "no entity" (isValid() is false).

--- Seat index for the driver in GTA V and RDR2 scripts. Unverified for GTA VI (sdk/docs/script_layer.md).
game.SEAT_DRIVER = -1

local Entity = {}
Entity.__index = Entity
local Ped = setmetatable({}, { __index = Entity })
Ped.__index = Ped
local Vehicle = setmetatable({}, { __index = Entity })
Vehicle.__index = Vehicle

function Entity.new(handle)
	return setmetatable({ handle = handle }, Entity)
end
function Ped.new(handle)
	return setmetatable({ handle = handle }, Ped)
end
function Vehicle.new(handle)
	return setmetatable({ handle = handle }, Vehicle)
end

local function handleIsValid(h): boolean
	if typeof(h) == "buffer" then
		return buffer.len(h) == 16 and (buffer.readu32(h, 0) ~= 0 or buffer.readu32(h, 4) ~= 0)
	elseif typeof(h) == "number" then
		return h ~= 0
	end
	return false
end

local function handlesEqual(a, b): boolean
	if typeof(a) == "buffer" and typeof(b) == "buffer" then
		if buffer.len(a) ~= buffer.len(b) then
			return false
		end
		for off = 0, buffer.len(a) - 4, 4 do
			if buffer.readu32(a, off) ~= buffer.readu32(b, off) then
				return false
			end
		end
		return true
	end
	return a == b
end

function Entity:isValid(): boolean
	return handleIsValid(self.handle)
end
function Entity:equals(other): boolean
	return other ~= nil and handlesEqual(self.handle, other.handle)
end
function Entity:exists(): boolean
	return self:isValid() and names.entity.doesExist(self.handle)
end
function Entity:position(): vector
	return names.entity.getCoords(self.handle, true, false)
end
function Entity:setPosition(v: vector)
	names.entity.setCoords(self.handle, v, false, false, false, false)
end
function Entity:heading(): number
	return names.entity.getHeading(self.handle)
end
function Entity:setHeading(degrees: number)
	names.entity.setHeading(self.handle, degrees)
end
function Entity:health(): number
	return names.entity.getHealth(self.handle)
end
function Entity:setHealth(value: number)
	names.entity.setHealth(self.handle, value, 0, 0, 0) -- killer: no entity
end
function Entity:maxHealth(): number
	return names.entity.getMaxHealth(self.handle, false)
end
function Entity:heal()
	self:setHealth(self:maxHealth())
end
function Entity:isDead(): boolean
	return names.entity.isDead(self.handle)
end
function Entity:speed(): number
	return names.entity.getSpeed(self.handle)
end
function Entity:forward(): vector
	return names.entity.getForwardVector(self.handle)
end
function Entity:model(): number
	return names.entity.getModel(self.handle)
end
function Entity:isVisible(): boolean
	return names.entity.isVisible(self.handle)
end
function Entity:setVisible(on: boolean)
	names.entity.setVisible(self.handle, on)
end
function Entity:setInvincible(on: boolean?)
	names.entity.setInvincible(self.handle, on ~= false)
end
function Entity:freeze(on: boolean?)
	names.entity.freezePosition(self.handle, on ~= false)
end
function Entity:setVelocity(v: vector)
	names.entity.setVelocity(self.handle, v)
end
-- World position of a point in this entity's local frame (x right, y forward, z up).
function Entity:offset(x: number?, y: number?, z: number?): vector
	return names.entity.getOffsetFromEntityInWorldCoords(self.handle, vector.create(x or 0, y or 0, z or 0))
end
function Entity:distanceTo(other): number
	local b = if typeof(other) == "vector" then other else other:position()
	return vector.magnitude(self:position() - b)
end

function Ped:setCanRagdoll(on: boolean)
	names.ped.setCanRagdoll(self.handle, on)
end
function Ped:inVehicle(): boolean
	return names.ped.isInAnyVehicle(self.handle, false, 0, 0)
end
-- The vehicle this ped is in, or nil.
function Ped:vehicle()
	if not self:inVehicle() then
		return nil
	end
	local v = Vehicle.new(names.ped.getVehiclePedIsUsing(self.handle, 0))
	return if v:isValid() then v else nil
end
function Ped:putInto(vehicle, seat: number?)
	names.ped.setIntoVehicle(self.handle, vehicle.handle, seat or game.SEAT_DRIVER)
end

function Vehicle:repair()
	names.vehicle.setFixed(self.handle)
end

game.Entity = Entity
game.Ped = Ped
game.Vehicle = Vehicle

-- ------------------------------------------------------------------ models
-- Models are names (hashed with joaat) or hashes.
local models = {}

function models.hashOf(model: string | number): number
	if typeof(model) == "string" then
		return native.joaat(model)
	end
	return (model :: number) % 4294967296
end
function models.isValid(model: string | number): boolean
	return names.streaming.isModelValid(models.hashOf(model))
end
-- Models each script thread has requested and is still waiting for. A mod's stop() releases the ones
-- held by its cancelled scripts (Python gets the same from the generator's finally block).
local pendingModels: { [thread]: { [number]: boolean } } = setmetatable({}, { __mode = "k" }) :: any

-- Request a model and wait (blocking this script) until it is loaded. False on timeout (the request is
-- released again).
function models.request(model: string | number, timeoutMs: number?): boolean
	local h = models.hashOf(model)
	names.streaming.requestModel(h, false)
	local co = coroutine.running()
	local mine = pendingModels[co] or {}
	pendingModels[co] = mine
	mine[h] = true
	local deadline = script.now() + (timeoutMs or 2000)
	while not names.streaming.hasModelLoaded(h) do
		if script.now() > deadline then
			mine[h] = nil
			models.release(h)
			return false
		end
		script.wait(0)
	end
	mine[h] = nil
	return true
end
function models.release(model: string | number)
	names.streaming.setModelAsNoLongerNeeded(models.hashOf(model))
end
game.models = models

local function releasePendingModels(co: thread)
	local mine = pendingModels[co]
	if mine then
		pendingModels[co] = nil
		for h in mine do
			models.release(h)
		end
	end
end

-- Stream the model, create the vehicle, release the model (blocks this script). Nil when the model does
-- not load in time or the game refuses to create the vehicle.
function Vehicle.spawn(model: string | number, pos: vector, heading: number?, timeoutMs: number?)
	local h = models.hashOf(model)
	if not models.request(h, timeoutMs or 2000) then
		return nil
	end
	local veh = Vehicle.new(names.vehicle.create(h, pos.x, pos.y, pos.z, heading or 0, false, false, false))
	models.release(h)
	return if veh:isValid() then veh else nil
end

-- ------------------------------------------------------------------ player
local player = {}

function player.ped()
	return Ped.new(names.player.pedId())
end
-- Player index. Uses PLAYER_ID, which is only a GTA V seed hash (unverified) in 1.0.2.1.
function player.id(): number
	return names.player.id()
end
function player.position(): vector
	return player.ped():position()
end
function player.vehicle()
	return player.ped():vehicle()
end
-- Move the player, or the vehicle the player is in when withVehicle is not false.
function player.teleport(pos: vector, heading: number?, withVehicle: boolean?)
	local ped = player.ped()
	local target = (withVehicle ~= false and ped:vehicle()) or ped
	target:setPosition(pos)
	if heading ~= nil then
		target:setHeading(heading)
	end
end
function player.wantedLevel(): number
	return names.player.getWantedLevel(player.id(), 0)
end
function player.clearWanted()
	names.player.clearWantedLevel(player.id(), 0)
end
function player.maxWanted(): number
	return names.player.getMaxWantedLevel()
end
function player.setMaxWanted(level: number)
	names.player.setMaxWantedLevel(level)
end
game.player = player

-- ------------------------------------------------------------------ world
local world = {}

-- The map waypoint, or nil when none is set. Its z may be 0; probe the ground before using it.
function world.waypoint(): vector?
	if not names.map.isWaypointActive() then
		return nil
	end
	return names.map.getWaypointCoords()
end
function world.requestCollision(pos: vector)
	names.streaming.requestCollisionAtCoord(pos)
end
-- One ground probe straight down from probeZ. Nil when there is no loaded ground there.
function world.groundZ(x: number, y: number, probeZ: number?): number?
	local found, z = names.misc.getGroundZAndNormalFor3dCoord(vector.create(x, y, probeZ or 1000))
	return if found then z else nil
end
-- Request collision at (x, y) and probe until ground is found (blocks this script). Nil on timeout.
function world.findGround(x: number, y: number, timeoutMs: number?, probeZ: number?): number?
	local pz = probeZ or 1000
	local deadline = script.now() + (timeoutMs or 1500)
	while true do
		world.requestCollision(vector.create(x, y, pz))
		local z = world.groundZ(x, y, pz)
		if z ~= nil or script.now() > deadline then
			return z
		end
		script.wait(0)
	end
end
function world.setTimeScale(scale: number)
	names.misc.setTimeScale(scale)
end
game.world = world

-- ------------------------------------------------------------------ clock
local clock = {}

function clock.time(): (number, number, number)
	return names.clock.getHours(), names.clock.getMinutes(), names.clock.getSeconds()
end
function clock.setTime(hours: number, minutes: number?, seconds: number?)
	names.clock.setTime(hours, minutes or 0, seconds or 0, 0)
end
function clock.add(hours: number?, minutes: number?, seconds: number?)
	names.clock.addToClockTime(hours or 0, minutes or 0, seconds or 0)
end
function clock.pause(on: boolean?)
	names.clock.pause(on ~= false, 0)
end
game.clock = clock

-- ------------------------------------------------------------------ controls
--- GTA V control indices (default keyboard key in the comment).
--- GTA VI's IS_CONTROL_* natives carry RDR2's signature, where the action is a hash. These values are
--- placeholders for config, not GTA VI facts; sdk/docs/script_layer.md says how to find the real ids.
game.GTA5Control = table.freeze({
	MULTIPLAYER_INFO = 20, -- Z
	SPECIAL_ABILITY_SECONDARY = 29, -- B
	DETONATE = 47, -- G
	CONTEXT = 51, -- E
	CONTEXT_SECONDARY = 52, -- Q
	VEH_DUCK = 73, -- X
	VEH_HEADLIGHT = 74, -- H
})

local controls = {}
function controls.justPressed(action: number | string, pad: number?): boolean
	return names.pad.isControlJustPressed(pad or 0, action)
end
function controls.pressed(action: number | string, pad: number?): boolean
	return names.pad.isControlPressed(pad or 0, action)
end
function controls.justReleased(action: number | string, pad: number?): boolean
	return names.pad.isControlJustReleased(pad or 0, action)
end
-- Disable an action for this frame (call every tick to keep it disabled).
function controls.disable(action: number | string, pad: number?)
	names.pad.disableControlAction(pad or 0, action, false)
end
game.controls = controls

-- ------------------------------------------------------------------ mod structure
-- game.mod(name) registers the mod's scripts as "<name>.<part>". Handlers run inside a script, so they
-- may wait. A key handler (other than a toggle) runs as its own script "<name>.keyN", so a waiting
-- handler never delays other bindings; a press while that binding's handler still runs is ignored.
-- Toggle and onStop callbacks also run from m.stop(), which may be called outside a script: keep them
-- free of waits.
function game.mod(name: string)
	local m = { name = name, stopped = false }
	local used: { [string]: boolean } = {}
	local live: { string } = {}  -- every script name this mod registered (cancelled on stop)
	local threads: { [thread]: boolean } = setmetatable({}, { __mode = "k" }) :: any
	local bindings = {}
	local toggles = {}
	local stopHandlers = {}
	local inputRegistered = false
	local keySeq = 0

	function m.log(msg: string)
		script.log("info", msg, name)
	end
	function m.warn(msg: string)
		script.log("warn", msg, name)
	end
	function m.error(msg: string)
		script.log("error", msg, name)
	end

	local function add(part: string, body: () -> ())
		local sname = name .. "." .. part
		local k = 2
		while used[sname] do
			sname = name .. "." .. part .. k
			k += 1
		end
		used[sname] = true
		table.insert(live, sname)
		script.register(sname, function()
			threads[coroutine.running()] = true
			body()
		end)
	end

	-- Run fn every tick until the mod stops (fn may also loop and wait itself).
	function m.loop(fn: () -> (), part: string?)
		add(part or "loop", function()
			while not m.stopped do
				fn()
				if m.stopped then
					return
				end
				script.wait(0)
			end
		end)
	end

	-- Call fn every ms of game time, only while `when` (a toggle) is on, if given.
	function m.every(ms: number, fn: () -> (), when)
		add("every", function()
			while not m.stopped do
				if when == nil or when.isOn() then
					fn()
				end
				if m.stopped then
					return
				end
				script.wait(ms)
			end
		end)
	end

	local function dispatch(b)
		if b.inline then
			b.fn()
			return
		end
		b.busy = true
		keySeq += 1
		add("key" .. keySeq, function()
			local ok, err = pcall(b.fn)
			b.busy = false
			if not ok then
				error(err, 0)
			end
		end)
	end

	local function inputLoop()
		while not m.stopped do
			for _, b in bindings do
				if not b.busy and controls.justPressed(b.key, b.pad) then
					dispatch(b)
					if m.stopped then
						return
					end
				end
			end
			script.wait(0)
		end
	end

	local function bind(key, pad: number?, fn: () -> (), inline: boolean)
		table.insert(bindings, { key = key, pad = pad or 0, fn = fn, inline = inline, busy = false })
		if not inputRegistered then
			inputRegistered = true
			add("input", inputLoop)
		end
	end

	-- Call fn when key is just pressed on pad (default 0).
	function m.onKey(key, fn: () -> (), pad: number?)
		bind(key, pad, fn, false)
	end

	-- An on/off switch bound to a key. Flip it with the key, or call set / flip.
	function m.toggle(key, label: string?, pad: number?)
		local t = { key = key, label = label or name }
		local on = false
		local enable, disable = {}, {}
		function t.isOn(): boolean
			return on
		end
		function t.set(v: boolean)
			v = v == true
			if v == on then
				return
			end
			on = v
			for _, f in (if on then enable else disable) do
				f()
			end
		end
		function t.flip()
			t.set(not on)
		end
		function t.onEnable(f: () -> ())
			table.insert(enable, f)
			return f
		end
		function t.onDisable(f: () -> ())
			table.insert(disable, f)
			return f
		end
		-- stop(): switch off, running every disable callback through `guarded`.
		function t._stopOff(guarded: (() -> ()) -> ())
			if on then
				on = false
				for _, f in disable do
					guarded(f)
				end
			end
		end
		table.insert(toggles, t)
		bind(key, pad, t.flip, true)
		return t
	end

	function m.onStop(fn: () -> ())
		table.insert(stopHandlers, fn)
		return fn
	end

	-- Stop the mod: cancel its scripts first (a suspended handler never resumes; models it was waiting
	-- for are released), then turn every toggle off and run onStop handlers. Each cleanup callback runs
	-- even if an earlier one fails; failures are logged and returned. If called from one of the mod's
	-- own scripts, that script ends at its next wait.
	function m.stop(): { string }
		if m.stopped then
			return {}
		end
		m.stopped = true
		for _, sname in live do
			script.cancel(sname)
		end
		for co in threads do
			releasePendingModels(co)
		end
		local errors: { string } = {}
		local function guarded(f: () -> ())
			local ok, err = pcall(f)
			if not ok then
				table.insert(errors, tostring(err))
				m.error("stop: cleanup failed: " .. tostring(err))
			end
		end
		for _, t in toggles do
			t._stopOff(guarded)
		end
		for _, f in stopHandlers do
			guarded(f)
		end
		return errors
	end

	return m
end

return table.freeze(game)
