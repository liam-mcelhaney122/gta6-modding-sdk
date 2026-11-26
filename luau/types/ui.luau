--!strict
-- Type stub of the host module @gta6/ui (sdk/luau/host/g6_luau.cpp, kUiLib), the Luau face of the g6_ui_api
-- C ABI (sdk/include/gta6mod/gta6mod.h, "UI layer"). The host never loads this file; sdk/luau/analyze.cmake
-- gives it to luau-analyze in place of the C functions. Keep it in step with kUiLib.
--
-- Model: ui.onFrame registers a function the host calls once per drawn frame while the in-game menu is open
-- (Insert), or always with { always = true }. Widget functions work only inside such a function (elsewhere
-- they raise an error). In-game the frame function runs on the render thread: natives fail there and
-- script.wait is an error; keep state in locals your scripts read, or use ui.defer(fn) to run fn on the
-- next script tick. Offline (g6_luau_runner) the mock UI records every call; --ui-click / --ui-set drive it.

local ui = {}

export type Frame = {
	menuOpen: boolean, -- the menu is shown and has the mouse and keyboard
	width: number, -- back buffer size in pixels
	height: number,
	dt: number, -- seconds since the previous frame
	frame: number, -- frame counter
}

export type FrameOptions = {
	always: boolean?, -- also while the menu is hidden (a HUD); it gets no input then
}

export type WindowOptions = {
	x: number?, -- position and size on first use (both of a pair are needed)
	y: number?,
	width: number?,
	height: number?,
	closable: boolean?, -- show a close button; the second result of window/beginWindow turns false when clicked
	noTitleBar: boolean?,
	noResize: boolean?,
	noMove: boolean?,
	noCollapse: boolean?,
	autoResize: boolean?,
	noBackground: boolean?,
	noInputs: boolean?,
}

export type Color = { number } -- { r, g, b [, a] }, each 0..1

export type PlotOptions = {
	overlay: string?,
	min: number?, -- min >= max (the default): automatic scale
	max: number?,
	width: number?,
	height: number?,
}

-- Registers fn for every drawn frame. False when the host has no in-game UI (overlay off, older runtime).
-- Errors if a frame with this name is registered. A frame function that fails 10 times in a row is removed.
function ui.onFrame(name: string, fn: (frame: Frame) -> (), opts: FrameOptions?): boolean
	error("type stub")
end

-- Unregisters the frame `name`. True if it was registered.
function ui.remove(name: string): boolean
	error("type stub")
end

-- True when the host has a UI layer (it may still be switched off: see ui.onFrame's result).
function ui.available(): boolean
	error("type stub")
end

-- Runs fn as a one-shot script on the next tick, where natives work and fn may wait.
function ui.defer(fn: () -> ()): ()
	error("type stub")
end

-- A window: fn draws its contents when they are visible. Returns (drawn, open).
function ui.window(title: string, fn: () -> (), opts: WindowOptions?): (boolean, boolean)
	error("type stub")
end

-- The unscoped pair: call ui.endWindow() whatever beginWindow returned. Returns (drawn, open).
function ui.beginWindow(title: string, opts: WindowOptions?): (boolean, boolean)
	error("type stub")
end

function ui.endWindow(): ()
	error("type stub")
end

-- Position/size of the next window; always = true applies it every frame, otherwise only on first use.
function ui.setNextWindowPos(x: number, y: number, always: boolean?): ()
	error("type stub")
end

function ui.setNextWindowSize(width: number, height: number, always: boolean?): ()
	error("type stub")
end

-- The arguments through tostring, separated by spaces.
function ui.text(...: any): ()
	error("type stub")
end

function ui.textColored(color: Color, text: string): ()
	error("type stub")
end

-- True on the frame the button was clicked.
function ui.button(label: string): boolean
	error("type stub")
end

-- Returns (value, changed).
function ui.checkbox(label: string, value: boolean): (boolean, boolean)
	error("type stub")
end

-- 32-bit integers; returns (value, changed).
function ui.sliderInt(label: string, value: number, min: number, max: number): (number, boolean)
	error("type stub")
end

-- format: printf-style, default "%.3f"; returns (value, changed).
function ui.sliderFloat(label: string, value: number, min: number, max: number, format: string?): (number, boolean)
	error("type stub")
end

-- maxBytes (default 256, 1..65536) bounds the text; returns (text, changed).
function ui.inputText(label: string, text: string, maxBytes: number?): (string, boolean)
	error("type stub")
end

-- index is 1-based into items; returns (index, changed).
function ui.combo(label: string, index: number, items: { string }): (number, boolean)
	error("type stub")
end

function ui.separator(): ()
	error("type stub")
end

-- The next widget goes on the same line as the previous one.
function ui.sameLine(): ()
	error("type stub")
end

-- A collapsible tree node: fn draws its children while it is open. Returns open.
function ui.tree(label: string, fn: () -> ()): boolean
	error("type stub")
end

-- Unscoped: call ui.treePop() only when treeNode returned true.
function ui.treeNode(label: string): boolean
	error("type stub")
end

function ui.treePop(): ()
	error("type stub")
end

-- A tab bar: fn adds the tabs (ui.tab). Returns visible.
function ui.tabBar(id: string, fn: () -> ()): boolean
	error("type stub")
end

-- A tab inside ui.tabBar: fn draws its contents while it is selected. Returns selected.
function ui.tab(label: string, fn: () -> ()): boolean
	error("type stub")
end

-- Unscoped: endTabBar / endTabItem only when the begin returned true.
function ui.beginTabBar(id: string): boolean
	error("type stub")
end

function ui.endTabBar(): ()
	error("type stub")
end

function ui.beginTabItem(label: string): boolean
	error("type stub")
end

function ui.endTabItem(): ()
	error("type stub")
end

-- Returns (a new color table, changed). opts.noAlpha hides the alpha channel.
function ui.colorEdit(label: string, color: Color, opts: { noAlpha: boolean? }?): (Color, boolean)
	error("type stub")
end

function ui.plotLines(label: string, values: { number }, opts: PlotOptions?): ()
	error("type stub")
end

-- The game's mission objective line (natives, so call from a script, not a frame): SET(text) and PRINT(ms) now,
-- CLEAR after ms of game time (ms default 7500; ms < 0: no timeout, no clear); a later objective replaces the
-- pending clear. UNVERIFIED LIVE (G-087, G-120): static evidence only; the natives are observed, so the mod
-- needs allow_unverified in game.
function ui.objective(text: string, ms: number?): ()
	error("type stub")
end

-- _UILOG_CLEAR_CACHED_OBJECTIVE now; drops a pending timed clear.
function ui.clearObjective(): ()
	error("type stub")
end

return ui
