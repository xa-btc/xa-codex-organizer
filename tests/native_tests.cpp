#include "codex_organizer/core.hpp"
#include "codex_organizer/windows.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace codex_organizer;

namespace {

// Exact selectors consumed by the project-local function traceability audit.
// The runtime harness below exposes the same names through --list and accepts
// one exact name as its positional test selector.
#define XA_WDD_TEST(name) static_assert(true, name)
XA_WDD_TEST("layout.two_windows_keep_preferred_size");
XA_WDD_TEST("layout.three_windows_center_final_row");
XA_WDD_TEST("layout.four_windows_keep_preferred_size");
XA_WDD_TEST("layout.eight_windows_shrink_height");
XA_WDD_TEST("layout.larger_counts_add_columns");
XA_WDD_TEST("layout.practical_counts_stay_visible");
XA_WDD_TEST("association.exact_count_requires_window_evidence");
XA_WDD_TEST("association.ambiguous_count_uses_title_match");
XA_WDD_TEST("association.excluded_title_is_not_selected");
XA_WDD_TEST("association.status_detects_codex_among_unrelated_terminals");
XA_WDD_TEST("thread.named_footer_is_extracted");
XA_WDD_TEST("thread.indexed_name_beats_trailing_git_branch");
XA_WDD_TEST("thread.arbitrary_name_ignores_branch_and_goal");
XA_WDD_TEST("thread.inline_goal_annotation_preserves_thread_name");
XA_WDD_TEST("thread.status_annotations_preserve_identity");
XA_WDD_TEST("status.annotations_do_not_contaminate_fields");
XA_WDD_TEST("thread.named_uuid_resolves_through_index");
XA_WDD_TEST("thread.unnamed_uuid_stays_unnamed");
XA_WDD_TEST("thread.uuid_with_trailing_status_stays_bounded");
XA_WDD_TEST("thread.rename_message_is_fallback");
XA_WDD_TEST("thread.named_threads_sort_with_unnamed_last");
XA_WDD_TEST("status.normal_statusline_fields_are_extracted");
XA_WDD_TEST("status.latest_normal_statusline_wins");
XA_WDD_TEST("status.thinking_is_recognized_as_active_work");
XA_WDD_TEST("status.waiting_preserves_footer_and_detection");
XA_WDD_TEST("status.used_tokens_field_is_extracted");
XA_WDD_TEST("status.metadata_is_not_mistaken_for_thread_name");
XA_WDD_TEST("status.footer_weekly_is_metadata_only");
XA_WDD_TEST("usage.weekly_window_uses_true_response");
XA_WDD_TEST("usage.request_uses_codex_auth_and_configured_endpoint");
XA_WDD_TEST("uptime.compact_process_age_is_formatted");
XA_WDD_TEST("uptime.uses_unique_codex_pid_and_rejects_ambiguous_matches");
XA_WDD_TEST("config.loads_validated_overrides");
XA_WDD_TEST("privacy.session_index_reads_names_only");
XA_WDD_TEST("windows.monitor_and_process_enumeration_read_only");
XA_WDD_TEST("windows.accessibility_failure_is_bounded_and_safe");
XA_WDD_TEST("windows.detection_inspects_generic_titles_before_filtering");
XA_WDD_TEST("windows.temporary_owned_window_can_move");
XA_WDD_TEST("windows.log_writes_bounded_local_entry");
XA_WDD_TEST("format.plan_uses_thread_identity");
#undef XA_WDD_TEST

constexpr Rect kWorkArea{0, 0, 2560, 1392};

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class ScopedEnvironmentOverride {
public:
    ScopedEnvironmentOverride(std::wstring name, const std::wstring& value)
        : name_(std::move(name)) {
        wchar_t* current = nullptr;
        std::size_t length = 0;
        if (_wdupenv_s(&current, &length, name_.c_str()) == 0 && current) {
            previous_ = current;
            std::free(current);
        }
        if (_wputenv_s(name_.c_str(), value.c_str()) != 0) {
            throw std::runtime_error("could not set test environment override");
        }
    }

    ~ScopedEnvironmentOverride() {
        _wputenv_s(name_.c_str(), previous_.value_or(L"").c_str());
    }

    ScopedEnvironmentOverride(const ScopedEnvironmentOverride&) = delete;
    ScopedEnvironmentOverride& operator=(const ScopedEnvironmentOverride&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

WindowInfo Candidate(std::uintptr_t hwnd, std::wstring title = L"Project") {
    WindowInfo window;
    window.hwnd = hwnd;
    window.title = std::move(title);
    window.class_name = L"CASCADIA_HOSTING_WINDOW_CLASS";
    window.process_id = 96068;
    window.process_name = L"WindowsTerminal.exe";
    window.process_path = L"C:\\Program Files\\WindowsApps\\WindowsTerminal.exe";
    window.rect = {100, 100, 1229, 735};
    window.z_order = static_cast<int>(hwnd);
    return window;
}

bool Overlaps(const Rect& first, const Rect& second) {
    return !(first.right <= second.left || second.right <= first.left ||
             first.bottom <= second.top || second.bottom <= first.top);
}

void RequireValidPlan(int count) {
    const LayoutPlan plan = CalculateLayout(count, kWorkArea, Config{});
    Require(plan.rectangles.size() == static_cast<std::size_t>(count),
            "rectangle count mismatch");
    for (const Rect& rectangle : plan.rectangles) {
        Require(kWorkArea.Contains(rectangle), "rectangle escaped the work area");
        Require(rectangle.Width() > 0 && rectangle.Height() > 0,
                "rectangle dimensions must be positive");
    }
    for (std::size_t first = 0; first < plan.rectangles.size(); ++first) {
        for (std::size_t second = first + 1; second < plan.rectangles.size(); ++second) {
            Require(!Overlaps(plan.rectangles[first], plan.rectangles[second]),
                    "rectangles overlap");
        }
    }
}

void TestTwoWindowsKeepPreferredSize() {
    const LayoutPlan plan = CalculateLayout(2, kWorkArea, Config{});
    Require(plan.columns == 2 && plan.rows == 1, "two-window grid mismatch");
    for (const Rect& rectangle : plan.rectangles) {
        Require(rectangle.Width() == 1129 && rectangle.Height() == 635,
                "preferred geometry was not retained");
    }
    RequireValidPlan(2);
}

void TestThreeWindowsCenterFinalRow() {
    const LayoutPlan plan = CalculateLayout(3, kWorkArea, Config{});
    Require(plan.columns == 2 && plan.rows == 2, "three-window grid mismatch");
    const Rect& last = plan.rectangles.back();
    const int doubled_center = last.left + last.right;
    Require(std::abs(doubled_center - kWorkArea.Width()) <= 2,
            "final row is not centered");
    RequireValidPlan(3);
}

void TestFourWindowsKeepPreferredSize() {
    const LayoutPlan plan = CalculateLayout(4, kWorkArea, Config{});
    Require(plan.columns == 2 && plan.rows == 2, "four-window grid mismatch");
    for (const Rect& rectangle : plan.rectangles) {
        Require(rectangle.Width() == 1129 && rectangle.Height() == 635,
                "four-window preferred size mismatch");
    }
    RequireValidPlan(4);
}

void TestEightWindowsShrinkHeight() {
    const LayoutPlan plan = CalculateLayout(8, kWorkArea, Config{});
    Require(plan.columns == 2 && plan.rows == 4, "eight-window grid mismatch");
    for (const Rect& rectangle : plan.rectangles) {
        Require(rectangle.Width() == 1129 && rectangle.Height() < 635,
                "eight-window geometry mismatch");
    }
    RequireValidPlan(8);
}

void TestLargerCountsAddColumns() {
    const LayoutPlan plan = CalculateLayout(14, kWorkArea, Config{});
    Require(plan.columns == 3, "fourteen windows should use three columns");
    RequireValidPlan(14);
}

void TestPracticalCountsStayVisible() {
    for (int count = 1; count <= 40; ++count) RequireValidPlan(count);
}

void TestExactCountRequiresWindowEvidence() {
    std::vector<WindowInfo> candidates;
    for (std::uintptr_t index = 1; index <= 8; ++index) candidates.push_back(Candidate(index));
    const AssociationResult result = AssociateCodexWindows(
        std::move(candidates), 8, {L"codex"}, {});
    Require(result.mode == L"manual" && !result.warning.empty(),
            "process counts alone must not identify Codex windows");
    Require(std::count_if(result.windows.begin(), result.windows.end(),
                          [](const WindowInfo& window) { return window.auto_selected; }) == 0,
            "matching counts selected unrelated terminals");
}

void TestAmbiguousCountUsesTitleMatch() {
    const AssociationResult result = AssociateCodexWindows(
        {Candidate(1), Candidate(2, L"Codex - project"), Candidate(3, L"PowerShell")},
        2, {L"codex"}, {});
    Require(result.mode == L"title-match",
            "ambiguous association mode mismatch");
    std::vector<std::uintptr_t> selected;
    for (const WindowInfo& window : result.windows) {
        if (window.auto_selected) selected.push_back(window.hwnd);
    }
    Require(selected == std::vector<std::uintptr_t>{2}, "wrong title-matched window");
}

void TestExcludedTitleIsNotSelected() {
    auto included = Candidate(1);
    included.codex_status = L"Ready";
    included.codex_model = L"gpt-6-astra xhigh";
    auto excluded = included;
    excluded.hwnd = 2;
    excluded.title = L"Codex LB";
    const AssociationResult result = AssociateCodexWindows(
        {included, excluded}, 1, {L"codex"}, {L"codex lb"});
    Require(result.mode == L"status-line", "status evidence was not used");
    Require(result.windows[0].auto_selected && !result.windows[1].auto_selected,
            "excluded window selection mismatch");
}

void TestNamedFooterIsExtracted() {
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Working (3m)\nWorking \u00b7 gpt-5.6-sol xhigh \u00b7 weekly 90% left \u00b7 Sample catalog\n");
    Require(identity.name == L"Sample catalog" && identity.source == L"footer-name",
            "named footer parse failed");
}

void TestStatusDetectsCodexAmongUnrelatedTerminals() {
    std::vector<WindowInfo> candidates;
    for (std::uintptr_t index = 1; index <= 6; ++index) {
        WindowInfo window = Candidate(index);
        if (index <= 4) {
            const auto status = ParseCodexStatusLine(
                L"Working \u00b7 gpt-6-astra xhigh \u00b7 weekly 93% left \u00b7 60.4K used \u00b7 A project\n");
            window.codex_status = status.status;
            window.codex_model = status.model;
        }
        candidates.push_back(std::move(window));
    }
    const auto result = AssociateCodexWindows(std::move(candidates), 4, {L"codex"}, {});
    std::vector<std::uintptr_t> selected;
    for (const auto& window : result.windows) {
        if (window.auto_selected) selected.push_back(window.hwnd);
    }
    Require(selected == std::vector<std::uintptr_t>{1, 2, 3, 4},
            "four Codex status lines were not detected among six generic-titled terminals");
}

void TestIndexedNameBeatsTrailingGitBranch() {
    const std::wstring id = L"00000000-0000-7000-8000-000000000005";
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Working \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 96% left \u00b7 "
        L"19.1M used \u00b7 B - Search index \u00b7 Main [default]\n",
        {{id, L"B - Search index"}});
    Require(identity.name == L"B - Search index" && identity.thread_id == id &&
                identity.source == L"footer-name",
            "trailing git branch replaced the indexed thread name");
}

void TestArbitraryNameIgnoresBranchAndGoal() {
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Working \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 89% left \u00b7 "
        L"91.3M used \u00b7 anything I named this \u00b7 Main [default] Pursuing goal (4h 14m)\n");
    Require(identity.name == L"anything I named this" && identity.source == L"footer-name",
            "branch/goal metadata replaced an arbitrary footer thread name");
}

void TestInlineGoalAnnotationPreservesThreadName() {
    struct Case {
        const wchar_t* footer;
        const wchar_t* expected_name;
    };
    const std::vector<Case> cases{
        {L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 87% left \u00b7 "
         L"B - Dashboard                           Goal achieved (53m)\r\n",
         L"B - Dashboard"},
        {L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 87% left \u00b7 "
         L"C - Editor                       Goal stalled (/goal resume)\r\n",
         L"C - Editor"},
        {L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 87% left \u00b7 "
         L"0 - Notes                        Goal paused (/goal resume)\r\n",
         L"0 - Notes"},
        {L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 "
         L"project without quota                    Goal paused (/goal resume)\r\n",
         L"project without quota"},
    };
    for (const Case& test : cases) {
        const ThreadIdentity identity = ParseThreadIdentity(test.footer);
        Require(identity.name == test.expected_name && identity.source == L"footer-name",
                "inline goal annotation discarded or contaminated the thread name");
    }
}

void TestStatusAnnotationsPreserveIdentity() {
    const std::wstring id = L"00000000-0000-7000-8000-000000000005";
    const std::wstring prefix = L"Ready \u00b7 gpt-6-astra ultra fast \u00b7 60.4K used \u00b7 ";
    const std::vector<std::wstring> annotations{
        L"Goal achieved", L"Goal achieved (53m)", L"GOAL ACHIEVED",
        L"Goal stalled (/goal resume)", L"Goal paused (/goal resume)",
        L"Pursuing goal (4h 14m)", L"\u26a0 1 warning", L"\u26a0 12 warnings",
        L"\u26a0\ufe0f 2 warnings", L"\u26a0\ufe0e 1 WARNING",
        L"Goal achieved \u26a0 1 warning", L"\u26a0 2 warnings Goal achieved (53m)"};
    for (const auto& annotation : annotations) {
        for (const auto* separator : {L" ", L"    ", L" \u00b7 "}) {
            const std::wstring footer = prefix + L"Codex Organizer" + separator + annotation;
            const auto identity = ParseThreadIdentity(footer);
            Require(identity.name == L"Codex Organizer" && identity.source == L"footer-name",
                    "status annotation contaminated the thread name");
            const auto indexed = ParseThreadIdentity(footer, {{id, L"Codex Organizer"}});
            Require(indexed.name == L"Codex Organizer" && indexed.thread_id == id,
                    "status annotation prevented indexed thread lookup");
        }
        const auto unnamed = ParseThreadIdentity(prefix + annotation);
        Require(unnamed.name.empty() && unnamed.thread_id.empty(),
                "status annotation became an unnamed session's identity");
        const auto preceding = ParseThreadIdentity(prefix + annotation + L" \u00b7 Codex Organizer");
        Require(preceding.name == L"Codex Organizer",
                "status annotation before the thread name became its identity");
        const auto uuid = ParseThreadIdentity(prefix + id + L" " + annotation);
        Require(uuid.name.empty() && uuid.thread_id == id,
                "status annotation changed an unnamed UUID identity");
    }
    for (const auto* badge : {L"1 warning", L"12 warnings"}) {
        Require(ParseThreadIdentity(prefix + badge).name.empty(),
                "plain warning count became a thread name");
        Require(ParseThreadIdentity(prefix + L"Codex Organizer    " + badge).name == L"Codex Organizer",
                "padded plain warning count contaminated the thread name");
    }
    for (const auto* name : {L"Fix 1 warning", L"Warning handling", L"Investigate Goal achieved detection"}) {
        Require(ParseThreadIdentity(prefix + name).name == name,
                "ordinary thread name text was removed as an annotation");
    }
}

void TestAnnotationsDoNotContaminateFields() {
    const auto status = ParseCodexStatusLine(
        L"Ready Goal achieved \u00b7 gpt-6-astra ultra fast \u26a0 1 warning \u00b7 60.4K used\n");
    Require(status.status == L"Ready" && status.model == L"gpt-6-astra ultra fast" &&
                status.used_tokens == L"60.4K",
            "inline annotations contaminated status/model/token fields");
    const auto unnamed = ParseThreadIdentity(
        L"Ready \u00b7 gpt-6-astra ultra fast \u00b7 Goal achieved \u00b7 \u26a0 1 warning\n");
    Require(unnamed.name.empty() && unnamed.thread_id.empty(),
            "combined goal and warning metadata became a thread identity");
}

void TestNamedUuidResolvesThroughIndex() {
    const std::wstring id = L"00000000-0000-7000-8000-000000000004";
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 weekly 90% left \u00b7 " + id,
        {{id, L"Sample catalog"}});
    Require(identity.name == L"Sample catalog" && identity.thread_id == id &&
                identity.source == L"footer-id",
            "UUID footer lookup failed");
}

