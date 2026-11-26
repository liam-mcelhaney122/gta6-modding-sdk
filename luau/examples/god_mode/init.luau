--!strict
-- god_mode: toggle invincibility, no ragdoll and full health (Luau twin of sdk/examples/python/god_mode.py).
-- Offline: g6_luau_runner sdk\luau\lib sdk\luau\examples\god_mode\init.luau 4000 --damage 50@496 --press INPUT_DETONATE@1008 --damage 50@2000 --press INPUT_DETONATE@3008 --damage 30@3504

local game = require("@gta6/game")

-- Config: a GTA VI input action (sdk/docs/api/controls.md). Default GTA VI bindings are not known yet.
local KEY_TOGGLE = game.Control.DETONATE -- GTA V default key G
local CHECK_MS = 250

local m = game.mod("god_mode")
local god = m.toggle(KEY_TOGGLE)
local lastPed: any = nil

local function protect(ped: any)
	ped:setInvincible(true)
	ped:setCanRagdoll(false)
end

god.onEnable(function()
	local ped = game.player.ped()
	protect(ped)
	ped:heal()
	lastPed = ped
	m.log("god mode ON")
end)

god.onDisable(function()
	local ped = game.player.ped()
	ped:setInvincible(false)
	ped:setCanRagdoll(true)
	lastPed = nil
	m.log("god mode OFF")
end)

m.every(CHECK_MS, function()
	local ped = game.player.ped()
	if lastPed == nil or not ped:equals(lastPed) then -- character switch or respawn: a new ped handle
		protect(ped)
		lastPed = ped
		m.log("new player ped, god mode reapplied")
	end
	local max = ped:maxHealth()
	if ped:health() < max then
		ped:heal()
		m.log(`health topped up to {max}`)
	end
end, god)
