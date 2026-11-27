#include "codex_organizer/windows.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <tlhelp32.h>
#include <UIAutomation.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace codex_organizer {
namespace {

template <typename Interface>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    Interface* Get() const noexcept { return value_; }
    Interface** Put() noexcept {
        Reset();
        return &value_;
    }
    Interface* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
    void Reset() noexcept {
        if (value_) value_->Release();
        value_ = nullptr;
    }

private:
    Interface* value_{};
};

std::wstring LastErrorMessage(DWORD error = GetLastError()) {
    if (error == ERROR_SUCCESS) return L"Unknown Windows error";
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length && buffer ? std::wstring(buffer, length)
                                            : L"Windows error " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

std::wstring HResultName(HRESULT result) {
    return L"HRESULT 0x" + [&] {
        std::wostringstream stream;
        stream << std::hex << std::uppercase << static_cast<unsigned long>(result);
        return stream.str();
    }();
}

std::wstring QueryProcessImagePath(DWORD process_id, std::uint64_t& creation_time_100ns) {
    creation_time_100ns = 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return {};
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER value{};
        value.LowPart = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        creation_time_100ns = value.QuadPart;
    }
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!ok) return {};
    path.resize(length);
    return path;
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(std::max(1, length + 1)), L'\0');
    const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(std::max(0, copied)));
    return text;
}

std::wstring WindowClass(HWND window) {
    wchar_t buffer[256]{};
    const int copied = GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
    return copied > 0 ? std::wstring(buffer, static_cast<std::size_t>(copied)) : std::wstring();
}

std::optional<Rect> WindowRect(HWND window) {
    RECT native{};
    if (!GetWindowRect(window, &native)) return std::nullopt;
    return Rect{native.left, native.top, native.right, native.bottom};
}

bool IsCloaked(HWND window) {
    DWORD cloaked = 0;
    const HRESULT result = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                                 sizeof(cloaked));
    return SUCCEEDED(result) && cloaked != 0;
}

std::wstring LocalTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

}  // namespace

void EnableDpiAwareness() noexcept {
    using SetContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (const auto set_context = reinterpret_cast<SetContext>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            set_context && set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    const auto callback = [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
        auto* output = reinterpret_cast<std::vector<MonitorInfo>*>(parameter);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            output->push_back({
                reinterpret_cast<std::uintptr_t>(monitor),
                info.szDevice,
                {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right,
                 info.rcMonitor.bottom},
                {info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom},
                (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
            });
        }
        return TRUE;
    };
    if (!EnumDisplayMonitors(nullptr, nullptr, callback,
                             reinterpret_cast<LPARAM>(&monitors))) {
        throw std::runtime_error("Windows monitor enumeration failed: " +
                                 WideToUtf8(LastErrorMessage()));
    }
    if (monitors.empty()) {
        throw std::runtime_error("Windows did not report any monitors");
    }
    std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& first,
                                                   const MonitorInfo& second) {
        if (first.primary != second.primary) return first.primary;
        return ToLower(first.device_name) < ToLower(second.device_name);
    });
    return monitors;
}

const MonitorInfo& ResolveMonitor(const std::vector<MonitorInfo>& monitors,
                                  const std::wstring& selector) {
    if (monitors.empty()) throw std::invalid_argument("No monitors are available");
    const std::wstring lowered = ToLower(selector);
    if (lowered == L"primary") {
        const auto primary = std::find_if(monitors.begin(), monitors.end(),
                                          [](const MonitorInfo& item) { return item.primary; });
        return primary == monitors.end() ? monitors.front() : *primary;
    }
    if (!selector.empty() &&
        std::all_of(selector.begin(), selector.end(), [](wchar_t character) {
            return std::iswdigit(character) != 0;
        })) {
        const unsigned long number = std::stoul(selector);
        if (number >= 1 && number <= monitors.size()) return monitors[number - 1];
        throw std::invalid_argument("Monitor number must be between 1 and " +
                                    std::to_string(monitors.size()));
    }
    const auto match = std::find_if(monitors.begin(), monitors.end(),
                                    [&](const MonitorInfo& item) {
                                        return ToLower(item.device_name) == lowered;
                                    });
    if (match == monitors.end()) {
        throw std::invalid_argument("Unknown monitor: " + WideToUtf8(selector));
    }
    return *match;
}

