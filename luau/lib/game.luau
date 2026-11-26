--!nonstrict
-- @gta6/game: the script layer helpers (Luau twin of sdk/python/gta6mod/game). Design: sdk/docs/script_layer.md.
--
--   local game = require("@gta6/game")
--   local m = game.mod("god_mode")
--   local god = m.toggle(game.Control.DETONATE)
--   god.onEnable(function() game.player.ped():setInvincible(true) end)
--   m.every(250, function() ... end, god)
--
-- Helpers call natives through @gta6/names only, so status checks and marshalling are the raw
-- wrappers' own. Helpers that wait (models.request, Vehicle.spawn, world.findGround) block the calling
-- script with script.wait; call them from a script, not from top-level mod code.

local script = require("@gta6/script")
local native = require("@gta6/native")
local names = require("@gta6/names")
local controlsData = require("@gta6/controls")
local hotkeysLib = require("@gta6/hotkeys")

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

--- Driver seat for SET_PED_INTO_VEHICLE. Static evidence (findings/phase5/30_script_layer_evidence.md, G-083):
--- the handler maps seat s >= -1 to internal seat s + 1 (-2 = any free seat), and story scripts pass -1 at 197
--- of the 203 sites that seat the player ped. Not yet confirmed in game.
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
	local b = if type(other) == "vector" then other else other:position()
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

-- Delete the entity (DELETE_ENTITY). The native takes the handle by pointer and clears it, so the wrapper's
-- handle becomes what the game wrote back: an all-zero handle (isValid() false) once it is gone. The game only
-- deletes entities this script created; it clears the handle either way.
function Entity:delete()
	self.handle = names.entity.delete(self.handle)
end
-- Tell the game this script no longer needs the entity (SET_ENTITY_AS_NO_LONGER_NEEDED); the world may clean
-- it up later. The game clears the handle (isValid() false) only when it accepted the release.
function Entity:release()
	self.handle = names.entity.setAsNoLongerNeeded(self.handle)
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
-- Player index: GET_PLAYER_INDEX (observed handler 0x144501320), which takes the PLAYER_ID role in GTA VI
-- scripts: it returns 0, and its result is what scripts pass in 1-slot player-index arguments
-- (findings/phase5/30_script_layer_evidence.md, G-082).
function player.id(): number
	return names.player.getIndex()
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
-- GET/CLEAR_PLAYER_WANTED_LEVEL take the player *ped*, not the player index: the handlers pass both argument
-- slots as one 16-byte handle to the ped resolver 0x14526e340, and a non-ped argument resolves to null, so the
-- level reads 0 and nothing is cleared (re-db script-vm.param_types.json, G-090).
function player.wantedLevel(): number
	return names.player.getWantedLevel(player.ped().handle)
end
function player.clearWanted()
	names.player.clearWantedLevel(player.ped().handle)
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
-- Input actions (script keys) and control groups of this build, from @gta6/controls (generated;
-- sdk/docs/api/controls.md). An action may also be given as an "INPUT_*" string or a number.
game.Control = controlsData.Control
game.ControlGroup = controlsData.ControlGroup

-- pad: control group, default ControlGroup.PLAYER_CONTROL (0)
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
function controls.released(action: number | string, pad: number?): boolean
	return names.pad.isControlReleased(pad or 0, action)
end
-- Analogue value of the action (axis or trigger), as GET_CONTROL_NORMAL returns it.
function controls.normal(action: number | string, pad: number?): number
	return names.pad.getControlNormal(pad or 0, action)
end
-- Disable an action for this frame (call every tick to keep it disabled).
function controls.disable(action: number | string, pad: number?)
	names.pad.disableControlAction(pad or 0, action, false)
end
function controls.enable(action: number | string, pad: number?)
	names.pad.enableControlAction(pad or 0, action, false)
end
game.controls = controls

-- ------------------------------------------------------------------ mod structure
-- One named type for a toggle's enable and disable lists: `if on then enable else disable` is then a
-- single type the analyzer can iterate (two inferred {() -> ()} tables form a union it cannot).
type Callbacks = { () -> () }

