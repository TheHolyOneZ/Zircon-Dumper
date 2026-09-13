#include "Browser.h"

#include "Host.h"
#include "core/Log.h"
#include "emit/Emitter.h"
#include "engine/Kismet.h"
#include "engine/ValueReader.h"
#include "engine/ValueWriter.h"
#include "ir/Json.h"

#include "imgui.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// <windows.h> defines GetClassName as a macro expanding to GetClassNameW, quietly turning
// every engine::GetClassName call into a window-manager one. This file means the engine's.
#undef GetClassName
#undef GetObject

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>

namespace zircon::gui {
namespace {

using core::Address;
using core::IsNull;
using core::Raw;

// Row colour carries what the CLI says in words: real data, unreadable, or a pointer worth
// following.
ImVec4 ValueColour(const std::string& value) {
    if (value.rfind("<unreadable", 0) == 0) return ImVec4(0.85f, 0.35f, 0.35f, 1.0f);
    if (value.rfind('<', 0) == 0)           return ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
    if (value == "nullptr")                 return ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
    if (value == "true")                    return ImVec4(0.45f, 0.80f, 0.45f, 1.0f);
    if (value == "false")                   return ImVec4(0.80f, 0.55f, 0.35f, 1.0f);
    if (value.rfind('/', 0) == 0)           return ImVec4(0.45f, 0.70f, 0.95f, 1.0f);
    return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
}

// Fixed pitch for anything read as a column of numbers. Scoped, so a missing font just
// means the proportional default and nothing worse.
struct MonoScope {
    bool pushed{false};
    MonoScope() {
        if (ImFont* font = MonoFont()) {
            ImGui::PushFont(font);
            pushed = true;
        }
    }
    ~MonoScope() { if (pushed) ImGui::PopFont(); }

    MonoScope(const MonoScope&)            = delete;
    MonoScope& operator=(const MonoScope&) = delete;
};

// A folder beside whichever binary is hosting the browser. Never beside the game: those
// directories are routinely read-only, and a write that fails quietly is worse than one
// that fails loudly.
std::string DefaultOutputDirectory() {
    wchar_t buffer[MAX_PATH * 2] = {};
    if (::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer))) == 0)
        return "zircon-out";

    std::error_code ec;
    const auto directory = std::filesystem::path(buffer).parent_path() / "zircon-out";
    return directory.string();
}

// The executable's own name. A pid identifies the process to the machine, not to whoever
// is reading the window.
std::string ModuleName(core::IMemorySource& memory) {
    const auto* module = memory.MainModule();
    return module ? module->name : std::string{};
}

// "{:04X}  " - the offset prefixing each decompiled line, and the width the panel dims.
// Kept beside the format string that produces it.
constexpr std::size_t kScriptOffsetWidth = 6;

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](char a, char b) {
                                    return std::tolower(static_cast<unsigned char>(a)) ==
                                           std::tolower(static_cast<unsigned char>(b));
                                });
    return it != haystack.end();
}

} // namespace

Browser::Browser() {
    const auto directory = DefaultOutputDirectory();
    std::snprintf(dump_dir_, sizeof(dump_dir_), "%s", directory.c_str());

    // What someone who clicked "Dump" almost certainly wanted: SDK and mappings, plus the
    // IR so the CLI can render anything else later without re-reading the game. The rest
    // are one tick away.
    const auto& emitters = emit::Emitters();
    dump_formats_.assign(emitters.size(), 0);
    for (std::size_t i = 0; i < emitters.size(); ++i) {
        const auto name = emitters[i].name;
        dump_formats_[i] = (name == "cpp_sdk" || name == "usmap" || name == "json") ? 1 : 0;
    }
}

Browser::~Browser() { JoinDump(); }

