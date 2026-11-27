#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace codex_organizer {

inline constexpr wchar_t kAppName[] = L"Codex Window Organizer";
inline constexpr wchar_t kAppVersion[] = L"0.3.0";
inline constexpr int kControlRailWidth = 170;
inline constexpr int kDefaultTerminalTailCharacters = 4096;

struct Rect {
    int left{};
    int top{};
    int right{};
    int bottom{};

    [[nodiscard]] int Width() const noexcept { return right - left; }
    [[nodiscard]] int Height() const noexcept { return bottom - top; }
    [[nodiscard]] bool Contains(const Rect& other) const noexcept;
    [[nodiscard]] std::wstring Xywh() const;
};

struct MonitorInfo {
    std::uintptr_t handle{};
    std::wstring device_name;
    Rect bounds;
    Rect work_area;
    bool primary{};

    [[nodiscard]] std::wstring Label() const;
};

struct ProcessInfo {
    std::uint32_t process_id{};
    std::uint32_t parent_process_id{};
    std::wstring name;
    std::wstring image_path;
    std::uint64_t creation_time_100ns{};
};

struct WindowInfo {
    std::uintptr_t hwnd{};
    std::wstring title;
    std::wstring class_name;
    std::uint32_t process_id{};
    std::wstring process_name;
    std::wstring process_path;
    Rect rect;
    bool minimized{};
    int z_order{};
    bool auto_selected{};
    std::wstring selection_reason{L"candidate"};
    std::wstring thread_name;
    std::wstring thread_id;
    std::wstring thread_identity_source;
    std::wstring codex_status;
    std::wstring codex_model;
    std::wstring codex_used_tokens;
    std::wstring process_uptime;
    std::uint32_t codex_process_id{};

    [[nodiscard]] std::wstring ThreadLabel() const;
};

struct ThreadIdentity {
    std::wstring name;
    std::wstring thread_id;
    std::wstring source;
};

struct CodexStatusLine {
    std::wstring status;
    std::wstring model;
    std::wstring used_tokens;
    std::wstring weekly;
};

struct Config {
    int preferred_width{1129};
    int preferred_height{635};
    int preferred_columns{2};
    int minimum_width{420};
    int minimum_height{190};
    int gap{8};
    int outer_margin{8};
    std::wstring monitor{L"primary"};
    std::vector<std::wstring> terminal_class_names{
        L"CASCADIA_HOSTING_WINDOW_CLASS", L"ConsoleWindowClass"};
    std::vector<std::wstring> title_include_patterns{L"codex"};
    std::vector<std::wstring> title_exclude_patterns{L"codex lb", L"visual studio code"};
    std::vector<std::wstring> codex_process_names{L"codex.exe"};
    std::vector<std::wstring> codex_path_exclude_patterns{L"\\.vscode\\extensions\\"};
    bool organize_by_thread_name{true};
    bool unnamed_threads_last{true};
    int terminal_text_tail_characters{kDefaultTerminalTailCharacters};
    std::wstring session_index_path;
    bool request_account_usage{true};
    bool restore_minimized{true};
    bool bring_to_front{true};
    bool minimize_after_arrange{true};
    bool confirm_before_move{false};
};

struct AssociationResult {
    std::vector<WindowInfo> windows;
    std::wstring mode;
    std::wstring warning;
};

struct DetectionResult {
    std::vector<WindowInfo> windows;
    std::vector<ProcessInfo> standalone_codex_processes;
    std::wstring association_mode;
    std::wstring warning;
    std::wstring thread_warning;

    [[nodiscard]] std::vector<WindowInfo> AutoWindows() const;
    [[nodiscard]] std::size_t NamedThreadCount() const noexcept;
};

struct LayoutPlan {
    int columns{};
    int rows{};
    Rect work_area;
    std::vector<Rect> rectangles;
};

struct MoveResult {
    std::uintptr_t hwnd{};
    Rect requested;
    std::optional<Rect> actual;
    std::wstring error;
    std::wstring warning;
};

[[nodiscard]] std::wstring ToLower(std::wstring value);
[[nodiscard]] bool MatchesAny(const std::wstring& value,
                              const std::vector<std::wstring>& patterns);

[[nodiscard]] Config LoadConfig(const std::filesystem::path& path);
[[nodiscard]] int ChooseColumnCount(int count, const Rect& work_area, const Config& config);
[[nodiscard]] LayoutPlan CalculateLayout(int count, const Rect& work_area, const Config& config);

[[nodiscard]] AssociationResult AssociateCodexWindows(
    std::vector<WindowInfo> candidates,
    std::size_t standalone_process_count,
    const std::vector<std::wstring>& include_patterns,
    const std::vector<std::wstring>& exclude_patterns);

[[nodiscard]] std::filesystem::path CodexHome();
[[nodiscard]] std::map<std::wstring, std::wstring> LoadThreadNameIndex(
    const std::optional<std::filesystem::path>& path = std::nullopt);
[[nodiscard]] ThreadIdentity ParseThreadIdentity(
    const std::wstring& terminal_tail,
    const std::map<std::wstring, std::wstring>& names_by_id = {});
[[nodiscard]] CodexStatusLine ParseCodexStatusLine(const std::wstring& terminal_tail);
[[nodiscard]] std::wstring ParseCodexWeeklyUsageResponse(const std::string& response_body);
[[nodiscard]] std::wstring FetchCodexWeeklyUsage();
void ApplyCodexProcessUptimes(std::vector<WindowInfo>& windows,
    const std::vector<ProcessInfo>& processes,
    const std::map<std::uint32_t, std::uintptr_t>& console_windows,
    std::uint64_t now_100ns);

[[nodiscard]] std::wstring FormatUptime(std::uint64_t elapsed_seconds);
[[nodiscard]] bool ThreadLess(const WindowInfo& first,
                              const WindowInfo& second,
                              bool unnamed_last = true);
void SortWindowsByThread(std::vector<WindowInfo>& windows, bool unnamed_last = true);

[[nodiscard]] std::wstring FormatPlan(const std::vector<WindowInfo>& windows,
                                      const MonitorInfo& monitor,
                                      const LayoutPlan& plan);

[[nodiscard]] std::wstring Utf8ToWide(const std::string& value);
[[nodiscard]] std::string WideToUtf8(const std::wstring& value);

}  // namespace codex_organizer