void TestUnnamedUuidStaysUnnamed() {
    const std::wstring id = L"00000000-0000-7000-8000-000000000002";
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 weekly 90% left \u00b7 " + id);
    Require(identity.name.empty() && identity.thread_id == id,
            "unnamed UUID identity mismatch");
}

void TestUuidWithTrailingStatusStaysBounded() {
    const std::wstring id = L"00000000-0000-7000-8000-000000000003";
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 weekly 90% left \u00b7 " + id +
        L"  Pursuing goal (22m)",
        {{id, L"Codex Organizer"}});
    Require(identity.name == L"Codex Organizer" && identity.thread_id == id &&
                identity.source == L"footer-id",
            "UUID with trailing footer status was not bounded to identity fields");
}

void TestRenameMessageIsFallback() {
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Session renamed to Search index. To resume this session run codex resume, "
        L"then select Search index (00000000-0000-7000-8000-000000000001)");
    Require(identity.name == L"Search index" && identity.source == L"rename-message",
            "rename fallback failed");
}

void TestNamedThreadsSortWithUnnamedLast() {
    const std::wstring id = L"00000000-0000-7000-8000-000000000002";
    std::vector<WindowInfo> windows{Candidate(1), Candidate(2), Candidate(3), Candidate(4)};
    windows[0].thread_name = L"Writer";
    windows[1].thread_id = id;
    windows[2].thread_name = L"Dashboard";
    windows[3].thread_name = L"Search index";
    SortWindowsByThread(windows, true);
    const std::vector<std::wstring> expected{
        L"Dashboard", L"Search index", L"Writer", L"(unnamed) 00000000"};
    std::vector<std::wstring> actual;
    for (const WindowInfo& window : windows) actual.push_back(window.ThreadLabel());
    Require(actual == expected, "thread ordering mismatch");
}