void Browser::Draw() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGui::Begin("Zircon", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        ImGui::TextColored(ImVec4(0.31f, 0.60f, 0.95f, 1.0f), "Zircon");
        ImGui::Separator();
        if (attached_) {
            ImGui::TextUnformatted(target_name_.empty() ? "attached" : target_name_.c_str());
            ImGui::TextDisabled("pid %u", attached_pid_);
            ImGui::Separator();

            ImGui::BeginDisabled(dumping_);
            if (ImGui::SmallButton("Dump...")) dump_panel_open_ = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("Detach")) Detach();
            ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("not attached");
        }

        // Right-aligned, so the one destructive control isn't where a click lands by
        // accident on the way to the tabs.
        const float quit_width = ImGui::CalcTextSize("Quit").x + ImGui::GetStyle().FramePadding.x * 4.0f;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - quit_width);
        if (ImGui::SmallButton("Quit")) exit_requested_ = true;
        ImGui::EndMenuBar();
    }

    if (!attached_) {
        DrawAttachPanel();
        ImGui::End();
        return;
    }

    DrawStatusPanel();

    const float list_width = ImGui::GetContentRegionAvail().x * 0.42f;
    ImGui::BeginChild("objects", ImVec2(list_width, 0), ImGuiChildFlags_Border);
    DrawObjectList();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("detail", ImVec2(0, 0), ImGuiChildFlags_Border);
    if (ImGui::BeginTabBar("detail_tabs")) {
        if (ImGui::BeginTabItem("Properties")) { DrawInspector(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Script"))     { DrawScriptPanel(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    DrawDumpPanel();
    ApplyFrozen();

    ImGui::End();
}

void Browser::DrawAttachPanel() {
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.89f, 0.90f, 0.93f, 1.0f));
    ImGui::TextUnformatted("Attach to a running Unreal Engine process");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Scored by what is in the process, not by a list of known games. "
                        "Hover a row for the evidence.");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // Rescanning per frame opens and closes a handle to every process on the machine sixty
    // times a second.
    const double now = ImGui::GetTime();
    if (candidates_refreshed_at_ < 0.0 || now - candidates_refreshed_at_ > 2.0) {
        candidates_ = engine::DetectUnrealProcesses(0.2f);
        candidates_refreshed_at_ = now;
    }

    if (candidates_.empty()) {
        ImGui::TextDisabled("No Unreal Engine processes detected. Start a game.");
    } else if (ImGui::BeginTable("procs", 5,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Score",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("PID",     ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthFixed, 340.0f);
        ImGui::TableSetupColumn("Project", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        for (const auto& candidate : candidates_) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float score = candidate.confidence;
            const ImVec4 score_colour = score >= 0.75f ? ImVec4(0.45f, 0.80f, 0.45f, 1.0f)
                                      : score >= 0.40f ? ImVec4(0.85f, 0.72f, 0.35f, 1.0f)
                                                       : ImVec4(0.60f, 0.60f, 0.65f, 1.0f);
            ImGui::TextColored(score_colour, "%.0f%%", score * 100.0f);
            ImGui::TableNextColumn();
            {
                MonoScope mono;
                ImGui::TextDisabled("%u", candidate.process.pid);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(candidate.process.name.c_str());
            if (ImGui::IsItemHovered() && !candidate.evidence.empty()) {
                ImGui::BeginTooltip();
                for (const auto& reason : candidate.evidence)
                    ImGui::BulletText("%s", reason.c_str());
                ImGui::EndTooltip();
            }

            // Recovered from the executable. Separates the game from the three helper
            // processes every launcher spawns.
            ImGui::TableNextColumn();
            if (candidate.project.empty()) ImGui::TextDisabled("-");
            else                           ImGui::TextUnformatted(candidate.project.c_str());

            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(candidate.process.pid));
            if (ImGui::SmallButton("Attach")) Attach(candidate.process.pid);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!attach_error_.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "%s", attach_error_.c_str());
    }
    if (!status_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", status_.c_str());
    }
}

