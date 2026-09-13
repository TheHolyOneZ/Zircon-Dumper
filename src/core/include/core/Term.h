#pragma once

// Terminal colour, decided once and then obeyed everywhere.
//
// Colour is a cue, not decoration: it exists so that the one line that matters -- the
// derived offset, the failing assertion, the count that is lower than it should be -- is
// findable in a screen of log output. That only works if it is used sparingly, so the
// palette here is deliberately small.
//
// Everything degrades to plain text. A pipe, a file, a CI log, NO_COLOR, an old console
// with no virtual-terminal support: in each case Colours() is false, every accessor
// returns "", and the output is exactly what it was before colour existed.

#include <string_view>

namespace zircon::core {

// Turns on the console's virtual-terminal processing if it has any, and decides whether
// colour is wanted at all. Safe and cheap to call more than once; the CLI calls it at
// startup and the injected payload calls it after allocating its console.
void InitTerminal();

// Force colour on or off, whatever the environment says. The CLI's --color/--no-color.
void SetColourEnabled(bool enabled);

// Whether escape sequences are actually being emitted.
bool Colours();

// The palette. Each returns an escape sequence, or "" when colour is off, so call sites
// can concatenate unconditionally and never branch.
namespace term {

std::string_view Reset();
std::string_view Bold();
std::string_view Dim();

std::string_view Red();
std::string_view Green();
std::string_view Yellow();
std::string_view Blue();
std::string_view Cyan();
std::string_view Grey();

} // namespace term
} // namespace zircon::core