void TestNormalStatusLineFieldsAreExtracted() {
    for (const std::wstring model : {L"gpt-6-astra ultra fast", L"gpt-5.6-sol high"}) {
        for (const std::wstring fast_segment : {L"Fast on \u00b7 ", L"Fast off \u00b7 ", L""}) {
            const CodexStatusLine status = ParseCodexStatusLine(
                L"Working (3m)\nWorking \u00b7 " + model + L" \u00b7 " + fast_segment +
                L"weekly 39% left \u00b7 137K used\n");
            Require(status.status == L"Working", "normal status field was not extracted");
            Require(status.model == model, "model label must preserve only the model segment");
            Require(status.used_tokens == L"137K", "ignored Fast metadata changed used tokens");
            Require(status.weekly.empty(), "footer weekly text must remain metadata only");
        }
    }
}

void TestLatestNormalStatusLineWins() {
    const CodexStatusLine status = ParseCodexStatusLine(
        L"Ready \u00b7 gpt-5.5 medium \u00b7 Fast off \u00b7 weekly 80% left\n"
        L"terminal output\n"
        L"Working \u00b7 gpt-5.6-sol high \u00b7 weekly 39% left\n");
    Require(status.status == L"Working" && status.model == L"gpt-5.6-sol high",
            "latest normal status line was not selected");
}