void Browser::DrawStatusPanel() {
    const auto& profile = reflection_.profile;
    const auto& array   = reflection_.array;

    // Each fact labelled. Every number here was derived, not configured, and the
    // user should be able to sanity-check the lot at a glance.
    const auto stat = [](const char* label, const std::string& value) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextUnformatted(value.c_str());
        ImGui::SameLine(0.0f, 18.0f);
    };

    stat("engine", std::format("{} ({:.0f}%)", profile.VersionString(),
                               profile.confidence * 100.0f));
    stat("objects", std::format("{}", array.num_elements));
    stat("indexed", std::format("{}", entries_.size()));

    ImGui::BeginDisabled(dumping_);
    if (ImGui::SmallButton("Reindex")) RebuildIndex();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Pause", &paused_);

    // Writing is opt-in every session and the switch goes all the way down to the
    // provider. Until it is on, IMemorySource::Write refuses.
    ImGui::SameLine();
    if (ImGui::Checkbox("Allow edits", &writes_enabled_)) {
        if (memory_) writes_enabled_ = memory_->EnableWrites(writes_enabled_);
        CancelEdit();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lets the Value column write back into the game.\n"
                          "Off by default. Numbers, bools and enums only.");
    }
    if (writes_enabled_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.65f, 0.30f, 1.0f), "writes on");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        // Every derived offset, so the UI audits as well as the dump does.
        ImGui::BeginTooltip();
        {
            // The font stack balances per window. A MonoScope living to the end of the
            // enclosing block would pop after EndTooltip and trip ImGui's assert.
            MonoScope mono;
            const auto& ol = reflection_.object_layout;
            const auto& sl = reflection_.struct_layout;
            const auto& pl = reflection_.property_layout;
            ImGui::Text("GObjects   0x%llX", static_cast<unsigned long long>(Raw(array.gobjects)));
            ImGui::Text("FNamePool  0x%llX",
                        static_cast<unsigned long long>(Raw(reflection_.pool.blocks)));
            ImGui::Separator();
            ImGui::Text("UObject  index +0x%X  class +0x%X  name +0x%X  outer +0x%X",
                        ol.index_offset, ol.class_offset, ol.name_offset, ol.outer_offset);
            ImGui::Text("UStruct  super +0x%X  children +0x%X  props +0x%X  size +0x%X",
                        sl.super_struct, sl.children, sl.child_properties, sl.properties_size);
            ImGui::Text("FProperty  next +0x%X  name +0x%X  offset +0x%X",
                        pl.next, pl.name, pl.offset_internal);
        }
        ImGui::EndTooltip();
    }

    ImGui::Separator();
}

