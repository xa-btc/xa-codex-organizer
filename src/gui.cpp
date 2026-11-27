#include "codex_organizer/windows.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace codex_organizer {
namespace {

constexpr COLORREF kBackgroundColor = RGB(13, 17, 23);
constexpr COLORREF kSurfaceColor = RGB(22, 27, 34);
constexpr COLORREF kControlColor = RGB(33, 38, 45);
constexpr COLORREF kControlHoverColor = RGB(48, 54, 61);
constexpr COLORREF kBorderColor = RGB(48, 54, 61);
constexpr COLORREF kTextColor = RGB(230, 237, 243);
constexpr COLORREF kMutedTextColor = RGB(139, 148, 158);
constexpr COLORREF kAccentColor = RGB(88, 166, 255);
constexpr COLORREF kAccentPressedColor = RGB(31, 111, 235);

enum ControlId : int {
    kHeading = 1000,
    kMonitorCombo = 1001,
    kRefresh = 1002,
    kWindowList = 1003,
    kArrange2 = 1102,
    kArrange3 = 1103,
    kArrange4 = 1104,
    kArrange8 = 1108,
    kPreviewSelected = 1201,
    kArrangeSelected = 1202,
    kSelectAuto = 1301,
    kSelectAll = 1302,
    kClearSelection = 1303,
};

struct GuiState {
    HINSTANCE instance{};
    HWND window{};
    HWND heading{};
    HWND subtitle{};
    HWND monitor_label{};
    HWND monitor_combo{};
    HWND refresh_button{};
    HWND list{};
    HWND status{};
    HWND quick_label{};
    std::vector<HWND> rail_controls;
    HFONT normal_font{};
    HFONT heading_font{};
    HBRUSH background_brush{};
    HBRUSH surface_brush{};
    HBRUSH control_brush{};
    Config config;
    std::filesystem::path base_directory;
    std::vector<MonitorInfo> monitors;
    DetectionResult detection;
};

GuiState* State(HWND window) {
    return reinterpret_cast<GuiState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void ApplyFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void ApplyDarkTitleBar(HWND window) {
    BOOL enabled = TRUE;
    if (FAILED(DwmSetWindowAttribute(window, 20, &enabled, sizeof(enabled)))) {
        DwmSetWindowAttribute(window, 19, &enabled, sizeof(enabled));
    }
}

void ApplyDarkControlTheme(HWND control, const wchar_t* theme = L"DarkMode_Explorer") {
    SetWindowTheme(control, theme, nullptr);
}

bool DrawButton(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_BUTTON) return false;
    COLORREF background = kControlColor;
    if ((item.itemState & ODS_SELECTED) != 0) {
        background = kAccentPressedColor;
    } else if ((item.itemState & ODS_HOTLIGHT) != 0) {
        background = kControlHoverColor;
    }
    HBRUSH background_brush = CreateSolidBrush(background);
    FillRect(item.hDC, &item.rcItem, background_brush);
    DeleteObject(background_brush);
    HBRUSH border_brush = CreateSolidBrush(
        (item.itemState & ODS_FOCUS) != 0 ? kAccentColor : kBorderColor);
    FrameRect(item.hDC, &item.rcItem, border_brush);
    DeleteObject(border_brush);

    wchar_t label[256]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    RECT text_rect = item.rcItem;
    if ((item.itemState & ODS_SELECTED) != 0) OffsetRect(&text_rect, 1, 1);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC,
                 (item.itemState & ODS_DISABLED) != 0 ? kMutedTextColor : kTextColor);
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const HGDIOBJ previous_font = font ? SelectObject(item.hDC, font) : nullptr;
    DrawTextW(item.hDC, label, -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previous_font) SelectObject(item.hDC, previous_font);
    return true;
}

