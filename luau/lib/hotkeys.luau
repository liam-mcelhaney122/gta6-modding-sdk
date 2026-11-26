--!strict
-- @gta6/hotkeys: the hotkey state machine behind game.mod (Luau twin of sdk/python/gta6mod/game/hotkeys.py,
-- which states the rules). Pure logic with no native calls of its own: poll(read, now) reads input through
-- read(query, action, pad) with query "justPressed", "pressed" or "justReleased" and returns the bindings
-- that fire this tick. game.mod passes game.controls; tests pass a fake.
--
--   press    fires when the action is just pressed while every modifier is held
--   release  fires when it is just released while every modifier is held; with maxMs, only when the press
--            that ends here began at most maxMs earlier (a tap) and was seen
--   hold     fires once the action has been held ms since a press this object saw (modifiers held), then
--            every repeatMs while still held (once if repeatMs is nil); an action already held when the
--            bindings start counts only from its next press
--   Most specific wins: among bindings of one kind on one action and pad that fire on a tick, one whose
--   modifiers are a strict subset of another's is dropped. A press that fired a chord also suppresses
--   release and hold bindings of that action with fewer modifiers until the action is released.
--   A binding with busy = true does not fire; its state still updates.
-- Actions and modifiers are 32-bit script keys (a Control value, a number or an "INPUT_*" name).

local native = require("@gta6/native")

export type Kind = "press" | "release" | "hold"
export type Query = "justPressed" | "pressed" | "justReleased"
export type Reader = (query: Query, action: number, pad: number) -> boolean
export type Action = number | string
export type Hotkey = {
	kind: Kind,
	action: number,
	pad: number,
	mods: { number },
	ms: number?,
	repeatMs: number?,
	fn: any,
	busy: boolean,
	nextAt: number?,
}
export type Hotkeys = {
	bindings: { Hotkey },
	add: (kind: Kind, action: Action, fn: any, pad: number?, mods: (Action | { Action })?, ms: number?, repeatMs: number?) -> Hotkey,
	poll: (read: Reader, now: number) -> { Hotkey },
}
export type Block = { actions: { number }, pad: number, when: any, active: () -> boolean }

local hotkeys = {}

-- 32-bit script key of a Control value, a number or an "INPUT_*" name.
function hotkeys.actionKey(action: Action): number
	if type(action) == "string" then
		return native.joaat(action)
	end
	return bit32.band(action :: number, 0xFFFFFFFF)
end

-- nil, one action, or a list of actions -> a sorted list of distinct script keys.
function hotkeys.modifierKeys(mods: (Action | { Action })?): { number }
	local out: { number } = {}
	local seen: { [number]: boolean } = {}
	local function put(a: Action)
		local k = hotkeys.actionKey(a)
		if not seen[k] then
			seen[k] = true
			table.insert(out, k)
		end
	end
	if type(mods) == "table" then
		for _, a in mods :: { Action } do
			put(a)
		end
	elseif mods ~= nil then
		put(mods :: Action)
	end
	table.sort(out)
	return out
end

local function contains(list: { number }, k: number): boolean
	return table.find(list, k) ~= nil
end

-- a is a strict subset of b (both lists of distinct keys)
local function strictSubset(a: { number }, b: { number }): boolean
	if #a >= #b then
		return false
	end
	for _, k in a do
		if not contains(b, k) then
			return false
		end
	end
	return true
end

local function keyOf(action: number, pad: number): string
	return pad .. ":" .. action
end