void Browser::DrawObjectList() {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "filter objects...", filter_, sizeof(filter_)))
        ApplyFilter();

    if (ImGui::Checkbox("functions only", &functions_only_)) ApplyFilter();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu shown", filtered_.size(), entries_.size());

    ImGui::Separator();

    ImGui::BeginChild("list");
    // Clipped. Drawing 70000 selectables a frame costs more than reading the process.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(filtered_.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const std::size_t entry_index = filtered_[static_cast<std::size_t>(i)];
            const Entry& entry = entries_[entry_index];

            ImGui::PushID(i);
            const bool selected = selected_ == entry_index;
            if (ImGui::Selectable(entry.path.c_str(), selected)) {
                selected_ = entry_index;
                script_target_.clear();
                RefreshRows(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(entry.class_name.c_str());
                {
                    MonoScope mono;
                    ImGui::TextDisabled("0x%llX   slot %d",
                                        static_cast<unsigned long long>(Raw(entry.address)),
                                        entry.index);
                }
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void Browser::DrawInspector() {
    // One reader at a time. While the worker walks the object graph this panel reads
    // nothing, instead of racing it through the same page cache.
    if (dumping_) {
        ImGui::TextDisabled("Reading the target for the dump; values resume when it "
                            "finishes.");
        return;
    }

    if (selected_ >= entries_.size()) {
        ImGui::TextDisabled("Select an object.");
        return;
    }

    const Entry& entry = entries_[selected_];
    ImGui::TextWrapped("%s", entry.path.c_str());
    ImGui::TextDisabled("%s", entry.class_name.c_str());
    ImGui::SameLine(0.0f, 10.0f);
    {
        MonoScope mono;
        ImGui::TextDisabled("0x%llX",
                            static_cast<unsigned long long>(Raw(entry.address)));
    }
    if (reading_defaults_) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.60f, 0.75f, 0.95f, 1.0f), "  defaults @ 0x%llX",
                           static_cast<unsigned long long>(Raw(read_from_)));
    }

    if (ImGui::Checkbox("inherited", &show_inherited_)) RefreshRows(true);
    ImGui::SameLine();
    if (ImGui::Checkbox("hex", &hex_integers_)) RefreshRows(true);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("refresh", &refresh_interval_, 0.05f, 2.0f, "%.2f s");
    ImGui::Separator();

    RefreshRows(false);

    if (rows_.empty()) {
        if (entry.is_class && !reading_defaults_)
            ImGui::TextDisabled("This class has no default object, so it has no values to "
                                "show. Its layout is still in the dump.");
        else
            ImGui::TextDisabled("No properties.");
        return;
    }

    if (ImGui::BeginTable("props", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthFixed, 210.0f);
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::string current_owner;
        for (const auto& row : rows_) {
            if (show_inherited_ && row.owner != current_owner) {
                current_owner = row.owner;

                // Which class declares the properties below. Name column, because it's the
                // widest and because the class then sits directly above the members it
                // owns. A full object path fits no column at all, so the leaf shows and the
                // path stays on hover.
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(2);

                const auto dot = current_owner.find_last_of("./");
                const char* leaf = dot == std::string::npos
                                     ? current_owner.c_str()
                                     : current_owner.c_str() + dot + 1;

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.60f, 0.95f, 1.0f));
                ImGui::TextUnformatted(leaf);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", current_owner.c_str());
            }

            ImGui::TableNextRow();

            const std::size_t row_index = static_cast<std::size_t>(&row - rows_.data());
            const bool frozen = IsFrozen(read_from_, row.field);

            ImGui::TableNextColumn();
            {
                MonoScope mono;
                ImGui::TextDisabled("0x%04X", row.offset);
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.55f, 0.72f, 0.60f, 1.0f), "%s", row.type.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableNextColumn();
            if (writes_enabled_ && row.writable) {
                // A padlock would need a font carrying one. An asterisk is legible at any
                // size and in any face, and it sits with the value it holds instead of in
                // a column of its own.
                ImGui::PushID(static_cast<int>(row_index));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      frozen ? ImVec4(0.45f, 0.75f, 0.95f, 1.0f)
                                             : ImVec4(0.34f, 0.36f, 0.41f, 1.0f));
                if (ImGui::SmallButton(frozen ? "*" : "\u00b7")) ToggleFreeze(row_index);
                ImGui::PopStyleColor();
                ImGui::PopID();

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(frozen ? "Frozen. Click to release."
                                             : "Hold this value against the game.");
                ImGui::SameLine(0.0f, 6.0f);
            }
            {
                MonoScope mono;
                const std::size_t index = row_index;

                if (editing_row_ == index) {
                    ImGui::SetNextItemWidth(-1.0f);
                    if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();

                    const bool done = ImGui::InputText(
                        "##edit", edit_buffer_, sizeof(edit_buffer_),
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);

                    if (done) CommitEdit();
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) CancelEdit();
                } else if (frozen) {
                    ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.95f, 1.0f), "%s",
                                       row.value.c_str());
                } else if (writes_enabled_ && row.writable) {
                    // A Selectable so the whole cell responds, and so hovering shows the
                    // row is editable. A Text item is only as wide as its own glyphs, and
                    // clicking the empty space past a short number would do nothing.
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::PushStyleColor(ImGuiCol_Text, ValueColour(row.value));
                    if (ImGui::Selectable(row.value.c_str(), false)) BeginEdit(index);
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                } else {
                    ImGui::TextColored(ValueColour(row.value), "%s", row.value.c_str());
                }
            }
        }
        ImGui::EndTable();
    }

    DrawFrozenPanel();

    if (!edit_error_.empty()) {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "%s", edit_error_.c_str());
    } else if (!last_write_.empty()) {
        ImGui::TextDisabled("wrote %s", last_write_.c_str());
    }
}

void Browser::DrawScriptPanel() {
    if (dumping_) {
        ImGui::TextDisabled("Reading the target for the dump.");
        return;
    }

    if (selected_ >= entries_.size()) {
        ImGui::TextDisabled("Select an object.");
        return;
    }

    const Entry& entry = entries_[selected_];
    if (!entry.is_function) {
        ImGui::TextDisabled("Not a UFunction. Tick 'functions only' in the list to find one.");
        return;
    }

    if (script_target_ != entry.path) {
        script_target_ = entry.path;
        script_lines_.clear();
        script_complete_ = true;
        script_stop_reason_.clear();

        if (script_layout_.Valid()) {
            const auto decompiled = engine::DecompileFunction(
                reflection_.Context(), script_layout_, entry.address);
            script_complete_    = decompiled.complete;
            script_stop_reason_ = decompiled.stop_reason;
            for (const auto& line : decompiled.lines)
                script_lines_.push_back(std::format(
                    "{:04X}  {}{}", line.offset,
                    std::string(static_cast<std::size_t>(line.depth) * 2, ' '), line.text));
        }
    }

    if (script_lines_.empty()) {
        ImGui::TextDisabled("No bytecode (native function).");
        return;
    }

    if (!script_complete_) {
        ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.35f, 1.0f),
                           "incomplete: %s", script_stop_reason_.c_str());
        ImGui::Separator();
    }

    // Fixed pitch is not cosmetic here: the nesting is expressed as leading spaces, so in
    // a proportional face the block structure of the decompiled script disappears.
    ImGui::BeginChild("scriptlines");
    {
        MonoScope mono;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(script_lines_.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& line = script_lines_[static_cast<std::size_t>(i)];

                // Each line starts with its four-digit bytecode offset. Dimming it keeps
                // the addresses available without letting them compete with the code.
                if (line.size() > kScriptOffsetWidth) {
                    ImGui::TextDisabled("%.*s", static_cast<int>(kScriptOffsetWidth),
                                        line.c_str());
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextUnformatted(line.c_str() + kScriptOffsetWidth);
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
        }
    }
    ImGui::EndChild();
}