std::map<std::uint32_t, ProcessInfo> EnumerateProcesses() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Process enumeration failed: " +
                                 WideToUtf8(LastErrorMessage()));
    }
    std::map<std::uint32_t, ProcessInfo> processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::uint32_t process_id = entry.th32ProcessID;
            std::uint64_t creation_time_100ns = 0;
            const std::wstring image_path =
                QueryProcessImagePath(process_id, creation_time_100ns);
            processes[process_id] = {process_id, entry.th32ParentProcessID, entry.szExeFile,
                                     image_path, creation_time_100ns};
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processes;
}

std::vector<ProcessInfo> StandaloneCodexProcesses(
    const std::map<std::uint32_t, ProcessInfo>& processes,
    const Config& config) {
    std::vector<ProcessInfo> matches;
    for (const auto& [id, process] : processes) {
        (void)id;
        const bool name_match = std::any_of(
            config.codex_process_names.begin(), config.codex_process_names.end(),
            [&](const std::wstring& name) { return ToLower(name) == ToLower(process.name); });
        if (!name_match) continue;
        if (!process.image_path.empty() &&
            MatchesAny(process.image_path, config.codex_path_exclude_patterns)) {
            continue;
        }
        matches.push_back(process);
    }
    return matches;
}

std::vector<WindowInfo> EnumerateTerminalWindows(
    const std::map<std::uint32_t, ProcessInfo>& processes,
    const Config& config) {
    struct Context {
        const std::map<std::uint32_t, ProcessInfo>* processes;
        const Config* config;
        std::vector<WindowInfo>* windows;
    } context{&processes, &config, nullptr};
    std::vector<WindowInfo> windows;
    context.windows = &windows;

    const auto callback = [](HWND window, LPARAM parameter) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(parameter);
        if (!IsWindowVisible(window) || IsCloaked(window)) return TRUE;
        const std::wstring class_name = WindowClass(window);
        if (std::find(context.config->terminal_class_names.begin(),
                      context.config->terminal_class_names.end(),
                      class_name) == context.config->terminal_class_names.end()) {
            return TRUE;
        }
        const auto rect = WindowRect(window);
        if (!rect) return TRUE;
        DWORD process_id = 0;
        GetWindowThreadProcessId(window, &process_id);
        const auto process = context.processes->find(process_id);
        WindowInfo candidate{
            reinterpret_cast<std::uintptr_t>(window),
            WindowText(window),
            class_name,
            process_id,
            process == context.processes->end() ? L"" : process->second.name,
            process == context.processes->end() ? L"" : process->second.image_path,
            *rect,
            IsIconic(window) != FALSE,
            static_cast<int>(context.windows->size()),
        };
        context.windows->push_back(std::move(candidate));
        return TRUE;
    };

    if (!EnumWindows(callback, reinterpret_cast<LPARAM>(&context))) {
        throw std::runtime_error("Window enumeration failed: " +
                                 WideToUtf8(LastErrorMessage()));
    }
    return windows;
}

