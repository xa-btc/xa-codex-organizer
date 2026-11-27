# Codex Organizer native GUI smoke test v0.3.0
# Starts the exact Release executable, waits for the native main window, verifies
# the title/minimum geometry, and closes only that exact process normally.
# Last Updated: 2026-08-30

$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CodexOrganizerGuiProbe {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, System.Text.StringBuilder className, int maximumCount);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int maximumCount);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
}
'@

function Wait-OrganizerDialog {
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        Start-Sleep -Milliseconds 100
        $dialog = [CodexOrganizerGuiProbe]::FindWindow('#32770', 'Codex Window Organizer')
        if ($dialog -ne [IntPtr]::Zero) { return $dialog }
    }
    throw 'Expected organizer dialog did not appear.'
}

function Invoke-DialogAction([IntPtr]$Window, [int]$Command, [int]$DialogResponse) {
    if (-not [CodexOrganizerGuiProbe]::PostMessage($Window, 0x0111, [IntPtr]$Command, [IntPtr]::Zero)) {
        throw "Could not post GUI command $Command."
    }
    $dialog = Wait-OrganizerDialog
    if ($DialogResponse -eq 0) {
        [CodexOrganizerGuiProbe]::PostMessage($dialog, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    }
    else {
        [CodexOrganizerGuiProbe]::PostMessage($dialog, 0x0111, [IntPtr]$DialogResponse, [IntPtr]::Zero) | Out-Null
    }
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        Start-Sleep -Milliseconds 100
        if ([CodexOrganizerGuiProbe]::FindWindow('#32770', 'Codex Window Organizer') -eq [IntPtr]::Zero) { return }
    }
    throw "Organizer dialog for command $Command did not close."
}

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = Join-Path $projectRoot 'build\Release\CodexOrganizer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Missing Release executable: $executable"
}