// --- dumping --------------------------------------------------------------------------

void Browser::DrawDumpPanel() {
    if (dump_panel_open_) {
        ImGui::OpenPopup("Dump this target");
        dump_panel_open_ = false;
    }

    // Centred, and modal only in the sense that it takes focus: the dump runs on its own
    // thread, so the window behind stays responsive and repaints while it works.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->GetCenter().x, viewport->GetCenter().y),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Dump this target", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::TextDisabled("Everything the CLI's 'dump' and 'emit' do, from here.");
    ImGui::Spacing();

    ImGui::BeginDisabled(dumping_);

    ImGui::SeparatorText("Include");
    ImGui::Checkbox("Blueprint bytecode", &dump_script_);
    ImGui::SameLine(220.0f);
    ImGui::Checkbox("Default values", &dump_defaults_);
    ImGui::SameLine(420.0f);
    ImGui::Checkbox("Name pool", &dump_names_);
    ImGui::TextDisabled("Each costs a second or two; the name pool mostly costs file size.");

    ImGui::SeparatorText("Formats");
    const auto& emitters = emit::Emitters();
    if (dump_formats_.size() != emitters.size()) dump_formats_.assign(emitters.size(), 0);

    for (std::size_t i = 0; i < emitters.size(); ++i) {
        if (i % 2 != 0) ImGui::SameLine(280.0f);

        bool on = dump_formats_[i] != 0;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Checkbox(std::string(emitters[i].name).c_str(), &on))
            dump_formats_[i] = on ? 1 : 0;
        ImGui::PopID();

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", std::string(emitters[i].description).c_str());
    }

    ImGui::SeparatorText("Output");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##outdir", dump_dir_, sizeof(dump_dir_));
    ImGui::TextDisabled("One folder per format underneath. Never written next to the game.");

    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (dumping_) {
        std::string status;
        {
            std::lock_guard lock(dump_mutex_);
            status = dump_status_;
        }
        ImGui::TextColored(ImVec4(0.31f, 0.60f, 0.95f, 1.0f), "%s", status.c_str());
    } else {
        const bool any_format = std::any_of(dump_formats_.begin(), dump_formats_.end(),
                                            [](char c) { return c != 0; });
        ImGui::BeginDisabled(!any_format);
        if (ImGui::Button("Dump", ImVec2(120.0f, 0.0f))) StartDump();
        ImGui::EndDisabled();
        if (!any_format) {
            ImGui::SameLine();
            ImGui::TextDisabled("pick at least one format");
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) ImGui::CloseCurrentPopup();
    }

    // What happened, kept after the run so the popup can be reopened to see it again.
    std::vector<std::string> log;
    {
        std::lock_guard lock(dump_mutex_);
        log = dump_log_;
    }
    if (!log.empty()) {
        ImGui::Spacing();
        ImGui::BeginChild("dumplog", ImVec2(0.0f, 140.0f), ImGuiChildFlags_Border);
        {
            // Scoped inside the child: ImGui's font stack has to balance within the
            // window that pushed it, and a MonoScope living to the end of the enclosing
            // block would pop after EndChild.
            MonoScope mono;
            for (const auto& line : log) ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
    }

    ImGui::EndPopup();
}

void Browser::JoinDump() {
    if (dump_thread_.joinable()) dump_thread_.join();
}

