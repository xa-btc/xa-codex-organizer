# Codex Organizer native CLI smoke test v0.3.0
# Runs read-only list/version/dry-run commands through the GUI-subsystem binary,
# captures redirected output, and never moves a live Codex window.
# Last Updated: 2026-08-30 22:23:00

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = Join-Path $projectRoot 'build\Release\CodexOrganizer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Missing Release executable: $executable"
}

function Invoke-OrganizerCli([string[]]$Arguments, [string]$Name) {
    $stdout = Join-Path ([System.IO.Path]::GetTempPath()) "codex-organizer-$PID-$Name.out"
    $stderr = Join-Path ([System.IO.Path]::GetTempPath()) "codex-organizer-$PID-$Name.err"
    try {
        $process = Start-Process -FilePath $executable -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = if (Test-Path -LiteralPath $stdout) { Get-Content -Raw -LiteralPath $stdout } else { '' }
            Stderr = if (Test-Path -LiteralPath $stderr) { Get-Content -Raw -LiteralPath $stderr } else { '' }
        }
    }
    finally {
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

$version = Invoke-OrganizerCli @('--version') 'version'
if ($version.ExitCode -ne 0 -or $version.Stdout -notmatch 'Codex Window Organizer 0\.3\.0') {
    throw "Version command failed: exit=$($version.ExitCode) stdout=$($version.Stdout) stderr=$($version.Stderr)"
}

$help = Invoke-OrganizerCli @('--help') 'help'
if ($help.ExitCode -ne 0 -or
    $help.Stdout -notmatch 'Usage:' -or
    $help.Stdout -notmatch '--all-terminals') {
    throw "Help command failed: exit=$($help.ExitCode) stdout=$($help.Stdout) stderr=$($help.Stderr)"
}

$list = Invoke-OrganizerCli @('--list') 'list'
if ($list.ExitCode -ne 0) {
    throw "List command failed: exit=$($list.ExitCode) stderr=$($list.Stderr)"
}
if ($list.Stdout -notmatch 'Standalone Codex processes: \d+' -or
    $list.Stdout -notmatch 'Candidate terminal windows: \d+' -or
    $list.Stdout -notmatch 'Association: (status-line|title-match|manual)') {
    throw "List output is missing the native detection contract: $($list.Stdout)"
}

$dryRun = Invoke-OrganizerCli @('--all-terminals', '--dry-run') 'dry-run'
if ($dryRun.ExitCode -ne 0) {
    throw "Dry-run command failed: exit=$($dryRun.ExitCode) stderr=$($dryRun.Stderr)"
}
if ($dryRun.Stdout -notmatch 'Monitor: ' -or
    $dryRun.Stdout -notmatch 'Layout: \d+ windows, \d+ columns x \d+ rows') {
    throw "Dry-run output is missing the layout contract: $($dryRun.Stdout)"
}

$countDryRun = Invoke-OrganizerCli @('--all-terminals', '--count', '1', '--dry-run') 'count-dry-run'
if ($countDryRun.ExitCode -ne 0 -or
    $countDryRun.Stdout -notmatch 'Layout: 1 windows, 1 columns x 1 rows') {
    throw "Count dry-run failed: exit=$($countDryRun.ExitCode) stdout=$($countDryRun.Stdout) stderr=$($countDryRun.Stderr)"
}

Write-Output 'CLI_SMOKE=PASS'
Write-Output 'LIVE_WINDOW_MOVEMENT=NONE'
