// File logger next to the runtime DLL. Thread-safe, unbuffered (each line is one WriteFile),
// so the log survives a crash of the host.
#pragma once
#include <cstdarg>

namespace g6rt::log {

enum class Level : int { debug = 0, info = 1, warn = 2, error = 3 };

bool open(const wchar_t* path) noexcept;   // truncates; keeps the previous run as <path>.prev
void set_level(Level l) noexcept;
void write(Level l, const char* who, const char* fmt, ...) noexcept;
void vwrite(Level l, const char* who, const char* fmt, va_list ap) noexcept;

} // namespace g6rt::log

#define G6LOG_D(...) ::g6rt::log::write(::g6rt::log::Level::debug, "runtime", __VA_ARGS__)
#define G6LOG_I(...) ::g6rt::log::write(::g6rt::log::Level::info,  "runtime", __VA_ARGS__)
#define G6LOG_W(...) ::g6rt::log::write(::g6rt::log::Level::warn,  "runtime", __VA_ARGS__)
#define G6LOG_E(...) ::g6rt::log::write(::g6rt::log::Level::error, "runtime", __VA_ARGS__)