void Browser::StartDump() {
    if (dumping_ || !attached_ || !memory_) return;

    // A previous run's thread has finished but not been joined yet.
    JoinDump();

    {
        std::lock_guard lock(dump_mutex_);
        dump_log_.clear();
        dump_status_ = "walking the object graph...";
    }

    engine::BuildOptions options;
    options.include_script   = dump_script_;
    options.include_defaults = dump_defaults_;
    options.include_names    = dump_names_;

    const std::string out_dir{dump_dir_};

    std::vector<std::string> formats;
    const auto& emitters = emit::Emitters();
    for (std::size_t i = 0; i < emitters.size() && i < dump_formats_.size(); ++i)
        if (dump_formats_[i]) formats.emplace_back(emitters[i].name);

    dumping_ = true;

    // Captures by value except the reflection, which points at the memory source this
    // object owns -- so the thread must not outlive the Browser. The destructor joins it.
    dump_thread_ = std::thread([this, options, out_dir, formats] {
        const auto note = [this](std::string line) {
            std::lock_guard lock(dump_mutex_);
            dump_log_.push_back(std::move(line));
        };
        const auto status = [this](std::string line) {
            std::lock_guard lock(dump_mutex_);
            dump_status_ = std::move(line);
        };

        const ir::Dump dump = engine::BuildDump(reflection_, options);
        note(std::format("{} packages, {} classes, {} structs, {} enums, {} properties, "
                         "{} functions",
                         dump.packages.size(), dump.TotalClasses(), dump.TotalStructs(),
                         dump.TotalEnums(), dump.TotalProperties(), dump.TotalFunctions()));

        std::string error;
        if (!emit::util::EnsureDirectory(out_dir, error)) {
            note(std::format("cannot create {}: {}", out_dir, error));
            status("failed");
            dumping_ = false;
            return;
        }

        for (const auto& name : formats) {
            status(std::format("writing {}...", name));

            const auto* emitter = emit::FindEmitter(name);
            if (!emitter) continue;

            emit::EmitOptions emit_options;
            emit_options.out_dir = out_dir + "/" + name;

            const auto result = emitter->emit(dump, emit_options);
            if (!result.ok()) {
                note(std::format("{}: {}", name, result.error));
                continue;
            }
            note(std::format("{}: {} file(s) -> {}", name, result.files.size(),
                             emit_options.out_dir));
        }

        status(std::format("done -> {}", out_dir));
        dumping_ = false;
    });
}

// --- frozen values ---------------------------------------------------------------------

bool Browser::IsFrozen(core::Address object, core::Address field) const {
    return std::any_of(frozen_.begin(), frozen_.end(), [&](const Frozen& entry) {
        return Raw(entry.object) == Raw(object) && Raw(entry.field) == Raw(field);
    });
}

void Browser::ToggleFreeze(std::size_t row_index) {
    if (row_index >= rows_.size()) return;
    const Row& row = rows_[row_index];

    const auto it = std::find_if(frozen_.begin(), frozen_.end(), [&](const Frozen& entry) {
        return Raw(entry.object) == Raw(read_from_) && Raw(entry.field) == Raw(row.field);
    });
    if (it != frozen_.end()) {
        frozen_.erase(it);
        return;
    }

    // Freeze what is on screen. Re-reading here would race the tick that is about to
    // overwrite it, which is the situation freezing exists for.
    Frozen entry;
    entry.object = read_from_;
    entry.field  = row.field;
    entry.value  = row.value;
    entry.label  = std::format("{}::{}", entries_[selected_].path, row.name);
    frozen_.push_back(std::move(entry));
}

void Browser::ApplyFrozen() {
    if (frozen_.empty() || !memory_ || !writes_enabled_ || dumping_) return;

    // Fast enough to beat a 60 Hz tick, slow enough that a dozen frozen values cost
    // nothing. Tying this to the value-refresh slider would let someone set it to two
    // seconds and wonder why freezing stopped working.
    constexpr double kInterval = 0.03;

    const double now = ImGui::GetTime();
    if (last_freeze_ >= 0.0 && now - last_freeze_ < kInterval) return;
    last_freeze_ = now;

    const auto context = reflection_.Context();
    for (auto& entry : frozen_) {
        const auto result =
            engine::WritePropertyValue(context, entry.object, entry.field, entry.value);
        if (result.ok) {
            entry.failures = 0;
            continue;
        }

        // A frozen value that cannot be written any more usually means the object is gone.
        // Say so once instead of retrying silently forever.
        if (++entry.failures == 1)
            core::LogWarn("frozen {}: {}", entry.label, result.error);
    }

    // Anything that has failed for a second or so is not coming back.
    std::erase_if(frozen_, [](const Frozen& entry) { return entry.failures > 30; });
}