std::pair<std::wstring, std::wstring> ReadTerminalTextTail(std::uintptr_t hwnd,
                                                           int character_count) {
    if (character_count <= 0 || character_count > 65536) {
        return {L"", L"terminal accessibility tail size is outside the safe range"};
    }
    if (!IsWindow(reinterpret_cast<HWND>(hwnd))) {
        return {L"", L"terminal window is no longer open"};
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return {L"", L"COM initialization failed"};
    }
    const auto finish = [&](std::pair<std::wstring, std::wstring> result) {
        if (uninitialize) CoUninitialize();
        return result;
    };

    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(automation.Put()));
    if (FAILED(result)) {
        result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(automation.Put()));
    }
    if (FAILED(result)) {
        return finish({L"", L"Windows UI Automation is unavailable (" + HResultName(result) +
                                  L")"});
    }

    ComPtr<IUIAutomationElement> root;
    result = automation->ElementFromHandle(reinterpret_cast<HWND>(hwnd), root.Put());
    if (FAILED(result) || !root) {
        return finish({L"", L"terminal accessibility root is unavailable"});
    }

    VARIANT class_value;
    VariantInit(&class_value);
    class_value.vt = VT_BSTR;
    class_value.bstrVal = SysAllocString(L"TermControl");
    ComPtr<IUIAutomationCondition> condition;
    result = automation->CreatePropertyCondition(UIA_ClassNamePropertyId, class_value,
                                                 condition.Put());
    VariantClear(&class_value);
    if (FAILED(result) || !condition) {
        return finish({L"", L"terminal accessibility condition could not be created"});
    }

    ComPtr<IUIAutomationElement> terminal;
    result = root->FindFirst(TreeScope_Descendants, condition.Get(), terminal.Put());
    if (FAILED(result) || !terminal) {
        return finish({L"", L"Windows Terminal accessibility text is unavailable"});
    }

    ComPtr<IUIAutomationTextPattern> pattern;
    result = terminal->GetCurrentPatternAs(UIA_TextPatternId,
                                           __uuidof(IUIAutomationTextPattern),
                                           reinterpret_cast<void**>(pattern.Put()));
    if (FAILED(result) || !pattern) {
        return finish({L"", L"Windows Terminal text pattern is unavailable"});
    }

    ComPtr<IUIAutomationTextRange> document;
    result = pattern->get_DocumentRange(document.Put());
    if (FAILED(result) || !document) {
        return finish({L"", L"Windows Terminal document range is unavailable"});
    }
    ComPtr<IUIAutomationTextRange> tail;
    result = document->Clone(tail.Put());
    if (FAILED(result) || !tail) {
        return finish({L"", L"Windows Terminal text range could not be cloned"});
    }
    result = tail->MoveEndpointByRange(TextPatternRangeEndpoint_Start, document.Get(),
                                       TextPatternRangeEndpoint_End);
    int moved = 0;
    if (SUCCEEDED(result)) {
        result = tail->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character,
                                          -character_count, &moved);
    }
    BSTR text = nullptr;
    if (SUCCEEDED(result)) result = tail->GetText(-1, &text);
    if (FAILED(result)) {
        if (text) SysFreeString(text);
        return finish({L"", L"Windows Terminal accessibility read failed (" +
                                  HResultName(result) + L")"});
    }
    std::wstring value = text ? std::wstring(text, SysStringLen(text)) : std::wstring();
    if (text) SysFreeString(text);
    return finish({std::move(value), L""});
}

// AttachConsole is process-global: isolate probing so the GUI/CLI never detaches
// from its own console. The helper reads console ownership and never sends input.
int RunConsoleWindowProbe(const std::vector<std::uint32_t>& process_ids) {
    HANDLE output = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE),
                         GetCurrentProcess(), &output, 0, FALSE, DUPLICATE_SAME_ACCESS)) return 1;
    std::ostringstream rows;
    for (const auto pid : process_ids) {
        FreeConsole();
        if (!AttachConsole(pid)) continue;
        std::uint64_t created = 0;
        QueryProcessImagePath(pid, created);
        const HWND console = GetConsoleWindow();
        HWND target = console ? GetWindow(console, GW_OWNER) : nullptr;
        if (!target && console && IsWindowVisible(console)) target = console;
        if (target && IsWindowVisible(target) && created) {
            rows << pid << ' ' << created << ' ' << reinterpret_cast<std::uintptr_t>(target) << '\n';
        }
        FreeConsole();
    }
    const auto data = rows.str();
    DWORD written = 0;
    const bool ok = WriteFile(output, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != FALSE;
    CloseHandle(output);
    return ok && written == data.size() ? 0 : 1;
}

