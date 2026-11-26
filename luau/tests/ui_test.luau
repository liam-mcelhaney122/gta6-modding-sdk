--!strict
-- @gta6/ui without interactions: every widget once with its default result, the scoped forms, the rules
-- (widgets only inside a frame, no waits, no natives there), ui.defer, ui.remove.
-- Run: g6_luau_runner sdk\luau\lib sdk\luau\tests\ui_test.luau 3000 --press 51@480 --expect-clean --expect-log "PASS ui_test"
-- Interactions (clicks, set values) are covered by tests/scenarios/ui_widgets.luau.

local script = require("@gta6/script")
local ui = require("@gta6/ui")
local game = require("@gta6/game")

assert(ui.available(), "the mock host has a UI layer")
assert(not pcall(ui.button, "outside"), "widgets outside a frame raise an error")

local frames = 0
local deferredRan = false
local checks: { [string]: boolean } = {}
local hudFrames = 0

local registered = ui.onFrame("ui_test", function(frame)
	frames += 1
	assert(frame.menuOpen and frame.width > 0 and frame.height > 0 and frame.frame >= 0, "frame fields")
	local drawn, open = ui.window("UI test", function()
		ui.text("plain", 1, true, nil)
		ui.textColored({ 1, 0, 0 }, "red")
		assert(ui.button("Button") == false, "button not clicked")
		local b, bc = ui.checkbox("Check", true)
		assert(b == true and bc == false, "checkbox keeps its value")
		local i, ic = ui.sliderInt("Int", 3, 0, 5)
		assert(i == 3 and not ic, "sliderInt keeps its value")
		local f, fc = ui.sliderFloat("Float", 0.5, 0, 1, "%.2f")
		assert(math.abs(f - 0.5) < 1e-6 and not fc, "sliderFloat keeps its value")
		local t, tc = ui.inputText("Name", "Lucia", 32)
		assert(t == "Lucia" and not tc, "inputText keeps its text")
		local k, kc = ui.combo("Weather", 2, { "Clear", "Rain", "Fog" })
		assert(k == 2 and not kc, "combo keeps its 1-based index")
		ui.separator()
		ui.sameLine()
		assert(ui.tree("Closed tree", function()
			error("a closed tree's function must not run")
		end) == false, "tree nodes start closed")
		local tabs = 0
		assert(ui.tabBar("tabs", function()
			ui.tab("First", function()
				tabs += 1
			end)
			ui.tab("Second", function()
				tabs += 10
			end)
		end), "tab bar visible")
		assert(tabs == 1, "the first tab is selected")
		local c, cc = ui.colorEdit("Tint", { 0.25, 0.5, 0.75 }, { noAlpha = true })
		assert(c[1] == 0.25 and c[3] == 0.75 and c[4] == 1 and not cc, "colorEdit returns the color, alpha defaults to 1")
		ui.plotLines("Plot", { 1, 2, 3 }, { overlay = "fps" })
		-- the unscoped forms
		if ui.treeNode("Raw tree") then
			ui.treePop()
		end
		if ui.beginTabBar("raw") then
			if ui.beginTabItem("Only") then
				ui.endTabItem()
			end
			ui.endTabBar()
		end
		checks.widgets = true
	end, { x = 10, y = 10, autoResize = true, closable = true })
	assert(drawn and open, "window drawn and open")
	local d2 = ui.beginWindow("Raw window")
	ui.endWindow()
	assert(d2, "beginWindow")

	-- an error inside a scoped function still closes the window, then propagates
	local ok = pcall(ui.window, "Throws", function()
		error("boom")
	end)
	assert(not ok, "the error propagates")
	-- no waits in a frame function: yielding across the host's call is an error
	assert(not pcall(script.wait, 10), "script.wait is refused in a frame")
	-- natives are refused while a frame runs (in-game: not the script thread)
	assert(not pcall(game.player.ped), "natives are refused in a frame")
	checks.rules = true

	if frames == 3 then
		ui.defer(function()
			game.player.ped() -- natives work in the deferred function
			deferredRan = true
		end)
	end
end)
assert(registered, "ui.onFrame registered")
assert(not pcall(ui.onFrame, "ui_test", function() end), "a duplicate frame name is an error")
assert(ui.onFrame("hud", function()
	hudFrames += 1
end, { always = true }), "always frame")

script.register("ui_test.check", function()
	script.wait(500)
	assert(frames > 10, `frames ran ({frames})`)
	assert(checks.widgets and checks.rules, "every check inside the frame ran")
	assert(deferredRan, "ui.defer ran on a tick")
	assert(hudFrames == frames, "the always frame runs with the menu frame")
	assert(ui.remove("hud"), "ui.remove")
	assert(not ui.remove("hud"), "removed once")
	local before = frames
	assert(ui.remove("ui_test"), "remove the menu frame")
	script.wait(100)
	assert(frames == before, "a removed frame no longer runs")
	script.log("info", "PASS ui_test")
end)