void Browser::DrawFrozenPanel() {
    if (frozen_.empty()) return;

    ImGui::Separator();
    ImGui::TextDisabled("frozen");
    ImGui::SameLine();
    if (ImGui::SmallButton("release all")) frozen_.clear();

    for (std::size_t i = 0; i < frozen_.size();) {
        ImGui::PushID(static_cast<int>(i));
        const bool release = ImGui::SmallButton("x");
        ImGui::SameLine();
        {
            MonoScope mono;
            ImGui::TextColored(ImVec4(0.45f, 0.75f, 0.95f, 1.0f), "%s = %s",
                               frozen_[i].label.c_str(), frozen_[i].value.c_str());
        }
        ImGui::PopID();

        if (release) frozen_.erase(frozen_.begin() + static_cast<std::ptrdiff_t>(i));
        else         ++i;
    }
}

// --- editing -------------------------------------------------------------------------

void Browser::BeginEdit(std::size_t row_index) {
    if (row_index >= rows_.size()) return;

    editing_row_ = row_index;
    edit_error_.clear();
    std::snprintf(edit_buffer_, sizeof(edit_buffer_), "%s", rows_[row_index].value.c_str());
}

void Browser::CancelEdit() {
    editing_row_ = static_cast<std::size_t>(-1);
    edit_buffer_[0] = '\0';
}

void Browser::CommitEdit() {
    if (editing_row_ >= rows_.size() || !memory_) {
        CancelEdit();
        return;
    }

    const Row& row = rows_[editing_row_];
    const auto context = reflection_.Context();

    const auto result = engine::WritePropertyValue(context, read_from_, row.field,
                                                   edit_buffer_);
    if (result.ok) {
        core::LogInfo("{} = {} at {:#x}", row.name, result.written,
                      core::Raw(read_from_ + static_cast<std::uint64_t>(row.offset)));
        last_write_ = std::format("{} = {}", row.name, result.written);
        edit_error_.clear();
    } else {
        core::LogWarn("{}: {}", row.name, result.error);
        edit_error_ = std::format("{}: {}", row.name, result.error);
    }

    CancelEdit();

    // Read it back immediately. If the game owns the field and overwrites it next tick,
    // the row showing the old value again is the useful answer.
    RefreshRows(true);
}

// --- state ---------------------------------------------------------------------------

void Browser::Attach(std::uint32_t pid) {
    attach_error_.clear();
    status_ = "attaching...";

    auto source = core::OpenExternalByPid(pid);
    if (!source) {
        attach_error_ = source.error().message;
        status_.clear();
        return;
    }

    memory_ = core::MakeCached(std::move(source.value()));
    reflection_ = engine::Reflect(*memory_);

    if (!reflection_.Valid()) {
        attach_error_ = "attached, but the reflection layout could not be derived";
        memory_.reset();
        status_.clear();
        return;
    }

    script_layout_ = engine::DeriveScriptLayout(*memory_, reflection_.array, reflection_.pool,
                                                 reflection_.object_layout,
                                                 reflection_.struct_layout);
    attached_pid_ = pid;
    attached_     = true;
    target_name_  = ModuleName(*memory_);
    status_.clear();

    RebuildIndex();
}

void Browser::Adopt(std::unique_ptr<core::IMemorySource> memory,
                    const engine::Reflection& reflection) {
    attach_error_.clear();
    status_.clear();

    memory_     = std::move(memory);
    reflection_ = reflection;

    // The caller's Reflection points at the source it derived from. That is the same
    // object we now own — moving a unique_ptr does not move what it points at — but
    // rebinding it keeps the invariant obvious instead of implied.
    reflection_.memory = memory_.get();

    if (!reflection_.Valid()) {
        attach_error_ = "the reflection layout handed over is not usable";
        memory_.reset();
        return;
    }

    script_layout_ = reflection_.script_layout.Valid()
                       ? reflection_.script_layout
                       : engine::DeriveScriptLayout(*memory_, reflection_.array,
                                                    reflection_.pool,
                                                    reflection_.object_layout,
                                                    reflection_.struct_layout);

    attached_pid_ = ::GetCurrentProcessId();
    attached_     = true;
    target_name_  = ModuleName(*memory_);
    RebuildIndex();
}