std::map<std::uint32_t, std::uintptr_t> ProbeCodexConsoleWindows(const std::vector<ProcessInfo>& processes) {
    std::map<std::uint32_t, std::uintptr_t> result;
    if (processes.empty() || processes.size() > 128) return result;
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (!length || length >= std::size(module)) return result;
    const auto helper = std::filesystem::path(module).parent_path() / L"CodexOrganizer.exe";
    std::wstring command = L"\"" + helper.wstring() + L"\"";
    for (const auto& process : processes) command += L" --console-window-pid " + std::to_wstring(process.process_id);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &security, 65536)) return result;
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(reader); CloseHandle(writer); return result;
    }
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writer;
    startup.hStdError = writer;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    CloseHandle(writer);
    if (!started) { CloseHandle(reader); return result; }
    const DWORD waited = WaitForSingleObject(child.hProcess, 3000);
    DWORD code = 1;
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 1); // Only our own bounded probe helper.
        WaitForSingleObject(child.hProcess, 1000);
    } else {
        GetExitCodeProcess(child.hProcess, &code);
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (code == 0) {
        char buffer[16384]{};
        DWORD available = 0, read = 0;
        if (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) &&
            available > 0 && available < sizeof(buffer) &&
            ReadFile(reader, buffer, available, &read, nullptr)) {
            std::istringstream rows(std::string(buffer, read));
            std::uint32_t pid = 0;
            std::uint64_t created = 0;
            std::uintptr_t hwnd = 0;
            while (rows >> pid >> created >> hwnd) {
                const auto process = std::find_if(processes.begin(), processes.end(), [&](const auto& item) {
                    return item.process_id == pid && item.creation_time_100ns == created;
                });
                if (process != processes.end() && IsWindow(reinterpret_cast<HWND>(hwnd))) result[pid] = hwnd;
            }
        }
    }
    CloseHandle(reader);
    return result;
}

DetectionResult DetectCodexWindows(const Config& config, bool include_all_terminals,
                                   const TerminalTextReader& read_terminal_text) {
    const auto processes = EnumerateProcesses();
    auto codex_processes = StandaloneCodexProcesses(processes, config);
    auto candidates = EnumerateTerminalWindows(processes, config);
    const auto console_windows = ProbeCodexConsoleWindows(codex_processes);
    FILETIME current_time{};
    GetSystemTimeAsFileTime(&current_time);
    ULARGE_INTEGER now{};
    now.LowPart = current_time.dwLowDateTime;
    now.HighPart = current_time.dwHighDateTime;
    ApplyCodexProcessUptimes(candidates, codex_processes, console_windows, now.QuadPart);
    std::wstring thread_warning;
    std::map<std::wstring, std::wstring> names;
    if (config.organize_by_thread_name) {
        try {
            names = LoadThreadNameIndex(config.session_index_path.empty()
                                            ? std::nullopt
                                            : std::optional<std::filesystem::path>(
                                                  config.session_index_path));
        } catch (const std::exception& error) {
            thread_warning = Utf8ToWide(error.what());
        }
    }
    std::size_t accessibility_failures = 0;
    for (WindowInfo& window : candidates) {
        if (MatchesAny(window.title, config.title_exclude_patterns)) continue;
        auto [terminal_tail, error] =
            read_terminal_text(window.hwnd, config.terminal_text_tail_characters);
        if (!error.empty()) {
            ++accessibility_failures;
            continue;
        }
        const CodexStatusLine status = ParseCodexStatusLine(terminal_tail);
        window.codex_status = status.status;
        window.codex_model = status.model;
        window.codex_used_tokens = status.used_tokens;
        if (config.organize_by_thread_name) {
            const ThreadIdentity identity = ParseThreadIdentity(terminal_tail, names);
            window.thread_name = identity.name;
            window.thread_id = identity.thread_id;
            window.thread_identity_source = identity.source;
        }
        terminal_tail.clear();
        terminal_tail.shrink_to_fit();
    }
    auto association = AssociateCodexWindows(std::move(candidates), codex_processes.size(),
                                             config.title_include_patterns,
                                             config.title_exclude_patterns);
    if (!include_all_terminals) {
        std::erase_if(association.windows, [](const WindowInfo& window) {
            return !window.auto_selected;
        });
    }
    if (config.organize_by_thread_name) {
        SortWindowsByThread(association.windows, config.unnamed_threads_last);
    }
    if (accessibility_failures) {
        if (!thread_warning.empty()) thread_warning += L" ";
        thread_warning += L"Terminal text was unavailable for " +
                          std::to_wstring(accessibility_failures) +
                          L" candidate(s); only configured title matches can identify those windows.";
    }
    return {std::move(association.windows), std::move(codex_processes),
            std::move(association.mode), std::move(association.warning),
            std::move(thread_warning)};
}