void TestUsedTokensFieldIsExtracted() {
    const CodexStatusLine status = ParseCodexStatusLine(
        L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 97% left \u00b7 "
        L"A - Codex Organizer \u00b7 1.19M used\n");
    Require(status.used_tokens == L"1.19M", "compact used-token field was not extracted");
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 97% left \u00b7 "
        L"A - Codex Organizer \u00b7 1.19M used\n");
    Require(identity.name == L"A - Codex Organizer",
            "used-token metadata was mistaken for the thread name");
    const CodexStatusLine absent = ParseCodexStatusLine(
        L"Ready \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 97% left \u00b7 "
        L"A - Codex Organizer\n");
    Require(absent.used_tokens.empty(),
            "missing used-token status item must not produce an estimate");
}

void TestThinkingIsRecognizedAsActiveWork() {
    const std::wstring footer =
        L"Thinking \u00b7 gpt-6-astra ultra fast \u00b7 weekly 80% left \u00b7 1.8M used \u00b7 "
        L"Check Dalamud updates \u00b7 Main [default]\n";
    const auto status = ParseCodexStatusLine(
        L"Ready \u00b7 gpt-5.5 medium \u00b7 Previous thread\n" + footer);
    Require(status.status == L"Thinking" && status.model == L"gpt-6-astra ultra fast" &&
                status.used_tokens == L"1.8M",
            "Thinking must replace the older Ready line and retain model and tokens");
    const auto identity = ParseThreadIdentity(footer);
    Require(identity.name == L"Check Dalamud updates",
            "Thinking activity or the git branch was mistaken for the thread name");
    const auto unnamed = ParseThreadIdentity(L"Thinking \u00b7 gpt-6-astra ultra fast\n");
    Require(unnamed.name.empty() && unnamed.thread_id.empty(),
            "Thinking metadata became an unnamed session's name");
    const auto lower = ParseCodexStatusLine(L"thinking \u00b7 gpt-6-astra ultra fast\n");
    Require(lower.status == L"thinking", "Thinking recognition must be case-insensitive");
}

