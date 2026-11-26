--!strict
-- Type stub of the host module @gta6/native (sdk/luau/host/g6_luau.cpp, l_require). The host never
-- loads this file; sdk/luau/analyze.cmake gives it to luau-analyze in place of the C functions.
-- Keep it in step with l_native_resolve, l_native_call and l_native_joaat.

-- Opaque at runtime (tagged userdata holding hash, signature and the resolved handler).
export type NativeRef = { _g6NativeRef: never }

local native = {}

-- hashHex: 16 hex digits; sig: "<args>><ret>" codes ("" = raw u64 wrapper); status "verified" or not.
function native.resolve(hashHex: string, sig: string, name: string?, status: string?): NativeRef
	error("type stub")
end

-- Arguments and results follow the signature the ref was resolved with (see natives.luau).
function native.call(ref: NativeRef, ...: any): ...any
	error("type stub")
end

function native.joaat(s: string): number
	error("type stub")
end

return native