-- game.mod(name) registers the mod's scripts as "<name>.<part>". Handlers run inside a script, so they
-- may wait. A key handler (other than a toggle) runs as its own script "<name>.keyN", so a waiting
-- handler never delays other bindings; a press while that binding's handler still runs is ignored.
-- Toggle and onStop callbacks also run from m.stop(), which may be called outside a script: keep them
-- free of waits. The host calls m.stop() itself when it raises its stop event (plugin unload).
-- Hotkeys (rules in @gta6/hotkeys): onKey fires on press, onRelease on release (maxMs makes it a tap),
-- onHold after the action is held ms (repeatMs repeats it). opts.mods adds modifier actions that must be
-- held; the most specific chord wins. m.block disables actions every tick while opts.when is on.
--   m.onKey(game.Control.CONTEXT, fn, { mods = game.Control.SPRINT })   -- SPRINT + CONTEXT
--   m.onHold(game.Control.CONTEXT, 800, fn)                            -- hold CONTEXT 0.8 s
--   m.onRelease(game.Control.CONTEXT, fn, { maxMs = 250 })             -- tap CONTEXT
--   m.block({ game.Control.ATTACK, game.Control.AIM }, { when = menu }) -- menu: a toggle or a function
-- Where opts is accepted, a number means { pad = number } (pad: control group, default 0).
function game.mod(name: string)
	local m = { name = name, stopped = false }
	local used: { [string]: boolean } = {}
	local live: { string } = {}  -- every script name this mod registered (cancelled on stop)
	local threads: { [thread]: boolean } = setmetatable({}, { __mode = "k" }) :: any
	local keys = hotkeysLib.new()
	local blocks = {}
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

	-- b.fn = { f = handler, inline = true for toggle flips (never wait) }
	local function dispatch(b)
		local h = b.fn
		if h.inline then
			h.f()
			return
		end
		b.busy = true
		keySeq += 1
		add("key" .. keySeq, function()
			local ok, err = pcall(h.f)
			b.busy = false
			if not ok then
				error(err, 0)
			end
		end)
	end

	local function readControls(query, action: number, pad: number): boolean
		return controls[query](action, pad)
	end

	-- Bindings are read first, then the blocks are applied, every tick.
	local function inputLoop()
		while not m.stopped do
			for _, b in keys.poll(readControls, game.now()) do
				dispatch(b)
				if m.stopped then
					return
				end
			end
			for _, blk in blocks do
				if blk.active() then
					for _, a in blk.actions do
						controls.disable(a, blk.pad)
					end
				end
			end
			script.wait(0)
		end
	end

	local function ensureInput()
		if not inputRegistered then
			inputRegistered = true
			add("input", inputLoop)
		end
	end

	local function options(o): (number, { [string]: any })
		if o == nil then
			return 0, {}
		elseif type(o) == "number" then
			return o, {}
		end
		return o.pad or 0, o
	end

	local function bind(kind, key, fn: () -> (), inline: boolean, o, ms: number?, repeatMs: number?)
		local pad, opts = options(o)
		keys.add(kind, key, { f = fn, inline = inline }, pad, opts.mods, ms, repeatMs)
		ensureInput()
	end

	-- Call fn when key is just pressed (opts: pad number, or { pad, mods }).
	function m.onKey(key, fn: () -> (), opts)
		bind("press", key, fn, false, opts)
	end

	-- Call fn when key is just released (opts: { pad, mods, maxMs }; maxMs: only after a press that short).
	function m.onRelease(key, fn: () -> (), opts)
		local _, o = options(opts)
		bind("release", key, fn, false, opts, o.maxMs)
	end

	-- Call fn once key has been held ms (opts: { pad, mods, repeatMs }; repeatMs: again while still held).
	function m.onHold(key, ms: number, fn: () -> (), opts)
		local _, o = options(opts)
		bind("hold", key, fn, false, opts, ms, o.repeatMs)
	end

	-- Disable actions (one or a list) every tick while opts.when is on (a toggle, a function, or nil for
	-- always; opts.pad default 0). The mod's own bindings are read before the disable each tick; other
	-- scripts and the game see the actions disabled for the rest of that frame [GTA V semantics, not
	-- verified on GTA VI].
	function m.block(actions, opts)
		local pad, o = options(opts)
		local blk = hotkeysLib.block(actions, pad, o.when)
		table.insert(blocks, blk)
		ensureInput()
		return blk
	end

	-- An on/off switch bound to a key (opts: pad number, or { pad, mods }). Flip it with the key, or call
	-- set / flip. key = nil: no key, only set / flip (e.g. from an @gta6/ui menu through ui.defer).
	function m.toggle(key, label: string?, opts)
		local t = { key = key, label = label or name }
		local on = false
		local enable: Callbacks, disable: Callbacks = {}, {}
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
		if key ~= nil then
			bind("press", key, t.flip, true, opts)
		end
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

	-- The host's stop event (plugin stop/unload, g6_luau_stop) stops the mod. Registered when the mod is
	-- created, from top-level code; a mod created after the event is not stopped by it.
	pcall(script.onStop, function()
		m.stop()
	end)

	return m
end

return table.freeze(game)
