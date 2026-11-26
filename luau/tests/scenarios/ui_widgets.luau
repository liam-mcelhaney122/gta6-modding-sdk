--!strict
-- Every @gta6/ui widget driven through the mock UI's interactions.
-- Run (build.cmd test and CTest use exactly these flags):
--   g6_luau_runner sdk\luau\lib sdk\luau\tests\scenarios\ui_widgets.luau 1500 --ui-click Heal@96 --ui-click "God mode@96"
--     --ui-set Wanted=4@96 --ui-set Speed=2.5@96 --ui-set Name=Jason@96 --ui-set Weather=Fog@96 --ui-click Advanced@96
--     --ui-click Second@96 --ui-set Tint=0.1,0.2,0.3,0.4@96 --ui-menu 0@496 --ui-menu 1@800 --expect-clean
--     --expect-ui "checkbox God mode = 1" --expect-ui "tab_item Second (selected)" --expect-log "PASS ui_widgets"

local script = require("@gta6/script")
local ui = require("@gta6/ui")

local state = {
	heals = 0,
	god = false,
	wanted = 0,
	speed = 1.0,
	name = "Lucia",
	weather = 1,
	advancedOpen = false,
	tab = "",
	tint = { 1, 1, 1, 1 } :: { number },
	changes = 0,
	hiddenFrames = 0,
	menuFrames = 0,
}

local weathers = { "Clear", "Rain", "Fog" }

ui.onFrame("widgets", function(frame)
	if not frame.menuOpen then
		state.hiddenFrames += 1 -- must not happen: this frame is not { always = true }
		return
	end
	state.menuFrames += 1
	ui.window("Widgets", function()
		if ui.button("Heal") then
			state.heals += 1
		end
		local changed
		state.god, changed = ui.checkbox("God mode", state.god)
		if changed then
			state.changes += 1
		end
		state.wanted = ui.sliderInt("Wanted", state.wanted, 0, 5)
		state.speed = ui.sliderFloat("Speed", state.speed, 0, 10)
		state.name = ui.inputText("Name", state.name)
		state.weather = ui.combo("Weather", state.weather, weathers)
		ui.tree("Advanced", function()
			state.advancedOpen = true
			ui.text("inside")
		end)
		ui.tabBar("tabs", function()
			ui.tab("First", function()
				state.tab = "First"
			end)
			ui.tab("Second", function()
				state.tab = "Second"
			end)
		end)
		state.tint = ui.colorEdit("Tint", state.tint)
	end)
end)

script.register("ui_widgets.check", function()
	script.wait(1200)
	assert(state.heals == 1, `button clicked once ({state.heals})`)
	assert(state.god == true and state.changes == 1, "checkbox flipped once")
	assert(state.wanted == 4, `sliderInt set ({state.wanted})`)
	assert(math.abs(state.speed - 2.5) < 1e-6, `sliderFloat set ({state.speed})`)
	assert(state.name == "Jason", `inputText set ({state.name})`)
	assert(state.weather == 3, `combo set by item text ({state.weather})`)
	assert(state.advancedOpen, "tree opened by a click")
	assert(state.tab == "Second", `tab selected by a click ({state.tab})`)
	assert(math.abs(state.tint[1] - 0.1) < 1e-6 and math.abs(state.tint[4] - 0.4) < 1e-6, "colorEdit set")
	assert(state.hiddenFrames == 0, "menu frames do not run while the menu is hidden")
	assert(state.menuFrames > 40 and state.menuFrames < 75, `menu frames ({state.menuFrames}): hidden from 496 to 800 ms`)
	script.log("info", "PASS ui_widgets")
end)