void Browser::Detach() {
    if (memory_ && writes_enabled_) memory_->EnableWrites(false);
    writes_enabled_ = false;
    CancelEdit();
    edit_error_.clear();
    last_write_.clear();
    target_name_.clear();
    attached_ = false;
    attached_pid_ = 0;
    memory_.reset();
    entries_.clear();
    filtered_.clear();
    rows_.clear();
    script_lines_.clear();
    script_target_.clear();
    selected_ = static_cast<std::size_t>(-1);
    applied_filter_ = "\x01";
}

void Browser::RebuildIndex() {
    entries_.clear();
    if (!attached_) return;

    const auto& array = reflection_.array;
    entries_.reserve(static_cast<std::size_t>(array.num_elements));

    for (std::int32_t i = 0; i < array.num_elements; ++i) {
        const auto object = engine::ObjectAt(*memory_, array, i);
        if (IsNull(object)) continue;

        Entry entry;
        entry.index   = i;
        entry.address = object;
        entry.path    = engine::GetObjectPathName(*memory_, reflection_.object_layout,
                                                   reflection_.pool, object);
        if (entry.path.empty()) continue;

        entry.class_name = engine::GetClassName(*memory_, reflection_.object_layout,
                                                 reflection_.pool, object);
        const auto kind = engine::ClassifyObject(*memory_, reflection_.object_layout,
                                                 reflection_.struct_layout,
                                                 reflection_.pool, object);
        entry.is_function = kind == engine::ObjectKind::Function;
        entry.is_class    = kind == engine::ObjectKind::Class;

        entries_.push_back(std::move(entry));
    }

    selected_ = static_cast<std::size_t>(-1);
    rows_.clear();
    applied_filter_ = "\x01";
    ApplyFilter();
}

void Browser::ApplyFilter() {
    const std::string needle = filter_;
    filtered_.clear();
    filtered_.reserve(entries_.size());

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (functions_only_ && !entries_[i].is_function) continue;
        if (!needle.empty() && !ContainsNoCase(entries_[i].path, needle)) continue;
        filtered_.push_back(i);
    }
    applied_filter_ = needle;
}

void Browser::RefreshRows(bool force) {
    if (selected_ >= entries_.size()) { rows_.clear(); return; }

    const double now = ImGui::GetTime();
    if (!force) {
        if (paused_) return;
        if (last_refresh_ >= 0.0 && now - last_refresh_ < refresh_interval_) return;
    }
    last_refresh_ = now;

    const Entry& entry = entries_[selected_];

    auto context = reflection_.Context();
    engine::ValueFormat format;
    format.hex_integers = hex_integers_;

    // A class is not an instance. Read as one it shows nothing, because the members it
    // declares belong to objects of that class, not to the class object itself. The
    // engine keeps one real instance of every class for exactly this purpose — the class
    // default object — so that is what a class selection reads.
    rows_.clear();
    reading_defaults_ = false;
    read_from_ = entry.address;

    if (entry.is_class) {
        const auto cdo = engine::GetClassDefaultObject(*memory_, reflection_.class_layout,
                                                       entry.address);
        if (IsNull(cdo)) return;      // abstract, or not yet constructed
        read_from_ = cdo;
        reading_defaults_ = true;
    }

    // Walking the class chain is what makes an instance readable: the few properties a
    // leaf class declares are rarely the interesting ones.
    const auto klass = engine::GetObjectClass(*memory_, reflection_.object_layout, read_from_);

    std::size_t guard = 0;
    for (auto current = klass; !IsNull(current) && guard < 64; ++guard) {
        const std::string owner = engine::GetObjectPathName(
            *memory_, reflection_.object_layout, reflection_.pool, current);

        for (const auto field : engine::GetChildProperties(*memory_, reflection_.struct_layout,
                                                            reflection_.property_layout,
                                                            current)) {
            Row row;
            row.owner  = owner;
            row.name   = engine::GetFieldName(*memory_, reflection_.property_layout,
                                               reflection_.pool, field);
            row.offset = engine::GetPropertyOffset(*memory_, reflection_.property_layout, field);
            row.size   = engine::GetElementSize(*memory_, reflection_.property_layout, field);
            row.type   = engine::DescribeType(engine::ResolveType(context, field));
            row.value  = engine::ReadPropertyValue(context, read_from_, field, format);
            row.field    = field;
            row.writable = engine::IsPropertyWritable(context, field);
            rows_.push_back(std::move(row));
        }

        if (!show_inherited_) break;
        current = engine::GetSuperStruct(*memory_, reflection_.struct_layout, current);
    }
}

} // namespace zircon::gui
