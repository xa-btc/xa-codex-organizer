#include "codex_organizer/windows.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace codex_organizer {
namespace {

struct Arguments {
    std::optional<std::filesystem::path> config;
    std::vector<std::uint32_t> console_probe_pids;
    bool list{};
    bool arrange{};
    std::optional<int> count;
    bool all_terminals{};
    std::optional<std::wstring> monitor;
    bool no_thread_order{};
    bool dry_run{};
    bool version{};
    bool help{};
};

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) {
        throw std::runtime_error("Could not resolve the executable directory");
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

void PrepareConsole() {
    if (!GetConsoleWindow()) AttachConsole(ATTACH_PARENT_PROCESS);
    SetConsoleOutputCP(CP_UTF8);
}

void WriteStream(bool error, const std::wstring& text) {
    PrepareConsole();
    HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        DWORD written = 0;
        WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    const std::string utf8 = WideToUtf8(text);
    DWORD written = 0;
    WriteFile(handle, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

int ParsePositiveInt(const std::wstring& value, const wchar_t* option) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size() || parsed <= 0) throw std::invalid_argument("range");
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument(WideToUtf8(option) + " must be a positive integer");
    }
}

Arguments ParseArguments(int count, wchar_t** values) {
    Arguments arguments;
    for (int index = 1; index < count; ++index) {
        const std::wstring option = values[index];
        const auto next = [&]() -> std::wstring {
            if (++index >= count) {
                throw std::invalid_argument("Missing value after " + WideToUtf8(option));
            }
            return values[index];
        };
        if (option == L"--console-window-pid") {
            if (arguments.console_probe_pids.size() >= 128) throw std::invalid_argument("Too many probe PIDs");
            arguments.console_probe_pids.push_back(static_cast<std::uint32_t>(ParsePositiveInt(next(), L"--console-window-pid")));
        }
        else if (option == L"--config") arguments.config = next();
        else if (option == L"--list") arguments.list = true;
        else if (option == L"--arrange") arguments.arrange = true;
        else if (option == L"--count") arguments.count = ParsePositiveInt(next(), L"--count");
        else if (option == L"--all-terminals") arguments.all_terminals = true;
        else if (option == L"--monitor") arguments.monitor = next();
        else if (option == L"--no-thread-order") arguments.no_thread_order = true;
        else if (option == L"--dry-run") arguments.dry_run = true;
        else if (option == L"--version") arguments.version = true;
        else if (option == L"--help" || option == L"-h" || option == L"/?") arguments.help = true;
        else throw std::invalid_argument("Unknown option: " + WideToUtf8(option));
    }
    return arguments;
}

std::wstring HelpText() {
    return std::wstring(kAppName) + L" " + kAppVersion +
           L"\n\nUsage:\n"
           L"  CodexOrganizer.exe                         Open the GUI\n"
           L"  CodexOrganizer.exe --list                  List candidates\n"
           L"  CodexOrganizer.exe --count 8 --dry-run     Preview a layout\n"
           L"  CodexOrganizer.exe --arrange               Arrange auto-selected windows\n\n"
           L"Options:\n"
           L"  --config PATH        Use another config.json\n"
           L"  --all-terminals      Explicitly include every terminal candidate\n"
           L"  --monitor VALUE      primary, 1-based number, or display name\n"
           L"  --no-thread-order    Preserve Windows z-order\n"
           L"  --version            Print the application version\n";
}

void PrintDetection(const DetectionResult& detection) {
    std::wstring output =
        L"Standalone Codex processes: " +
        std::to_wstring(detection.standalone_codex_processes.size()) +
        L"\nCandidate terminal windows: " + std::to_wstring(detection.windows.size()) +
        L"\nAssociation: " + detection.association_mode +
        L"\nNamed threads: " + std::to_wstring(detection.NamedThreadCount()) + L"\n";
    if (!detection.warning.empty()) output += L"Warning: " + detection.warning + L"\n";
    if (!detection.thread_warning.empty()) {
        output += L"Thread warning: " + detection.thread_warning + L"\n";
    }
    for (std::size_t index = 0; index < detection.windows.size(); ++index) {
        const WindowInfo& window = detection.windows[index];
        output += window.auto_selected ? L"* " : L"  ";
        output += std::to_wstring(index + 1) + L". HWND " + std::to_wstring(window.hwnd) +
                  L"  " + window.class_name + L"  " + window.rect.Xywh() + L"  " +
                  window.ThreadLabel() + L" [" + window.selection_reason + L"]" +
                  L"  Codex PID " + std::to_wstring(window.codex_process_id) +
                  L"  Uptime " + window.process_uptime + L"\n";
    }
    WriteStream(false, output);
}

