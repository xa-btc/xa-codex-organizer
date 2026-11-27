# Codex Organizer synthetic eight-window CLI dry-run regression v0.3.0
# Creates only test-owned top-level windows with a unique class, verifies the
# 2x4 eight-window plan, and destroys every synthetic HWND without moving it.

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class CodexOrganizerSyntheticWindows {
    private const uint WS_OVERLAPPEDWINDOW = 0x00CF0000;
    private const uint WS_VISIBLE = 0x10000000;
    private const uint WS_EX_TOOLWINDOW = 0x00000080;
    private const uint WS_EX_NOACTIVATE = 0x08000000;

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr WindowProcedure(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass {
        public uint size;
        public uint style;
        public IntPtr procedure;
        public int classExtra;
        public int windowExtra;
        public IntPtr instance;
        public IntPtr icon;
        public IntPtr cursor;
        public IntPtr background;
        public string menuName;
        public string className;
        public IntPtr smallIcon;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ushort RegisterClassEx(ref WindowClass windowClass);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowEx(uint extendedStyle, string className, string title,
        uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu,
        IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll")] private static extern IntPtr DefWindowProc(
        IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool UnregisterClass(string className, IntPtr instance);
    [DllImport("kernel32.dll")] private static extern IntPtr GetModuleHandle(string moduleName);

    private static readonly WindowProcedure Procedure = DefWindowProc;
    private static readonly List<IntPtr> Windows = new List<IntPtr>();
    private static string registeredClass;
    private static IntPtr module;

    public static void Create(string className, int count) {
        module = GetModuleHandle(null);
        var definition = new WindowClass {
            size = (uint)Marshal.SizeOf(typeof(WindowClass)),
            procedure = Marshal.GetFunctionPointerForDelegate(Procedure),
            instance = module,
            className = className,
        };
        if (RegisterClassEx(ref definition) == 0) {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "RegisterClassEx failed");
        }
        registeredClass = className;
        for (int index = 0; index < count; ++index) {
            IntPtr window = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                className, "Codex Organizer synthetic terminal " + (index + 1),
                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40 + index * 8, 40 + index * 8,
                220, 100, IntPtr.Zero, IntPtr.Zero, module, IntPtr.Zero);
            if (window == IntPtr.Zero) {
                Destroy();
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateWindowEx failed");
            }
            Windows.Add(window);
        }
    }

    public static void Destroy() {
        foreach (IntPtr window in Windows) DestroyWindow(window);
        Windows.Clear();
        if (!String.IsNullOrEmpty(registeredClass)) {
            UnregisterClass(registeredClass, module);
            registeredClass = null;
        }
    }
}
'@

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = Join-Path $projectRoot 'build\Release\CodexOrganizer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Missing Release executable: $executable"
}

$className = "CodexOrganizerSyntheticTerminal_$PID"
$temporaryConfig = Join-Path ([System.IO.Path]::GetTempPath()) "codex-organizer-eight-window-$PID.json"
$stdout = Join-Path ([System.IO.Path]::GetTempPath()) "codex-organizer-eight-window-$PID.out"
$stderr = Join-Path ([System.IO.Path]::GetTempPath()) "codex-organizer-eight-window-$PID.err"
try {
    [CodexOrganizerSyntheticWindows]::Create($className, 8)
    $config = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'config.json') | ConvertFrom-Json
    $config.terminal_class_names = @($className)
    $config.organize_by_thread_name = $false
    $config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporaryConfig -Encoding UTF8

    $arguments = @('--config', $temporaryConfig, '--all-terminals', '--count', '8',
                   '--dry-run', '--no-thread-order')
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $output = if (Test-Path -LiteralPath $stdout) { Get-Content -Raw -LiteralPath $stdout } else { '' }
    $errorOutput = if (Test-Path -LiteralPath $stderr) { Get-Content -Raw -LiteralPath $stderr } else { '' }
    if ($process.ExitCode -ne 0) {
        throw "Eight-window dry run failed: exit=$($process.ExitCode) stdout=$output stderr=$errorOutput"
    }
    if ($output -notmatch 'Layout: 8 windows, 2 columns x 4 rows') {
        throw "Eight-window dry run did not produce the expected 2x4 plan: $output"
    }

    Write-Output 'EIGHT_WINDOW_DRY_RUN=PASS'
    Write-Output 'SYNTHETIC_WINDOWS=8'
    Write-Output 'LIVE_WINDOW_MOVEMENT=NONE'
}
finally {
    [CodexOrganizerSyntheticWindows]::Destroy()
    Remove-Item -LiteralPath $temporaryConfig, $stdout, $stderr -Force -ErrorAction SilentlyContinue
}