$temporaryConfig = Join-Path ([System.IO.Path]::GetTempPath()) "codex-organizer-gui-smoke-$PID.json"
$testConfig = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'config.json') | ConvertFrom-Json
$testConfig.confirm_before_move = $true
$testConfig.request_account_usage = $false
$testConfig | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporaryConfig -Encoding UTF8
$process = Start-Process -FilePath $executable -ArgumentList @('--config', $temporaryConfig) -PassThru
try {
    $handle = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 150; ++$attempt) {
        Start-Sleep -Milliseconds 200
        $process.Refresh()
        if ($process.HasExited) { throw "GUI exited early with code $($process.ExitCode)." }
        $handle = $process.MainWindowHandle
        if ($handle -ne [IntPtr]::Zero) { break }
    }
    if ($handle -eq [IntPtr]::Zero) { throw 'Native GUI main window did not appear.' }
    if ($process.MainWindowTitle -ne 'Codex Window Organizer 0.3.0') {
        throw "Unexpected GUI title: $($process.MainWindowTitle)"
    }
    $rect = New-Object CodexOrganizerGuiProbe+Rect
    if (-not [CodexOrganizerGuiProbe]::GetWindowRect($handle, [ref]$rect)) {
        throw 'GetWindowRect failed for the native GUI.'
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 820 -or $height -lt 500) {
        throw "GUI opened below the supported minimum: ${width}x${height}."
    }

    $windowList = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        $windowList = [CodexOrganizerGuiProbe]::GetDlgItem($handle, 1003)
        if ($windowList.ToInt64() -ne 0 -and
            [CodexOrganizerGuiProbe]::SendMessage(
                $windowList, 0x101D, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64() -gt 0) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if ($windowList.ToInt64() -eq 0) { throw 'Window list was not created.' }
    $expectedColumns = @('Uptime', 'Thread name (A-Z)', 'Status', 'Model', 'Used tokens')
    for ($column = 0; $column -lt $expectedColumns.Count; ++$column) {
        $columnWidth = [CodexOrganizerGuiProbe]::SendMessage(
            $windowList, 0x101D, [IntPtr]$column, [IntPtr]::Zero).ToInt64()
        if ($columnWidth -le 0) { throw "Runtime column $column has no width." }
    }
    if ([CodexOrganizerGuiProbe]::SendMessage(
            $windowList, 0x101D, [IntPtr]5, [IntPtr]::Zero).ToInt64() -ne 0) {
        throw 'Unexpected sixth runtime column.'
    }
    $threadColumnWidth = [CodexOrganizerGuiProbe]::SendMessage(
        $windowList, 0x101D, [IntPtr]1, [IntPtr]::Zero).ToInt64()
    if ($threadColumnWidth -lt 250) {
        throw "Thread-name column is not wide enough: $threadColumnWidth pixels."
    }
    $heading = [CodexOrganizerGuiProbe]::GetDlgItem($handle, 1000)
    if ($heading -eq [IntPtr]::Zero) { throw 'Organizer heading was not created.' }
    $headingText = New-Object System.Text.StringBuilder 256
    [CodexOrganizerGuiProbe]::GetWindowText($heading, $headingText, $headingText.Capacity) | Out-Null
    if (-not $headingText.ToString().StartsWith('Codex Window Organizer')) {
        throw "Unexpected Organizer heading: $headingText"
    }
    $guiSource = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'src\gui.cpp')
    foreach ($columnName in $expectedColumns) {
        if ($guiSource -notmatch ('L"' + [regex]::Escape($columnName) + '"')) {
            throw "Missing GUI source contract for '$columnName'."
        }
    }
    if ($guiSource -notmatch 'FetchCodexWeeklyUsage\(\)' -or $guiSource -match 'codex_weekly') {
        throw 'Weekly heading must use the authenticated account request, not a terminal footer field.'
    }
    if ($guiSource -notmatch '(?s)LRESULT\s+DrawListHeader\b.*?SetTextColor\(draw\.hdc,\s*kTextColor\);') {
        throw 'Custom list headers must use the readable white text token.'
    }
    if ($guiSource -notmatch '(?s)LRESULT\s+(?:CALLBACK\s+)?ListViewProcedure\b.*?DrawListHeader' -or
        $guiSource -notmatch 'SetWindowSubclass\(state\.list,\s*ListViewProcedure') {
        throw 'Header custom-draw notifications must be handled by the header parent list view.'
    }
    $listBackground = [CodexOrganizerGuiProbe]::SendMessage(
        $windowList, 0x1000, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
    $listText = [CodexOrganizerGuiProbe]::SendMessage(
        $windowList, 0x1023, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
    if ($listBackground -ne 0x221B16 -or $listText -ne 0xF3EDE6) {
        throw "Dark list palette mismatch: background=$listBackground text=$listText."
    }

    # Force SelectedMonitor's error path and close the resulting ShowError dialog.
    $monitorCombo = [CodexOrganizerGuiProbe]::GetDlgItem($handle, 1001)
    if ($monitorCombo -eq [IntPtr]::Zero) { throw 'Target monitor combo was not created.' }
    [CodexOrganizerGuiProbe]::SendMessage($monitorCombo, 0x014E, [IntPtr](-1), [IntPtr]::Zero) | Out-Null
    Invoke-DialogAction $handle 1201 0
    [CodexOrganizerGuiProbe]::SendMessage($monitorCombo, 0x014E, [IntPtr]0, [IntPtr]::Zero) | Out-Null

    # Exercise selection, preview, and the confirmation-cancel path. IDNO keeps
    # this test read-only: Arrange computes a plan but never calls MoveWindows.
    [CodexOrganizerGuiProbe]::PostMessage($handle, 0x0111, [IntPtr]1302, [IntPtr]::Zero) | Out-Null
    Invoke-DialogAction $handle 1201 0
    Invoke-DialogAction $handle 1202 7

    # Exercise the insufficient-selection warning and restore auto-selection.
    [CodexOrganizerGuiProbe]::PostMessage($handle, 0x0111, [IntPtr]1303, [IntPtr]::Zero) | Out-Null
    Invoke-DialogAction $handle 1102 0
    [CodexOrganizerGuiProbe]::PostMessage($handle, 0x0111, [IntPtr]1301, [IntPtr]::Zero) | Out-Null

    Write-Output "GUI_SMOKE=PASS SIZE=${width}x${height} PID=$($process.Id)"
    Write-Output 'GUI_COLUMNS=Uptime,Thread name (A-Z),Status,Model,Used tokens'
    Write-Output "GUI_HEADING=$headingText"
    Write-Output "GUI_THREAD_COLUMN_WIDTH=$threadColumnWidth"
    Write-Output 'GUI_HEADER_TEXT=white'
    Write-Output 'GUI_HEADER_DRAW_ROUTING=listview-subclass'
    Write-Output 'GUI_WEEKLY_SOURCE=authenticated-request'
    Write-Output 'GUI_THEME=dark'
    Write-Output 'GUI_INTERACTIONS=error,select-all,preview,cancel-arrange,warning,select-auto'
    Write-Output 'LIVE_WINDOW_MOVEMENT=NONE'
}
finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force
        }
    }
    Remove-Item -LiteralPath $temporaryConfig -Force -ErrorAction SilentlyContinue
}