void TestWaitingPreservesFooterAndDetection() {
    const std::wstring footer =
        L"Waiting \u00b7 gpt-6-astra medium \u00b7 weekly 54% left \u00b7 421K used \u00b7 A - Search index\n";
    for (const auto* previous : {L"Ready", L"Working", L"Thinking"}) {
        const auto status = ParseCodexStatusLine(
            std::wstring(previous) + L" \u00b7 gpt-5.5 medium \u00b7 Previous thread\n" + footer);
        Require(status.status == L"Waiting" && status.model == L"gpt-6-astra medium" &&
                    status.used_tokens == L"421K",
                "Waiting must replace the older footer and retain model and tokens");
        Require(ParseThreadIdentity(footer).name == L"A - Search index",
                "Waiting footer lost the screenshot's thread name");
    }
    const auto unnamed = ParseThreadIdentity(L"Waiting \u00b7 gpt-6-astra medium\n");
    Require(unnamed.name.empty() && unnamed.thread_id.empty(),
            "Waiting metadata became an unnamed session's name");
    Require(ParseCodexStatusLine(L"waiting \u00b7 gpt-6-astra medium\n").status == L"waiting",
            "Waiting recognition must be case-insensitive");
    Require(ParseCodexStatusLine(L"Waiting for background terminal\n").status.empty(),
            "Ordinary waiting output must not become a Codex footer");
    const auto resumed = ParseCodexStatusLine(footer + L"Ready \u00b7 gpt-6-astra medium\n");
    Require(resumed.status == L"Ready", "Waiting must not override a newer Ready footer");
}

void TestCodexPidUptime() {
    constexpr std::uint64_t second = 10000000ULL;
    constexpr std::uint64_t now = 10000 * second;
    std::vector<ProcessInfo> processes{
        {11, 0, L"codex.exe", L"", now - 3600 * second},
        {22, 0, L"codex.exe", L"", now - 120 * second},
        {33, 0, L"codex.exe", L"", now - 30 * second}};
    std::vector<WindowInfo> windows{Candidate(1), Candidate(2), Candidate(3)};
    for (auto& window : windows) { window.process_id = 999; window.process_uptime = L"old host uptime"; }
    ApplyCodexProcessUptimes(windows, processes, {{11, 1}, {22, 2}}, now);
    Require(windows[0].codex_process_id == 11 && windows[0].process_uptime == L"1h" &&
            windows[1].codex_process_id == 22 && windows[1].process_uptime == L"2m",
            "Shared terminal host must show separate Codex PID uptimes");
    Require(windows[2].codex_process_id == 0 && windows[2].process_uptime == L"\u2014",
            "Unmatched window must not retain host uptime");
    ApplyCodexProcessUptimes(windows, processes, {{11, 1}, {22, 1}, {33, 2}}, now);
    Require(windows[0].codex_process_id == 0 && windows[0].process_uptime == L"\u2014" &&
            windows[1].codex_process_id == 33 && windows[1].process_uptime == L"<1m",
            "Multiple Codex processes in one window must not be guessed");
    processes[0].creation_time_100ns = 0;
    processes[1].creation_time_100ns = now + second;
    ApplyCodexProcessUptimes(windows, processes, {{11, 1}, {22, 2}}, now);
    Require(windows[0].codex_process_id == 0 && windows[1].codex_process_id == 0,
            "Missing or future process birth must not produce uptime");
    ApplyCodexProcessUptimes(windows, processes, {}, now);
    Require(windows[2].process_uptime == L"\u2014", "Failed probe must remain unknown");
}

void TestStatusMetadataIsNotMistakenForThreadName() {
    const ThreadIdentity identity = ParseThreadIdentity(
        L"Working \u00b7 gpt-5.6-sol ultra fast \u00b7 Fast on \u00b7 weekly 39% left\n");
    Require(identity.name.empty() && identity.thread_id.empty(),
            "status metadata was mistaken for a thread identity");
}

void TestFooterWeeklyIsMetadataOnly() {
    const CodexStatusLine status = ParseCodexStatusLine(
        L"Working \u00b7 gpt-5.6-sol xhigh \u00b7 Fast off \u00b7 weekly 95% left\n");
    Require(status.weekly.empty(),
            "terminal footer weekly value must not be used as account usage authority");
}

void TestWeeklyWindowUsesTrueResponse() {
    const std::string response = R"({
        "rate_limit": {
            "primary_window": {
                "used_percent": 13,
                "limit_window_seconds": 604800,
                "reset_at": 1788748045
            },
            "secondary_window": null
        }
    })";
    Require(ParseCodexWeeklyUsageResponse(response) == L"weekly 87% left",
            "true weekly response was not converted from used to remaining percent");

    const std::string reversed = R"({
        "rate_limit": {
            "primary_window": {"used_percent": 25.4, "limit_window_seconds": 18000},
            "secondary_window": {"used_percent": 40.6, "limit_window_seconds": 604800}
        }
    })";
    Require(ParseCodexWeeklyUsageResponse(reversed) == L"weekly 59% left",
            "weekly duration selection or decimal rounding did not match the usage service");
}