std::vector<MoveResult> MoveWindows(const std::vector<WindowInfo>& windows,
                                    const std::vector<Rect>& rectangles,
                                    const Config& config) {
    if (windows.size() != rectangles.size()) {
        throw std::invalid_argument("Each window must have exactly one target rectangle");
    }
    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (!config.bring_to_front) flags |= SWP_NOZORDER;
    std::vector<MoveResult> results;
    results.reserve(windows.size());
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const HWND window = reinterpret_cast<HWND>(windows[index].hwnd);
        const Rect& target = rectangles[index];
        if (!IsWindow(window)) {
            results.push_back({windows[index].hwnd, target, std::nullopt,
                               L"Window is no longer open", L""});
            continue;
        }
        if (config.restore_minimized && (IsIconic(window) || IsZoomed(window))) {
            ShowWindow(window, SW_RESTORE);
        }
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowPos(window, HWND_TOP, target.left, target.top, target.Width(),
                          target.Height(), flags)) {
            results.push_back({windows[index].hwnd, target, std::nullopt,
                               LastErrorMessage(), L""});
            continue;
        }
        const auto actual = WindowRect(window);
        std::wstring warning;
        if (actual &&
            (std::abs(target.left - actual->left) > 3 ||
             std::abs(target.top - actual->top) > 3 ||
             std::abs(target.Width() - actual->Width()) > 3 ||
             std::abs(target.Height() - actual->Height()) > 3)) {
            warning = L"Windows or the terminal applied a minimum-size constraint; actual=" +
                      actual->Xywh();
        }
        results.push_back({windows[index].hwnd, target, actual, L"", std::move(warning)});
    }
    return results;
}

void WriteLog(const std::filesystem::path& base_directory,
              const std::wstring& level,
              const std::wstring& message) noexcept {
    try {
        const std::filesystem::path log_directory = base_directory / L"logs";
        std::filesystem::create_directories(log_directory);
        const std::filesystem::path log_path = log_directory / L"codex-organizer.log";
        std::error_code error;
        if (std::filesystem::exists(log_path, error) &&
            std::filesystem::file_size(log_path, error) >= 512000) {
            const std::filesystem::path old_path = log_directory / L"codex-organizer.log.1";
            std::filesystem::remove(old_path, error);
            error.clear();
            std::filesystem::rename(log_path, old_path, error);
        }
        std::ofstream stream(log_path, std::ios::binary | std::ios::app);
        if (stream) {
            stream << WideToUtf8(LocalTimestamp() + L" " + level + L" " + message + L"\n");
        }
    } catch (const std::exception&) {
    }
}

}  // namespace codex_organizer
