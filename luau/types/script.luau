--!strict
-- Type stub of the host module @gta6/script (sdk/luau/host/g6_luau.cpp, l_require). The host never
-- loads this file; sdk/luau/analyze.cmake gives it to luau-analyze in place of the C functions.
-- Keep it in step with l_script_register, l_script_wait, l_script_now, l_script_log, l_script_cancel,
-- l_script_on_stop.

local script = {}

-- Errors if a live script already has this name.
function script.register(name: string, fn: () -> ()): ()
	error("type stub")
end

-- Yields the calling script for ms of game time (negative = 0). Resumed with no values.
function script.wait(ms: number?): ()
	error("type stub")
end

-- Game time in ms.
function script.now(): number
	error("type stub")
end

-- level: "debug" | "info" | "warn" | "error" (anything else logs as info); source defaults to the script.
function script.log(level: string, msg: string, source: string?): ()
	error("type stub")
end

-- True if a live script with this name was found and cancelled.
function script.cancel(name: string): boolean
	error("type stub")
end

-- fn runs once when the host raises the stop event (g6_luau_stop). Errors after the event was raised.
function script.onStop(fn: () -> ()): ()
	error("type stub")
end

return script