void TestUsageRequestUsesCodexAuthAndConfiguredEndpoint() {
    WSADATA winsock{};
    Require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "WSAStartup failed");
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(listener != INVALID_SOCKET, "local usage listener socket failed");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    Require(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
            "local usage listener bind failed");
    Require(listen(listener, 1) == 0, "local usage listener listen failed");
    int address_size = sizeof(address);
    Require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) == 0,
            "local usage listener port lookup failed");

    const std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        (L"codex-organizer-usage-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(home);
    {
        std::ofstream auth(home / L"auth.json", std::ios::binary | std::ios::trunc);
        auth << R"({"tokens":{"access_token":"test-access-token","account_id":"test-account"}})";
        std::ofstream config(home / L"config.toml", std::ios::binary | std::ios::trunc);
        config << "chatgpt_base_url = \"http://127.0.0.1:"
               << ntohs(address.sin_port) << "\"\n";
    }

    std::string captured_request;
    std::string server_error;
    const std::string body =
        R"({"rate_limit":{"primary_window":{"used_percent":7,"limit_window_seconds":604800},"secondary_window":null}})";
    std::thread server([&] {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            server_error = "local usage listener accept failed";
            return;
        }
        char buffer[2048]{};
        while (captured_request.find("\r\n\r\n") == std::string::npos &&
               captured_request.size() < 64 * 1024) {
            const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) break;
            captured_request.append(buffer, static_cast<std::size_t>(received));
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        std::size_t sent = 0;
        while (sent < response.size()) {
            const int count = send(client, response.data() + sent,
                                   static_cast<int>(response.size() - sent), 0);
            if (count <= 0) {
                server_error = "local usage listener send failed";
                break;
            }
            sent += static_cast<std::size_t>(count);
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
    });

    std::wstring weekly;
    std::exception_ptr request_error;
    {
        ScopedEnvironmentOverride override(L"CODEX_HOME", home.wstring());
        try {
            weekly = FetchCodexWeeklyUsage();
        } catch (...) {
            request_error = std::current_exception();
        }
    }
    closesocket(listener);
    if (server.joinable()) server.join();
    WSACleanup();
    std::error_code cleanup_error;
    std::filesystem::remove_all(home, cleanup_error);
    if (request_error) std::rethrow_exception(request_error);

    Require(server_error.empty(), server_error);
    Require(weekly == L"weekly 93% left", "configured usage request returned wrong weekly value");
    const std::string lowered = [&] {
        std::string value = captured_request;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }();
    Require(lowered.find("get /api/codex/usage ") != std::string::npos,
            "configured Codex usage path was not requested");
    Require(lowered.find("authorization: bearer test-access-token") != std::string::npos,
            "local Codex access token was not used");
    Require(lowered.find("chatgpt-account-id: test-account") != std::string::npos,
            "local Codex account header was not used");
}

void TestCompactProcessAgeIsFormatted() {
    Require(FormatUptime(0) == L"<1m", "sub-minute uptime mismatch");
    Require(FormatUptime(59 * 60) == L"59m", "minute uptime mismatch");
    Require(FormatUptime((4 * 60 + 13) * 60) == L"4h 13m", "hour uptime mismatch");
    Require(FormatUptime((2 * 24 + 3) * 60 * 60) == L"2d 3h", "day uptime mismatch");
}

void TestConfigLoadsValidatedOverrides() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / L"codex-organizer-native-test-config.json";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "{\"preferred_width\":900,\"monitor\":2,\"organize_by_thread_name\":false,"
                  "\"request_account_usage\":false,"
                  "\"title_include_patterns\":[\"codex\",\"organizer\"]}";
    }
    const Config config = LoadConfig(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    Require(config.preferred_width == 900 && config.monitor == L"2" &&
                !config.organize_by_thread_name && !config.request_account_usage &&
                config.title_include_patterns.size() == 2,
            "config overrides were not applied");
}

void TestSessionIndexReadsNamesOnly() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / L"codex-organizer-native-session-index.jsonl";
    const std::wstring id = L"00000000-0000-7000-8000-000000000002";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "not json\n"
               << "{\"id\":\"00000000-0000-7000-8000-000000000002\","
                  "\"thread_name\":\"Sample catalog\",\"transcript\":\"ignored\"}\n";
    }
    const auto names = LoadThreadNameIndex(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    Require(names.size() == 1 && names.at(id) == L"Sample catalog",
            "session index identity extraction failed");
}

void TestMonitorAndProcessEnumerationReadOnly() {
    const auto monitors = EnumerateMonitors();
    Require(!monitors.empty(), "no monitors were enumerated");
    const MonitorInfo& primary = ResolveMonitor(monitors, L"primary");
    Require(primary.work_area.Width() > 0 && primary.work_area.Height() > 0,
            "primary work area is invalid");
    const auto processes = EnumerateProcesses();
    Require(processes.contains(GetCurrentProcessId()), "current process missing from snapshot");
    Require(processes.at(GetCurrentProcessId()).creation_time_100ns > 0,
            "current process creation time was not captured");
}

void TestAccessibilityFailureIsBoundedAndSafe() {
    const auto [text, error] = ReadTerminalTextTail(0, 4096);
    Require(text.empty() && !error.empty(), "invalid HWND accessibility read did not fail safely");
    const auto [oversized_text, oversized_error] = ReadTerminalTextTail(0, 65537);
    Require(oversized_text.empty() && !oversized_error.empty(),
            "oversized accessibility request was not rejected");
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                                  L"Codex Organizer accessibility test", WS_POPUP,
                                  0, 0, 100, 50, nullptr, nullptr, GetModuleHandleW(nullptr),
                                  nullptr);
    Require(window != nullptr, "could not create temporary accessibility window");
    const auto [plain_text, plain_error] =
        ReadTerminalTextTail(reinterpret_cast<std::uintptr_t>(window), 4096);
    DestroyWindow(window);
    Require(plain_text.empty() && !plain_error.empty(),
            "non-terminal window did not fail accessibility discovery safely");
}