bool DrawComboItem(const GuiState& state, const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_COMBOBOX || item.CtlID != kMonitorCombo) return false;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    FillRect(item.hDC, &item.rcItem, selected ? state.surface_brush : state.control_brush);

    LRESULT index = static_cast<LRESULT>(item.itemID);
    if (item.itemID == static_cast<UINT>(-1)) {
        index = SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0);
    }
    std::wstring label;
    if (index != CB_ERR) {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN,
                                            static_cast<WPARAM>(index), 0);
        if (length != CB_ERR) {
            label.resize(static_cast<std::size_t>(length) + 1);
            SendMessageW(item.hwndItem, CB_GETLBTEXT, static_cast<WPARAM>(index),
                         reinterpret_cast<LPARAM>(label.data()));
            label.resize(static_cast<std::size_t>(length));
        }
    }
    RECT text_rect = item.rcItem;
    text_rect.left += 8;
    text_rect.right -= 4;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, kTextColor);
    const HGDIOBJ previous_font = state.normal_font
                                      ? SelectObject(item.hDC, state.normal_font)
                                      : nullptr;
    DrawTextW(item.hDC, label.c_str(), -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previous_font) SelectObject(item.hDC, previous_font);
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(item.hDC, &focus);
    }
    return true;
}

LRESULT DrawListHeader(const GuiState& state, NMCUSTOMDRAW& draw) {
    if (draw.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

    wchar_t label[256]{};
    HDITEMW header_item{};
    header_item.mask = HDI_TEXT;
    header_item.pszText = label;
    header_item.cchTextMax = static_cast<int>(std::size(label));
    Header_GetItem(ListView_GetHeader(state.list), static_cast<int>(draw.dwItemSpec),
                   &header_item);
    FillRect(draw.hdc, &draw.rc, state.surface_brush);
    HBRUSH border_brush = CreateSolidBrush(kBorderColor);
    FrameRect(draw.hdc, &draw.rc, border_brush);
    DeleteObject(border_brush);
    RECT text_rect = draw.rc;
    text_rect.left += 8;
    text_rect.right -= 6;
    SetBkMode(draw.hdc, TRANSPARENT);
    SetTextColor(draw.hdc, kTextColor);
    const HGDIOBJ previous_font = state.normal_font
                                      ? SelectObject(draw.hdc, state.normal_font)
                                      : nullptr;
    DrawTextW(draw.hdc, label, -1, &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previous_font) SelectObject(draw.hdc, previous_font);
    return CDRF_SKIPDEFAULT;
}

LRESULT CALLBACK ListViewProcedure(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam, UINT_PTR subclass_id,
                                   DWORD_PTR reference_data) {
    auto* state = reinterpret_cast<GuiState*>(reference_data);
    if (message == WM_NOTIFY && state) {
        const auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header && header->hwndFrom == ListView_GetHeader(window) &&
            header->code == NM_CUSTOMDRAW) {
            return DrawListHeader(*state, *reinterpret_cast<NMCUSTOMDRAW*>(lparam));
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ListViewProcedure, subclass_id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

HWND MakeControl(GuiState& state, DWORD extended_style, const wchar_t* class_name,
                 const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowExW(
        extended_style, class_name, text, style | WS_CHILD | WS_VISIBLE, 0, 0, 10, 10,
        state.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), state.instance,
        nullptr);
    if (!control) throw std::runtime_error("Could not create a Windows GUI control");
    ApplyFont(control, state.normal_font);
    return control;
}

void AddListColumn(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

void SetListText(HWND list, int row, int column, const std::wstring& text) {
    ListView_SetItemText(list, row, column, const_cast<LPWSTR>(text.c_str()));
}

void ShowError(HWND owner, const std::wstring& message) {
    MessageBoxW(owner, message.c_str(), kAppName, MB_OK | MB_ICONERROR);
}

void ShowWarning(HWND owner, const std::wstring& message) {
    MessageBoxW(owner, message.c_str(), kAppName, MB_OK | MB_ICONWARNING);
}

const MonitorInfo& SelectedMonitor(GuiState& state) {
    const LRESULT index = SendMessageW(state.monitor_combo, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<std::size_t>(index) >= state.monitors.size()) {
        throw std::runtime_error("Choose a target monitor");
    }
    return state.monitors[static_cast<std::size_t>(index)];
}

std::vector<WindowInfo> SelectedWindows(const GuiState& state) {
    std::vector<WindowInfo> selected;
    for (int row = 0; row < static_cast<int>(state.detection.windows.size()); ++row) {
        if ((ListView_GetItemState(state.list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0) {
            selected.push_back(state.detection.windows[static_cast<std::size_t>(row)]);
        }
    }
    return selected;
}

void SelectRows(GuiState& state, int mode) {
    for (int row = 0; row < static_cast<int>(state.detection.windows.size()); ++row) {
        bool selected = false;
        if (mode == 1) selected = state.detection.windows[static_cast<std::size_t>(row)].auto_selected;
        if (mode == 2) selected = true;
        ListView_SetItemState(state.list, row, selected ? LVIS_SELECTED : 0, LVIS_SELECTED);
    }
}

void Refresh(GuiState& state) {
    try {
        SetWindowTextW(state.heading, kAppName);
        SetWindowTextW(state.status, L"Scanning...");
        state.detection = DetectCodexWindows(state.config);
        std::wstring heading = kAppName;
        std::wstring usage_warning;
        if (state.config.request_account_usage) {
            try {
                heading += L" \u00b7 " + FetchCodexWeeklyUsage();
            } catch (const std::exception& error) {
                heading += L" \u00b7 weekly unavailable";
                usage_warning = L" Weekly usage request failed: " + Utf8ToWide(error.what());
                WriteLog(state.base_directory, L"WARNING", usage_warning);
            }
        }
        SetWindowTextW(state.heading, heading.c_str());
        ListView_DeleteAllItems(state.list);
        for (std::size_t index = 0; index < state.detection.windows.size(); ++index) {
            const WindowInfo& window = state.detection.windows[index];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = const_cast<LPWSTR>(window.process_uptime.c_str());
            const int row = ListView_InsertItem(state.list, &item);
            SetListText(state.list, row, 1, window.ThreadLabel());
            SetListText(state.list, row, 2, window.codex_status);
            SetListText(state.list, row, 3, window.codex_model);
            SetListText(state.list, row, 4, window.codex_used_tokens);
            if (window.auto_selected) {
                ListView_SetItemState(state.list, row, LVIS_SELECTED, LVIS_SELECTED);
            }
        }
        std::wstring status =
            std::to_wstring(state.detection.standalone_codex_processes.size()) +
            L" standalone Codex processes; " +
            std::to_wstring(state.detection.windows.size()) + L" Codex windows; " +
            std::to_wstring(state.detection.AutoWindows().size()) + L" auto-selected (" +
            state.detection.association_mode + L").";
        if (state.config.organize_by_thread_name) {
            status += L" " + std::to_wstring(state.detection.NamedThreadCount()) +
                      L" named thread(s), ordered A-Z" +
                      (state.config.unnamed_threads_last ? L" with unnamed last." : L".");
        }
        if (!state.detection.warning.empty()) status += L" " + state.detection.warning;
        if (!state.detection.thread_warning.empty()) status += L" " + state.detection.thread_warning;
        if (!usage_warning.empty()) status += usage_warning;
        SetWindowTextW(state.status, status.c_str());
    } catch (const std::exception& error) {
        WriteLog(state.base_directory, L"ERROR", L"Window detection failed: " + Utf8ToWide(error.what()));
        ShowError(state.window, L"Window detection failed:\n" + Utf8ToWide(error.what()));
        SetWindowTextW(state.status, L"Window detection failed. See logs for details.");
    }
}

void Preview(GuiState& state) {
    const auto windows = SelectedWindows(state);
    if (windows.empty()) {
        ShowWarning(state.window, L"Select at least one window.");
        return;
    }
    const MonitorInfo& monitor = SelectedMonitor(state);
    const LayoutPlan plan = CalculateLayout(static_cast<int>(windows.size()), monitor.work_area,
                                            state.config);
    MessageBoxW(state.window, FormatPlan(windows, monitor, plan).c_str(), kAppName,
                MB_OK | MB_ICONINFORMATION);
}

void Arrange(GuiState& state, std::vector<WindowInfo> windows) {
    if (windows.empty()) {
        ShowWarning(state.window, L"Select at least one window.");
        return;
    }
    const MonitorInfo& monitor = SelectedMonitor(state);
    const LayoutPlan plan = CalculateLayout(static_cast<int>(windows.size()), monitor.work_area,
                                            state.config);
    if (state.config.confirm_before_move) {
        const std::wstring prompt = FormatPlan(windows, monitor, plan) + L"\n\nMove these windows now?";
        if (MessageBoxW(state.window, prompt.c_str(), kAppName,
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }
    const auto results = MoveWindows(windows, plan.rectangles, state.config);
    std::vector<std::wstring> issues;
    for (const MoveResult& result : results) {
        if (!result.error.empty()) issues.push_back(result.error);
        else if (!result.warning.empty()) issues.push_back(result.warning);
    }
    WriteLog(state.base_directory, L"INFO",
             L"Arranged " + std::to_wstring(windows.size()) + L" windows on " +
                 monitor.device_name + L" with " + std::to_wstring(issues.size()) + L" issue(s)");
    if (!issues.empty()) {
        std::wstring details = L"Arrangement completed with warnings:";
        for (const std::wstring& issue : issues) details += L"\n" + issue;
        ShowWarning(state.window, details);
    }
    if (state.config.minimize_after_arrange) {
        ShowWindow(state.window, SW_MINIMIZE);
    } else {
        const std::wstring message = L"Arranged " + std::to_wstring(windows.size()) +
                                     L" windows in " + std::to_wstring(plan.columns) + L" x " +
                                     std::to_wstring(plan.rows) + L".";
        SetWindowTextW(state.status, message.c_str());
    }
}

void ArrangeCount(GuiState& state, int count) {
    auto windows = SelectedWindows(state);
    if (static_cast<int>(windows.size()) < count) {
        ShowWarning(state.window, L"Select at least " + std::to_wstring(count) +
                                      L" windows first. Currently selected: " +
                                      std::to_wstring(windows.size()) + L".");
        return;
    }
    windows.resize(static_cast<std::size_t>(count));
    Arrange(state, std::move(windows));
}

void LayoutControls(GuiState& state, int width, int height) {
    constexpr int margin = 12;
    constexpr int monitor_top = 72;
    constexpr int monitor_height = 28;
    constexpr int monitor_label_width = 118;
    constexpr int body_top = 112;
    constexpr int status_height = 48;
    const int rail_left = std::max(margin, width - margin - kControlRailWidth);
    const int list_width = std::max(120, rail_left - margin - 12);
    const int body_height = std::max(100, height - body_top - status_height - margin);

    MoveWindow(state.heading, margin, 10, width - margin * 2, 28, TRUE);
    MoveWindow(state.subtitle, margin, 42, width - margin * 2, 22, TRUE);
    MoveWindow(state.monitor_label, margin, monitor_top + 5, monitor_label_width, 20, TRUE);
    MoveWindow(state.monitor_combo, margin + monitor_label_width, monitor_top,
               std::max(120, width - margin * 2 - monitor_label_width - 88), monitor_height, TRUE);
    MoveWindow(state.refresh_button, width - margin - 80, monitor_top, 80, monitor_height, TRUE);
    MoveWindow(state.list, margin, body_top, list_width, body_height, TRUE);
    MoveWindow(state.status, margin, height - status_height, width - margin * 2,
               status_height - 4, TRUE);

    int y = body_top;
    MoveWindow(state.quick_label, rail_left, y, kControlRailWidth, 22, TRUE);
    y += 26;
    for (std::size_t index = 0; index < state.rail_controls.size(); ++index) {
        if (index == 4 || index == 6) y += 12;
        MoveWindow(state.rail_controls[index], rail_left, y, kControlRailWidth, 30, TRUE);
        y += 36;
    }
}

void InitializeGui(GuiState& state) {
    ApplyDarkTitleBar(state.window);
    ApplyDarkControlTheme(state.window);
    state.normal_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    state.heading_font = CreateFontW(-23, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    state.heading = MakeControl(state, 0, L"STATIC", kAppName, SS_LEFT, kHeading);
    ApplyFont(state.heading, state.heading_font);
    state.subtitle = MakeControl(
        state, 0, L"STATIC",
        L"Select Codex CMD / Windows Terminal windows, then use a quick count or arrange the full selection.",
        SS_LEFT, 0);
    state.monitor_label = MakeControl(state, 0, L"STATIC", L"Target monitor:", SS_LEFT, 0);
    state.monitor_combo = MakeControl(state, 0, WC_COMBOBOXW, L"",
                                      CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                                          WS_TABSTOP | WS_VSCROLL,
                                      kMonitorCombo);
    state.refresh_button = MakeControl(state, 0, L"BUTTON", L"Refresh",
                                       BS_OWNERDRAW | WS_TABSTOP, kRefresh);
    state.list = MakeControl(state, WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP,
                             kWindowList);
    if (!SetWindowSubclass(state.list, ListViewProcedure, 1,
                           reinterpret_cast<DWORD_PTR>(&state))) {
        throw std::runtime_error("Could not install the list-view dark-header handler.");
    }
    ListView_SetExtendedListViewStyle(
        state.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
                        LVS_EX_BORDERSELECT);
    ListView_SetBkColor(state.list, kSurfaceColor);
    ListView_SetTextBkColor(state.list, kSurfaceColor);
    ListView_SetTextColor(state.list, kTextColor);
    ApplyDarkControlTheme(state.list);
    ApplyDarkControlTheme(ListView_GetHeader(state.list), L"DarkMode_ItemsView");
    ApplyDarkControlTheme(state.monitor_combo, L"DarkMode_CFD");
    ApplyDarkControlTheme(state.refresh_button);
    SendMessageW(state.monitor_combo, CB_SETITEMHEIGHT, 0, 24);
    SendMessageW(state.monitor_combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 24);
    AddListColumn(state.list, 0, 70, L"Uptime");
    AddListColumn(state.list, 1, 250, L"Thread name (A-Z)");
    AddListColumn(state.list, 2, 90, L"Status");
    AddListColumn(state.list, 3, 235, L"Model");
    AddListColumn(state.list, 4, 100, L"Used tokens");
    state.status = MakeControl(state, 0, L"STATIC", L"Scanning...", SS_LEFT, 0);
    state.quick_label = MakeControl(state, 0, L"STATIC", L"Quick layouts", SS_LEFT, 0);

    const std::pair<int, const wchar_t*> buttons[] = {
        {kArrange2, L"Arrange 2"},
        {kArrange3, L"Arrange 3"},
        {kArrange4, L"Arrange 4"},
        {kArrange8, L"Arrange 8"},
        {kPreviewSelected, L"Preview selected"},
        {kArrangeSelected, L"Arrange selected"},
        {kSelectAuto, L"Select auto-detected"},
        {kSelectAll, L"Select all candidates"},
        {kClearSelection, L"Clear selection"},
    };
    for (const auto& [id, label] : buttons) {
        state.rail_controls.push_back(
            MakeControl(state, 0, L"BUTTON", label, BS_OWNERDRAW | WS_TABSTOP, id));
        ApplyDarkControlTheme(state.rail_controls.back());
    }

    for (std::size_t index = 0; index < state.monitors.size(); ++index) {
        const std::wstring label = std::to_wstring(index + 1) + L": " + state.monitors[index].Label();
        SendMessageW(state.monitor_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }
    const MonitorInfo& configured = ResolveMonitor(state.monitors, state.config.monitor);
    const auto iterator = std::find_if(
        state.monitors.begin(), state.monitors.end(),
        [&](const MonitorInfo& monitor) { return monitor.handle == configured.handle; });
    SendMessageW(state.monitor_combo, CB_SETCURSEL,
                 static_cast<WPARAM>(std::distance(state.monitors.begin(), iterator)), 0);
    Refresh(state);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* state = static_cast<GuiState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    GuiState* state = State(window);
    try {
        switch (message) {
            case WM_CREATE:
                InitializeGui(*state);
                return 0;
            case WM_SIZE:
                if (state) LayoutControls(*state, LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_ERASEBKGND:
                if (state && state->background_brush) {
                    RECT bounds{};
                    GetClientRect(window, &bounds);
                    FillRect(reinterpret_cast<HDC>(wparam), &bounds,
                             state->background_brush);
                    return 1;
                }
                break;
            case WM_CTLCOLORSTATIC:
                if (state) {
                    HDC context = reinterpret_cast<HDC>(wparam);
                    SetBkMode(context, TRANSPARENT);
                    const HWND control = reinterpret_cast<HWND>(lparam);
                    SetTextColor(context,
                                 control == state->heading ? kAccentColor : kMutedTextColor);
                    return reinterpret_cast<LRESULT>(state->background_brush);
                }
                break;
            case WM_CTLCOLORLISTBOX:
            case WM_CTLCOLOREDIT:
                if (state) {
                    HDC context = reinterpret_cast<HDC>(wparam);
                    SetBkColor(context, kControlColor);
                    SetTextColor(context, kTextColor);
                    return reinterpret_cast<LRESULT>(state->control_brush);
                }
                break;
            case WM_DRAWITEM:
                if (state) {
                    const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
                    if (item && (DrawButton(*item) || DrawComboItem(*state, *item))) {
                        return TRUE;
                    }
                }
                break;
            case WM_GETMINMAXINFO: {
                auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = 820;
                limits->ptMinTrackSize.y = 500;
                return 0;
            }
            case WM_COMMAND:
                if (!state || HIWORD(wparam) != BN_CLICKED) break;
                switch (LOWORD(wparam)) {
                    case kRefresh: Refresh(*state); return 0;
                    case kArrange2: ArrangeCount(*state, 2); return 0;
                    case kArrange3: ArrangeCount(*state, 3); return 0;
                    case kArrange4: ArrangeCount(*state, 4); return 0;
                    case kArrange8: ArrangeCount(*state, 8); return 0;
                    case kPreviewSelected: Preview(*state); return 0;
                    case kArrangeSelected: Arrange(*state, SelectedWindows(*state)); return 0;
                    case kSelectAuto: SelectRows(*state, 1); return 0;
                    case kSelectAll: SelectRows(*state, 2); return 0;
                    case kClearSelection: SelectRows(*state, 0); return 0;
                    default: break;
                }
                break;
            case WM_DESTROY:
                if (state) {
                    if (state->heading_font) DeleteObject(state->heading_font);
                    if (state->normal_font) DeleteObject(state->normal_font);
                    if (state->control_brush) DeleteObject(state->control_brush);
                    if (state->surface_brush) DeleteObject(state->surface_brush);
                    if (state->background_brush) DeleteObject(state->background_brush);
                    state->heading_font = nullptr;
                    state->normal_font = nullptr;
                    state->control_brush = nullptr;
                    state->surface_brush = nullptr;
                    state->background_brush = nullptr;
                }
                PostQuitMessage(0);
                return 0;
            default: break;
        }
    } catch (const std::exception& error) {
        if (state) {
            WriteLog(state->base_directory, L"ERROR", Utf8ToWide(error.what()));
            ShowError(window, Utf8ToWide(error.what()));
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int RunGui(void* instance, const Config& config, const std::filesystem::path& base_directory) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    GuiState state;
    state.instance = static_cast<HINSTANCE>(instance);
    state.config = config;
    state.base_directory = base_directory;
    state.monitors = EnumerateMonitors();
    state.background_brush = CreateSolidBrush(kBackgroundColor);
    state.surface_brush = CreateSolidBrush(kSurfaceColor);
    state.control_brush = CreateSolidBrush(kControlColor);
    if (!state.background_brush || !state.surface_brush || !state.control_brush) {
        if (state.control_brush) DeleteObject(state.control_brush);
        if (state.surface_brush) DeleteObject(state.surface_brush);
        if (state.background_brush) DeleteObject(state.background_brush);
        throw std::runtime_error("Could not create dark-theme resources");
    }

    constexpr wchar_t class_name[] = L"CodexOrganizerMainWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = state.instance;
    window_class.hIcon = static_cast<HICON>(LoadImageW(
        state.instance, MAKEINTRESOURCEW(IDI_CODEX_ORGANIZER), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(
        state.instance, MAKEINTRESOURCEW(IDI_CODEX_ORGANIZER), IMAGE_ICON, 16, 16,
        LR_DEFAULTCOLOR));
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = state.background_brush;
    window_class.lpszClassName = class_name;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Could not register the organizer window class");
    }

    const std::wstring title = std::wstring(kAppName) + L" " + kAppVersion;
    HWND window = CreateWindowExW(0, class_name, title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 980, 620, nullptr, nullptr,
                                  state.instance, &state);
    if (!window) {
        DeleteObject(state.control_brush);
        DeleteObject(state.surface_brush);
        DeleteObject(state.background_brush);
        throw std::runtime_error("Could not create the organizer window");
    }
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace codex_organizer
