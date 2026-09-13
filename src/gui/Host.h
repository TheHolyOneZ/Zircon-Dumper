#pragma once

// A window, a D3D11 device, and a frame loop for the Browser.
//
// Split out of the standalone executable so the injected payload opens the same UI without
// duplicating any of it. Both shells call one of these two functions and get the identical
// browser. The payload does not draw over the game's own frames; that means hooking the
// swap chain, which writes to code the game owns. See docs/SCOPE.md.

#include "core/MemorySource.h"
#include "engine/DumpBuilder.h"

#include <cstdint>
#include <memory>

struct ImFont;

namespace zircon::gui {

// Fixed-pitch face for the columns that are really numbers: offsets, addresses, decompiled
// script. Null when the host could not load one and callers fall back to the proportional
// default, so guard every use.
ImFont* MonoFont();

// Opens the browser and returns when the window closes. A non-zero pid attaches straight
// away; zero shows the process picker.
int RunBrowserWindow(std::uint32_t attach_pid = 0);

// Same window over a target that is already open, which is what the injected payload has:
// it is inside the process and has derived everything already. The browser takes ownership
// of the source, and `reflection` must refer to that same source.
int RunBrowserWindow(std::unique_ptr<core::IMemorySource> memory,
                     const engine::Reflection& reflection);

} // namespace zircon::gui