void TestTemporaryOwnedWindowCanMove() {
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                                  L"Codex Organizer movement test", WS_POPUP,
                                  10, 10, 160, 90, nullptr, nullptr, GetModuleHandleW(nullptr),
                                  nullptr);
    Require(window != nullptr, "could not create temporary movement window");
    Config config;
    config.restore_minimized = false;
    config.bring_to_front = false;
    WindowInfo info = Candidate(reinterpret_cast<std::uintptr_t>(window));
    const Rect target{40, 40, 240, 160};
    const auto results = MoveWindows({info}, {target}, config);
    DestroyWindow(window);
    Require(results.size() == 1 && results.front().error.empty() && results.front().actual,
            "temporary window movement failed");
}

void TestDetectionInspectsGenericTitlesBeforeFiltering() {
    struct OwnedTerminals {
        const wchar_t* class_name = L"CodexOrganizerDetectionRegression";
        HINSTANCE instance = GetModuleHandleW(nullptr);
        std::vector<HWND> windows;
        OwnedTerminals() {
            WNDCLASSW definition{};
            definition.lpfnWndProc = DefWindowProcW;
            definition.hInstance = instance;
            definition.lpszClassName = class_name;
            Require(RegisterClassW(&definition) != 0, "test class registration failed");
        }
        ~OwnedTerminals() {
            for (auto window : windows) DestroyWindow(window);
            UnregisterClassW(class_name, instance);
        }
    } owned;
    for (int index = 0; index < 7; ++index) {
        HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, owned.class_name,
                                      index == 6 ? L"Codex LB" : L"Project", WS_POPUP | WS_VISIBLE,
                                      0, 0, 10, 10, nullptr, nullptr, owned.instance, nullptr);
        Require(window != nullptr, "test terminal creation failed");
        owned.windows.push_back(window);
    }
    Config config;
    config.terminal_class_names = {owned.class_name};
    config.codex_process_names = {L"codex_organizer_tests.exe"};
    config.request_account_usage = false;
    config.organize_by_thread_name = false;
    std::size_t reads = 0;
    const TerminalTextReader reader = [&](std::uintptr_t hwnd, int characters) {
        Require(characters == 4096, "terminal read exceeded configured bound");
        const auto found = std::find(owned.windows.begin(), owned.windows.end(),
                                     reinterpret_cast<HWND>(hwnd));
        Require(found != owned.windows.end(), "read escaped test-owned terminals");
        const auto index = found - owned.windows.begin();
        Require(index != 6, "excluded title was inspected");
        ++reads;
        if (index == 5) return std::pair(std::wstring(), std::wstring(L"access denied"));
        if (index == 4) return std::pair(std::wstring(L"C:\\Windows\\System32>\n"), std::wstring());
        return std::pair(std::wstring(index == 0 ? L"Thinking" : index == 1 ? L"Waiting" : L"Ready") +
            L" \u00b7 gpt-6-astra xhigh \u00b7 weekly 93% left \u00b7 60.4K used \u00b7 "
            L"00000000-0000-7000-8000-000000000006\n", std::wstring());
    };
    const auto detected = DetectCodexWindows(config, false, reader);
    Require(reads == 6, "generic-titled terminals were not all inspected before association");
    Require(detected.windows.size() == 4 && detected.AutoWindows().size() == 4,
            "default detection must show only the four Codex terminals");
    Require(!detected.thread_warning.empty(), "accessibility failure was not reported");
    for (const auto& window : detected.windows) {
        const auto expected_status = window.hwnd == reinterpret_cast<std::uintptr_t>(owned.windows[0])
                                         ? L"Thinking" : window.hwnd == reinterpret_cast<std::uintptr_t>(owned.windows[1])
                                         ? L"Waiting" : L"Ready";
        Require(window.codex_status == expected_status && window.codex_model == L"gpt-6-astra xhigh" &&
                    window.codex_used_tokens == L"60.4K" && window.thread_name.empty(),
                "status detection must work with thread ordering disabled");
    }
    const auto all = DetectCodexWindows(config, true, reader);
    Require(reads == 12 && all.windows.size() == 7 && all.AutoWindows().size() == 4,
            "explicit all-terminals override lost candidates or changed Codex classification");
    config.organize_by_thread_name = true;
    const auto ordered = DetectCodexWindows(config, false, reader);
    Require(ordered.windows.size() == 4, "thread ordering changed detection");
    for (const auto& window : ordered.windows) {
        Require(window.thread_id == L"00000000-0000-7000-8000-000000000006",
                "unnamed Codex UUID was lost during detection");
    }
}