function hotkeys.new(): Hotkeys
	local self = {} :: Hotkeys
	self.bindings = {}
	local downAt: { [string]: number } = {}       -- action/pad -> time of the press we saw
	local chord: { [string]: { number } } = {}   -- action/pad -> modifiers of the chord that press fired

	function self.add(kind: Kind, action: Action, fn: any, pad: number?, mods: (Action | { Action })?, ms: number?, repeatMs: number?): Hotkey
		if kind ~= "press" and kind ~= "release" and kind ~= "hold" then
			error("unknown hotkey kind " .. tostring(kind), 2)
		end
		if kind == "hold" and ms == nil then
			error("a hold binding needs ms >= 0", 2)
		end
		if ms ~= nil and ms < 0 then
			error("ms must be >= 0", 2)
		end
		if repeatMs ~= nil and repeatMs <= 0 then
			error("repeatMs must be > 0", 2)
		end
		local b: Hotkey = {
			kind = kind,
			action = hotkeys.actionKey(action),
			pad = pad or 0,
			mods = hotkeys.modifierKeys(mods),
			ms = ms,
			repeatMs = repeatMs,
			fn = fn,
			busy = false,
			nextAt = nil,
		}
		if contains(b.mods, b.action) then
			error("an action cannot be its own modifier", 2)
		end
		table.insert(self.bindings, b)
		return b
	end

	function self.poll(read: Reader, now: number): { Hotkey }
		local cache: { [string]: boolean } = {}
		local function q(query: Query, action: number, pad: number): boolean
			local k = query .. ":" .. pad .. ":" .. action
			local v = cache[k]
			if v == nil then
				v = read(query, action, pad) == true
				cache[k] = v
			end
			return v :: boolean
		end
		local function modsHeld(b: Hotkey): boolean
			for _, m in b.mods do
				if not q("pressed", m, b.pad) then
					return false
				end
			end
			return true
		end

		-- actions whose press time or chord matters: release/hold bindings, and chord presses
		local tracked: { [string]: { number } } = {}
		for _, b in self.bindings do
			if b.kind ~= "press" or #b.mods > 0 then
				tracked[keyOf(b.action, b.pad)] = { b.action, b.pad }
			end
		end
		for key, ap in tracked do
			if q("justPressed", ap[1], ap[2]) then
				downAt[key] = now
				chord[key] = nil
			end
		end

		local fired: { Hotkey } = {}
		for _, b in self.bindings do
			local key = keyOf(b.action, b.pad)
			if b.kind == "hold" then
				local t0 = downAt[key]
				if not q("pressed", b.action, b.pad) or t0 == nil then
					b.nextAt = nil
				else
					if b.nextAt == nil then
						b.nextAt = t0 + (b.ms :: number)
					end
					if now >= (b.nextAt :: number) and modsHeld(b) then
						local r = b.repeatMs
						b.nextAt = if r ~= nil then now + r else math.huge
						table.insert(fired, b)
					end
				end
			elseif b.kind == "press" then
				if q("justPressed", b.action, b.pad) and modsHeld(b) then
					table.insert(fired, b)
				end
			else -- release
				if q("justReleased", b.action, b.pad) and modsHeld(b) then
					local t0 = downAt[key]
					if b.ms == nil or (t0 ~= nil and now - t0 <= (b.ms :: number)) then
						table.insert(fired, b)
					end
				end
			end
		end

		-- most specific wins
		local kept: { Hotkey } = {}
		for _, b in fired do
			local key = keyOf(b.action, b.pad)
			local drop = b.kind ~= "press" and chord[key] ~= nil and strictSubset(b.mods, chord[key])
			if not drop then
				for _, o in fired do
					if o ~= b and o.kind == b.kind and o.action == b.action and o.pad == b.pad and strictSubset(b.mods, o.mods) then
						drop = true
						break
					end
				end
			end
			if not drop then
				table.insert(kept, b)
			end
		end
		for _, b in kept do
			if b.kind == "press" and #b.mods > 0 then
				local key = keyOf(b.action, b.pad)
				if chord[key] == nil or #b.mods > #chord[key] then
					chord[key] = b.mods
				end
			end
		end

		-- after the bindings, so the release tick still sees the press time and chord
		for key, ap in tracked do
			if not q("pressed", ap[1], ap[2]) and not q("justPressed", ap[1], ap[2]) then
				downAt[key] = nil
				chord[key] = nil
			end
		end

		local out: { Hotkey } = {}
		for _, b in kept do
			if not b.busy then
				table.insert(out, b)
			end
		end
		return out
	end

	return self
end

-- Actions disabled every tick while `when` is truthy: a toggle (isOn), a function, or nil for always.
function hotkeys.block(actions: Action | { Action }, pad: number?, when: any): Block
	local keys: { number } = {}
	if type(actions) == "table" then
		for _, a in actions :: { Action } do
			table.insert(keys, hotkeys.actionKey(a))
		end
	else
		table.insert(keys, hotkeys.actionKey(actions :: Action))
	end
	if #keys == 0 then
		error("block needs at least one action", 2)
	end
	local blk = { actions = keys, pad = pad or 0, when = when } :: Block
	function blk.active(): boolean
		local w = blk.when
		if w == nil then
			return true
		elseif type(w) == "function" then
			return not not w()
		elseif type(w) == "table" and type(w.isOn) == "function" then
			return not not w.isOn()
		end
		return not not w
	end
	return blk
end

return table.freeze(hotkeys)