int RunApplication(HINSTANCE instance, const Arguments& arguments,
                   const std::filesystem::path& base_directory) {
    if (!arguments.console_probe_pids.empty()) return RunConsoleWindowProbe(arguments.console_probe_pids);
    if (arguments.help) {
        WriteStream(false, HelpText());
        return 0;
    }
    if (arguments.version) {
        WriteStream(false, std::wstring(kAppName) + L" " + kAppVersion + L"\n");
        return 0;
    }

    std::filesystem::path config_path = arguments.config.value_or(base_directory / L"config.json");
    if (arguments.config && config_path.is_relative()) config_path = std::filesystem::absolute(config_path);
    Config config = LoadConfig(config_path);
    if (arguments.monitor) config.monitor = *arguments.monitor;
    if (arguments.no_thread_order) config.organize_by_thread_name = false;

    const bool command_requested = arguments.arrange || arguments.count.has_value() ||
                                   arguments.dry_run;
    if (!arguments.list && !command_requested) {
        return RunGui(instance, config, base_directory);
    }

    DetectionResult detection = DetectCodexWindows(config, arguments.all_terminals);
    if (arguments.list) {
        PrintDetection(detection);
        if (!command_requested) return 0;
    }

    std::vector<WindowInfo> pool = arguments.all_terminals ? detection.windows
                                                            : detection.AutoWindows();
    if (pool.empty()) {
        WriteStream(true,
                    L"No Codex windows were identified. Refresh with the Codex tab visible, "
                    L"or explicitly pass --all-terminals.\n");
        return 2;
    }
    const std::size_t requested = arguments.count ? static_cast<std::size_t>(*arguments.count)
                                                   : pool.size();
    if (requested > pool.size()) {
        WriteStream(true, L"Requested " + std::to_wstring(requested) +
                              L" windows, but only " + std::to_wstring(pool.size()) +
                              L" are selected.\n");
        return 2;
    }
    pool.resize(requested);
    const auto monitors = EnumerateMonitors();
    const MonitorInfo& monitor = ResolveMonitor(monitors, config.monitor);
    const LayoutPlan plan = CalculateLayout(static_cast<int>(pool.size()), monitor.work_area,
                                            config);
    WriteStream(false, FormatPlan(pool, monitor, plan) + L"\n");
    if (arguments.dry_run) return 0;

    const auto results = MoveWindows(pool, plan.rectangles, config);
    std::size_t failures = 0;
    std::size_t warnings = 0;
    for (const MoveResult& result : results) {
        if (!result.error.empty()) {
            ++failures;
            WriteStream(true, L"HWND " + std::to_wstring(result.hwnd) + L": " +
                                  result.error + L"\n");
        } else if (!result.warning.empty()) {
            ++warnings;
            WriteStream(true, L"HWND " + std::to_wstring(result.hwnd) + L": " +
                                  result.warning + L"\n");
        }
    }
    WriteLog(base_directory, L"INFO",
             L"CLI arranged " + std::to_wstring(pool.size()) + L" windows on " +
                 monitor.device_name + L": " + std::to_wstring(failures) + L" failures, " +
                 std::to_wstring(warnings) + L" warnings");
    return failures ? 1 : 0;
}

}  // namespace
}  // namespace codex_organizer

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace codex_organizer;
    const std::filesystem::path base_directory = [&] {
        try {
            return ExecutableDirectory();
        } catch (const std::exception&) {
            return std::filesystem::current_path();
        }
    }();
    int argument_count = 0;
    wchar_t** argument_values = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!argument_values) return 1;
    int result = 1;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        const Arguments arguments = ParseArguments(argument_count, argument_values);
        EnableDpiAwareness();
        result = RunApplication(instance, arguments, base_directory);
    } catch (const std::exception& error) {
        const std::wstring message = Utf8ToWide(error.what());
        WriteLog(base_directory, L"CRITICAL", message);
        if (argument_count > 1) {
            WriteStream(true, L"Codex Organizer error: " + message + L"\n");
        } else {
            MessageBoxW(nullptr, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
        }
        result = 1;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    LocalFree(argument_values);
    return result;
}