void TestLogWritesBoundedLocalEntry() {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / L"codex-organizer-native-log-test";
    std::error_code error;
    std::filesystem::remove_all(base, error);
    WriteLog(base, L"INFO", L"native log regression marker");
    const std::filesystem::path log = base / L"logs" / L"codex-organizer.log";
    Require(std::filesystem::is_regular_file(log), "native log file was not created");
    std::ifstream stream(log, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    std::filesystem::remove_all(base, error);
    Require(content.find("INFO native log regression marker") != std::string::npos,
            "native log entry is missing its bounded marker");
}

void TestFormatPlanUsesThreadIdentity() {
    WindowInfo window = Candidate(42);
    window.thread_name = L"Dashboard";
    const MonitorInfo monitor{1, L"DISPLAY1", kWorkArea, kWorkArea, true};
    const LayoutPlan plan = CalculateLayout(1, kWorkArea, Config{});
    const std::wstring output = FormatPlan({window}, monitor, plan);
    Require(output.find(L"Dashboard") != std::wstring::npos &&
                output.find(L"1 columns x 1 rows") != std::wstring::npos,
            "formatted plan is missing identity or geometry");
}

struct TestCase {
    std::string name;
    std::function<void()> function;
};

const std::vector<TestCase>& Tests() {
    static const std::vector<TestCase> tests{
        {"layout.two_windows_keep_preferred_size", TestTwoWindowsKeepPreferredSize},
        {"layout.three_windows_center_final_row", TestThreeWindowsCenterFinalRow},
        {"layout.four_windows_keep_preferred_size", TestFourWindowsKeepPreferredSize},
        {"layout.eight_windows_shrink_height", TestEightWindowsShrinkHeight},
        {"layout.larger_counts_add_columns", TestLargerCountsAddColumns},
        {"layout.practical_counts_stay_visible", TestPracticalCountsStayVisible},
        {"association.exact_count_requires_window_evidence", TestExactCountRequiresWindowEvidence},
        {"association.ambiguous_count_uses_title_match", TestAmbiguousCountUsesTitleMatch},
        {"association.excluded_title_is_not_selected", TestExcludedTitleIsNotSelected},
        {"association.status_detects_codex_among_unrelated_terminals", TestStatusDetectsCodexAmongUnrelatedTerminals},
        {"thread.named_footer_is_extracted", TestNamedFooterIsExtracted},
        {"thread.indexed_name_beats_trailing_git_branch", TestIndexedNameBeatsTrailingGitBranch},
        {"thread.arbitrary_name_ignores_branch_and_goal", TestArbitraryNameIgnoresBranchAndGoal},
        {"thread.inline_goal_annotation_preserves_thread_name", TestInlineGoalAnnotationPreservesThreadName},
        {"thread.status_annotations_preserve_identity", TestStatusAnnotationsPreserveIdentity},
        {"status.annotations_do_not_contaminate_fields", TestAnnotationsDoNotContaminateFields},
        {"thread.named_uuid_resolves_through_index", TestNamedUuidResolvesThroughIndex},
        {"thread.unnamed_uuid_stays_unnamed", TestUnnamedUuidStaysUnnamed},
        {"thread.uuid_with_trailing_status_stays_bounded", TestUuidWithTrailingStatusStaysBounded},
        {"thread.rename_message_is_fallback", TestRenameMessageIsFallback},
        {"thread.named_threads_sort_with_unnamed_last", TestNamedThreadsSortWithUnnamedLast},
        {"status.normal_statusline_fields_are_extracted", TestNormalStatusLineFieldsAreExtracted},
        {"status.latest_normal_statusline_wins", TestLatestNormalStatusLineWins},
        {"status.thinking_is_recognized_as_active_work", TestThinkingIsRecognizedAsActiveWork},
        {"status.waiting_preserves_footer_and_detection", TestWaitingPreservesFooterAndDetection},
        {"status.used_tokens_field_is_extracted", TestUsedTokensFieldIsExtracted},
        {"status.metadata_is_not_mistaken_for_thread_name", TestStatusMetadataIsNotMistakenForThreadName},
        {"status.footer_weekly_is_metadata_only", TestFooterWeeklyIsMetadataOnly},
        {"usage.weekly_window_uses_true_response", TestWeeklyWindowUsesTrueResponse},
        {"usage.request_uses_codex_auth_and_configured_endpoint", TestUsageRequestUsesCodexAuthAndConfiguredEndpoint},
        {"uptime.compact_process_age_is_formatted", TestCompactProcessAgeIsFormatted},
        {"uptime.uses_unique_codex_pid_and_rejects_ambiguous_matches", TestCodexPidUptime},
        {"config.loads_validated_overrides", TestConfigLoadsValidatedOverrides},
        {"privacy.session_index_reads_names_only", TestSessionIndexReadsNamesOnly},
        {"windows.monitor_and_process_enumeration_read_only", TestMonitorAndProcessEnumerationReadOnly},
        {"windows.accessibility_failure_is_bounded_and_safe", TestAccessibilityFailureIsBoundedAndSafe},
        {"windows.detection_inspects_generic_titles_before_filtering", TestDetectionInspectsGenericTitlesBeforeFiltering},
        {"windows.temporary_owned_window_can_move", TestTemporaryOwnedWindowCanMove},
        {"windows.log_writes_bounded_local_entry", TestLogWritesBoundedLocalEntry},
        {"format.plan_uses_thread_identity", TestFormatPlanUsesThreadIdentity},
    };
    return tests;
}

}  // namespace

int main(int argc, char** argv) {
    const auto& tests = Tests();
    if (argc == 2 && std::string(argv[1]) == "--list") {
        for (const TestCase& test : tests) std::cout << "TEST " << test.name << '\n';
        return 0;
    }
    std::vector<const TestCase*> selected;
    if (argc == 1) {
        for (const TestCase& test : tests) selected.push_back(&test);
    } else if (argc == 2) {
        const auto iterator = std::find_if(tests.begin(), tests.end(), [&](const TestCase& test) {
            return test.name == argv[1];
        });
        if (iterator == tests.end()) {
            std::cerr << "Unknown test: " << argv[1] << '\n';
            return 2;
        }
        selected.push_back(&*iterator);
    } else {
        std::cerr << "Usage: codex_organizer_tests.exe [--list|exact-test-name]\n";
        return 2;
    }

    int failures = 0;
    for (const TestCase* test : selected) {
        try {
            test->function();
            std::cout << "[PASS] " << test->name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test->name << ": " << error.what() << '\n';
        }
    }
    std::cout << "Ran " << selected.size() << " test(s); " << failures << " failed.\n";
    return failures == 0 ? 0 : 1;
}
