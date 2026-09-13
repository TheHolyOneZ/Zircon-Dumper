#pragma once

// The live object browser, drawn with ImGui.
//
// Host-agnostic on purpose. It draws into whatever ImGui context is current and knows
// nothing about windows, devices or swap chains, which is what lets the same code run as a
// standalone window attached externally and as an in-game overlay from the injected
// payload. One implementation, two hosts.

#include "core/MemorySource.h"
#include "engine/DumpBuilder.h"
#include "engine/UnrealDetect.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zircon::gui {

class Browser {
public:
    Browser();
    ~Browser();

    // Draws one frame. Safe to call before anything is attached.
    void Draw();

    bool WantsExit() const { return exit_requested_; }

    // Skips the picker, so a host can honour "--attach <pid>". Failures land in the same
    // error line the panel shows, not a message box; the UI stays usable and the
    // picker is right there.
    void AttachTo(std::uint32_t pid) { Attach(pid); }

    // Takes over a target already open and already derived. That is the injected payload's
    // case, where re-deriving repeats several seconds of work it just did. `reflection`
    // must describe the source being handed over.
    void Adopt(std::unique_ptr<core::IMemorySource> memory,
               const engine::Reflection& reflection);

private:
    // Names resolve once at attach. Re-reading 70000 outer chains per frame would make the
    // UI unusable, and the loaded set does not change fast enough to justify it.
    struct Entry {
        std::int32_t   index{};
        core::Address  address{};
        std::string    path;
        std::string    class_name;
        bool           is_function{false};
        bool           is_class{false};
    };

    void DrawAttachPanel();
    void DrawStatusPanel();
    void DrawObjectList();
    void DrawInspector();
    void DrawScriptPanel();
    void DrawDumpPanel();

    // Worker thread. While it runs the UI does not touch the target at all; see dumping_.
    void StartDump();
    void JoinDump();

    void Attach(std::uint32_t pid);
    void Detach();
    void RebuildIndex();
    void ApplyFilter();

    // --- attach state ---
    std::vector<engine::UnrealCandidate> candidates_;
    double                               candidates_refreshed_at_{-1.0};
    std::uint32_t                        attached_pid_{0};
    std::string                          target_name_;   // the executable, for the title bar
    std::string                          attach_error_;
    std::string                          status_;

    std::unique_ptr<core::IMemorySource> memory_;
    engine::Reflection                   reflection_;
    engine::ScriptLayout                 script_layout_;
    bool                                 attached_{false};

    // --- object index ---
    std::vector<Entry>        entries_;
    std::vector<std::size_t>  filtered_;
    char                      filter_[128]{};
    std::string               applied_filter_{"\x01"};   // forces the first filter pass
    bool                      functions_only_{false};

    std::size_t selected_{static_cast<std::size_t>(-1)};

    // --- inspector ---
    bool  show_inherited_{true};
    bool  hex_integers_{false};
    bool  paused_{false};

    // Timer, not every frame. A deep struct member costs many reads, and at 60 fps that is
    // thousands of syscalls a second for data nobody can read that fast.
    float  refresh_interval_{0.25f};
    double last_refresh_{-1.0};

    struct Row {
        std::string name;
        std::string type;
        std::string value;
        std::int32_t offset{};
        std::int32_t size{};
        std::string owner;      // the class that declares it, for the inherited grouping
    };
    std::vector<Row> rows_;
    void RefreshRows(bool force);

    // What the rows were actually read from. For a class that is its default object, so the
    // header can say so instead of implying a live instance.
    core::Address read_from_{};
    bool          reading_defaults_{false};

    // --- script ---
    std::string              script_target_;
    std::vector<std::string> script_lines_;
    bool                     script_complete_{true};
    std::string              script_stop_reason_;

    // --- dumping ---
    //
    // Its own thread: walking seventy thousand objects takes seconds, and a frozen window
    // looks like a crash.
    //
    // None of this locks the memory source. While `dumping_` is set the UI reads the target
    // exactly zero times: the inspector and script panel say so and return, Reindex is
    // disabled. One reader at a time, enforced by not having a second one, which is easier
    // to be sure of than a mutex around every read.
    std::thread              dump_thread_;
    std::atomic<bool>        dumping_{false};
    std::mutex               dump_mutex_;      // guards the two fields below
    std::string              dump_status_;
    std::vector<std::string> dump_log_;

    bool  dump_panel_open_{false};
    char  dump_dir_[512]{};
    bool  dump_script_{true};
    bool  dump_defaults_{true};
    bool  dump_names_{false};

    // One flag per registered emitter in the order Emitters() reports them, so a new format
    // shows up here without this file knowing its name.
    std::vector<char> dump_formats_;

    bool exit_requested_{false};
};

} // namespace zircon::gui
