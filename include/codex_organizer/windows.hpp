#pragma once

#include "codex_organizer/core.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace codex_organizer {

int RunConsoleWindowProbe(const std::vector<std::uint32_t>& process_ids);

void EnableDpiAwareness() noexcept;
[[nodiscard]] std::vector<MonitorInfo> EnumerateMonitors();
[[nodiscard]] const MonitorInfo& ResolveMonitor(const std::vector<MonitorInfo>& monitors,
                                                const std::wstring& selector);

[[nodiscard]] std::map<std::uint32_t, ProcessInfo> EnumerateProcesses();
[[nodiscard]] std::vector<ProcessInfo> StandaloneCodexProcesses(
    const std::map<std::uint32_t, ProcessInfo>& processes,
    const Config& config);
[[nodiscard]] std::vector<WindowInfo> EnumerateTerminalWindows(
    const std::map<std::uint32_t, ProcessInfo>& processes,
    const Config& config);

[[nodiscard]] std::pair<std::wstring, std::wstring> ReadTerminalTextTail(
    std::uintptr_t hwnd,
    int character_count);
using TerminalTextReader = std::function<std::pair<std::wstring, std::wstring>(std::uintptr_t, int)>;
[[nodiscard]] DetectionResult DetectCodexWindows(
    const Config& config,
    bool include_all_terminals = false,
    const TerminalTextReader& read_terminal_text = ReadTerminalTextTail);
[[nodiscard]] std::vector<MoveResult> MoveWindows(const std::vector<WindowInfo>& windows,
                                                  const std::vector<Rect>& rectangles,
                                                  const Config& config);

void WriteLog(const std::filesystem::path& base_directory,
              const std::wstring& level,
              const std::wstring& message) noexcept;

int RunGui(void* instance, const Config& config, const std::filesystem::path& base_directory);

}  // namespace codex_organizer
