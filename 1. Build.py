############################################################################################################################
#
#   ██████╗ ██████╗ ██████╗ ███████╗██╗  ██╗     ██████╗ ██████╗  ██████╗  █████╗ ███╗   ██╗██╗███████╗███████╗██████╗
#  ██╔════╝██╔═══██╗██╔══██╗██╔════╝╚██╗██╔╝    ██╔═══██╗██╔══██╗██╔════╝ ██╔══██╗████╗  ██║██║╚══███╔╝██╔════╝██╔══██╗
#  ██║     ██║   ██║██║  ██║█████╗   ╚███╔╝     ██║   ██║██████╔╝██║  ███╗███████║██╔██╗ ██║██║  ███╔╝ █████╗  ██████╔╝
#  ██║     ██║   ██║██║  ██║██╔══╝   ██╔██╗     ██║   ██║██╔══██╗██║   ██║██╔══██║██║╚██╗██║██║ ███╔╝  ██╔══╝  ██╔══██╗
#  ╚██████╗╚██████╔╝██████╔╝███████╗██╔╝ ██╗    ╚██████╔╝██║  ██║╚██████╔╝██║  ██║██║ ╚████║██║███████╗███████╗██║  ██║
#   ╚═════╝ ╚═════╝ ╚═════╝ ╚══════╝╚═╝  ╚═╝     ╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝╚═╝╚══════╝╚══════╝╚═╝  ╚═╝
#
#  ██████╗ ██╗   ██╗██╗██╗     ██████╗     ██╗   ██╗ ██╗    ██████╗  ██████╗
#  ██╔══██╗██║   ██║██║██║     ██╔══██╗    ██║   ██║███║   ██╔═████╗██╔═████╗
#  ██████╔╝██║   ██║██║██║     ██║  ██║    ██║   ██║╚██║   ██║██╔██║██║██╔██║
#  ██╔══██╗██║   ██║██║██║     ██║  ██║    ╚██╗ ██╔╝ ██║   ████╔╝██║████╔╝██║
#  ██████╔╝╚██████╔╝██║███████╗██████╔╝     ╚████╔╝  ██║██╗╚██████╔╝╚██████╔╝
#  ╚═════╝  ╚═════╝ ╚═╝╚══════╝╚═════╝       ╚═══╝   ╚═╝╚═╝ ╚═════╝  ╚═════╝
#
############################################################################################################################
#
# Build, test, stage, verify, and optionally launch the native Codex Organizer.
#
# This is the canonical one-step Windows build entry point. It wraps the existing CMake/MSVC project, runs its native and
# smoke regressions, installs into an isolated staging directory, verifies the exact release inventory and PE identity,
# then swaps the verified release into place while preserving the previous release as a timestamped backup.
#
# Core Features:
# • Portable project-root discovery with CMake 3.24+ and x64 Release validation
# • Bounded parallel compilation and exact child-process failure propagation
# • Native CTest plus optional CLI, resource, synthetic-window, and GUI smoke tests
# • Verified four-file release staging with SHA-256 and PE x64 GUI checks
# • Exact-path graceful shutdown of only a running released CodexOrganizer.exe
# • Recoverable release swap with rollback if publication fails
# • Optional relaunch, external build_config.json overrides, and noninteractive mode
# • Timestamped crash logs with full tracebacks
#
# Important Note: Automated tests never move live Codex windows; the synthetic layout test owns every window it creates.
#
# Codex Organizer Build v1.00
# Safe native build, regression, staging, and release orchestration.
# Created by: XA
# Last Updated: 2026-08-30 23:55:00
#
# ## Release Notes ##
#
# v1.00 - Initial canonical one-step native build wrapper
#
############################################################################################################################

"""Canonical one-step build entry point for Codex Organizer."""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import traceback
from typing import Any, Sequence


SCRIPT_VERSION = "1.00"
PROJECT_DIR = Path(__file__).resolve().parent
BUILD_DIR = PROJECT_DIR / "build"
BUILT_EXE = BUILD_DIR / "Release" / "CodexOrganizer.exe"
STAGE_ROOT = BUILD_DIR / "release-stage"
STAGE_DIR = STAGE_ROOT / "Codex Organizer"
RELEASE_DIR = PROJECT_DIR / "release" / "Codex Organizer"
RELEASE_EXE = RELEASE_DIR / "CodexOrganizer.exe"
BACKUP_ROOT = PROJECT_DIR / "backups"
BUILD_CONFIG_FILE = PROJECT_DIR / "build_config.json"

DEFAULT_CONFIG: dict[str, Any] = {
    "jobs": 4,
    "run_tests": True,
    "full_tests": True,
    "launch_after_build": True,
    "pause_on_exit": True,
}

EXPECTED_RELEASE_FILES = {
    "CodexOrganizer.exe",
    "config.json",
    "resources/codex-organizer-icon.png",
    "resources/codex-organizer.ico",
}

FULL_TEST_SCRIPTS = (
    "tests/resource_contract.ps1",
    "tests/cli_smoke.ps1",
    "tests/eight_window_dry_run.ps1",
    "tests/gui_smoke.ps1",
)

POWERSHELL_CLOSE_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$target = [IO.Path]::GetFullPath($env:CODEX_ORGANIZER_BUILD_TARGET)
$force = $env:CODEX_ORGANIZER_BUILD_FORCE -eq '1'
$matches = @(Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $target
})
$requested = @()
foreach ($item in $matches) {
    $process = Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue
    if ($process -and $process.CloseMainWindow()) {
        $requested += $item.ProcessId
    }
}
foreach ($pidValue in $requested) {
    try { Wait-Process -Id $pidValue -Timeout 8 -ErrorAction Stop } catch { }
}
$remaining = @(Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $target
})
$forced = @()
if ($remaining.Count -gt 0 -and $force) {
    foreach ($item in $remaining) {
        Stop-Process -Id $item.ProcessId -Force -ErrorAction Stop
        $forced += $item.ProcessId
    }
    Start-Sleep -Milliseconds 250
}
$stillRunning = @(Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $target
})
Write-Output ('CLOSE_REQUESTED_PIDS=' + ($requested -join ','))
Write-Output ('FORCED_PIDS=' + ($forced -join ','))
if ($stillRunning.Count -gt 0) {
    Write-Error ('Codex Organizer is still running from the release path. PIDs: ' + (($stillRunning.ProcessId) -join ','))
    exit 42
}
"""


class BuildFailure(RuntimeError):
    """Expected build failure with a concise operator-facing message."""


def banner(title: str) -> None:
    print("\n" + "=" * 80, flush=True)
    print(title, flush=True)
    print("=" * 80, flush=True)


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build and stage the native Codex Organizer Windows application.")
    parser.add_argument("--jobs", type=int, help="Bounded parallel build jobs (1-32; default from build_config.json).")
    parser.add_argument("--fresh", action="store_true", help="Ask CMake to regenerate its configure cache.")
    parser.add_argument("--clean", action="store_true", help="Clean the selected CMake configuration before building.")
    parser.add_argument("--skip-tests", action="store_true", help="Skip CTest and smoke tests.")
    parser.add_argument("--full-tests", action="store_true", help="Run all project-owned smoke tests after CTest.")
    parser.add_argument("--configure-only", action="store_true", help="Configure CMake, then stop before compilation.")
    parser.add_argument("--check", action="store_true", help="Run tool and project preflight checks only.")
    parser.add_argument("--force-close", action="store_true", help="Force-stop only the exact released Organizer if normal close fails.")
    launch = parser.add_mutually_exclusive_group()
    launch.add_argument("--run", action="store_true", help="Launch the verified release after a successful build.")
    launch.add_argument("--no-run", action="store_true", help="Do not launch the release after building.")
    parser.add_argument("--no-pause", action="store_true", help="Return immediately instead of waiting for Enter.")
    parser.add_argument("--version", action="version", version=f"Codex Organizer Build {SCRIPT_VERSION}")
    return parser.parse_args(argv)


def load_build_config(path: Path = BUILD_CONFIG_FILE) -> dict[str, Any]:
    config = dict(DEFAULT_CONFIG)
    if not path.is_file():
        return config
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise BuildFailure(f"Invalid JSON in {path.name}: {exc}") from exc
    if not isinstance(loaded, dict):
        raise BuildFailure(f"{path.name} must contain one JSON object.")
    unknown = sorted(set(loaded) - set(DEFAULT_CONFIG))
    if unknown:
        raise BuildFailure(f"Unknown {path.name} setting(s): {', '.join(unknown)}")
    config.update(loaded)
    if isinstance(config["jobs"], bool) or not isinstance(config["jobs"], int) or not 1 <= config["jobs"] <= 32:
        raise BuildFailure("build_config.json jobs must be an integer from 1 through 32.")
    for name in ("run_tests", "full_tests", "launch_after_build", "pause_on_exit"):
        if not isinstance(config[name], bool):
            raise BuildFailure(f"build_config.json {name} must be true or false.")
    print(f"[CONFIG] Loaded configuration from {path}", flush=True)
    return config


def parse_cmake_version(output: str) -> tuple[int, int, int]:
    match = re.search(r"cmake version\s+(\d+)\.(\d+)\.(\d+)", output, re.IGNORECASE)
    if not match:
        raise BuildFailure("Could not parse the installed CMake version.")
    version = tuple(int(part) for part in match.groups())
    if version < (3, 24, 0):
        raise BuildFailure(f"CMake 3.24 or newer is required; found {'.'.join(map(str, version))}.")
    return version


def resolve_cmake() -> tuple[str, tuple[int, int, int]]:
    executable = shutil.which("cmake")
    if not executable:
        raise BuildFailure("CMake was not found on PATH. Install CMake 3.24 or newer.")
    result = subprocess.run(
        [executable, "--version"],
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if result.returncode != 0:
        raise BuildFailure(f"CMake version check failed with exit code {result.returncode}.")
    return executable, parse_cmake_version(result.stdout)


def project_version(project_dir: Path = PROJECT_DIR) -> str:
    text = (project_dir / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(CodexOrganizer\s+VERSION\s+(\d+\.\d+\.\d+)", text, re.IGNORECASE)
    if not match:
        raise BuildFailure("CMakeLists.txt does not declare the CodexOrganizer semantic version.")
    return match.group(1)


def validate_project_layout(project_dir: Path = PROJECT_DIR) -> str:
    required = (
        "CMakeLists.txt",
        "config.json",
        "src/main.cpp",
        "src/gui.cpp",
        "src/core.cpp",
        "src/windows.cpp",
        "tests/native_tests.cpp",
        "resources/codex-organizer.ico",
        "resources/codex-organizer-icon.png",
    )
    missing = [relative for relative in required if not (project_dir / relative).is_file()]
    if missing:
        raise BuildFailure("Project layout is incomplete; missing: " + ", ".join(missing))
    return project_version(project_dir)


def run_command(command: Sequence[str | Path], label: str, cwd: Path = PROJECT_DIR) -> None:
    normalized = [str(part) for part in command]
    print(f"> {subprocess.list2cmdline(normalized)}", flush=True)
    result = subprocess.run(normalized, cwd=cwd, check=False)
    if result.returncode != 0:
        raise BuildFailure(f"{label} failed with exit code {result.returncode}.")


def assert_within(path: Path, parent: Path) -> Path:
    resolved = path.resolve(strict=False)
    resolved_parent = parent.resolve(strict=False)
    try:
        common = Path(os.path.commonpath((str(resolved), str(resolved_parent))))
    except ValueError as exc:
        raise BuildFailure(f"Path is outside the expected root: {resolved}") from exc
    if os.path.normcase(str(common)) != os.path.normcase(str(resolved_parent)):
        raise BuildFailure(f"Path is outside the expected root: {resolved}")
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def verify_pe_x64_gui(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise BuildFailure(f"Not a valid Windows PE executable: {path}")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset + 96 > len(data) or data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise BuildFailure(f"Invalid PE header: {path}")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    optional_magic = struct.unpack_from("<H", data, pe_offset + 24)[0]
    subsystem = struct.unpack_from("<H", data, pe_offset + 24 + 68)[0]
    if machine != 0x8664 or optional_magic != 0x20B or subsystem != 2:
        raise BuildFailure(
            f"Expected an x64 PE32+ GUI executable; found machine=0x{machine:04X}, "
            f"magic=0x{optional_magic:04X}, subsystem={subsystem}."
        )


def prepare_stage(stage_root: Path = STAGE_ROOT) -> None:
    resolved = assert_within(stage_root, BUILD_DIR)
    if resolved.exists():
        shutil.rmtree(resolved)
    resolved.mkdir(parents=True, exist_ok=False)


def verify_staged_release(
    stage_dir: Path,
    built_exe: Path,
    project_dir: Path,
    version: str,
) -> str:
    files = {
        path.relative_to(stage_dir).as_posix()
        for path in stage_dir.rglob("*")
        if path.is_file()
    }
    if files != EXPECTED_RELEASE_FILES:
        missing = sorted(EXPECTED_RELEASE_FILES - files)
        extra = sorted(files - EXPECTED_RELEASE_FILES)
        raise BuildFailure(f"Release inventory mismatch. Missing={missing or 'none'} Extra={extra or 'none'}")
    staged_exe = stage_dir / "CodexOrganizer.exe"
    if sha256_file(staged_exe) != sha256_file(built_exe):
        raise BuildFailure("The staged executable does not match the freshly built executable.")
    for relative in ("config.json", "resources/codex-organizer.ico", "resources/codex-organizer-icon.png"):
        if sha256_file(stage_dir / relative) != sha256_file(project_dir / relative):
            raise BuildFailure(f"The staged {relative} does not match the canonical project file.")
    verify_pe_x64_gui(staged_exe)
    version_result = subprocess.run(
        [str(staged_exe), "--version"],
        cwd=stage_dir,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )
    expected_version = f"Codex Window Organizer {version}"
    if version_result.returncode != 0 or expected_version not in version_result.stdout:
        raise BuildFailure(
            f"Staged version check failed: exit={version_result.returncode}, expected {expected_version!r}."
        )
    return sha256_file(staged_exe)


def shutdown_exact_release(executable: Path, force: bool) -> None:
    if not executable.is_file():
        return
    environment = os.environ.copy()
    environment["CODEX_ORGANIZER_BUILD_TARGET"] = str(executable.resolve())
    environment["CODEX_ORGANIZER_BUILD_FORCE"] = "1" if force else "0"
    result = subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            POWERSHELL_CLOSE_SCRIPT,
        ],
        cwd=PROJECT_DIR,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=30,
    )
    if result.stdout.strip():
        print(result.stdout.strip(), flush=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit code {result.returncode}"
        raise BuildFailure(
            "Could not close the exact released Codex Organizer. Close it normally or rerun with --force-close. "
            + detail
        )


def unique_release_backup(backup_root: Path = BACKUP_ROOT, stamp: str | None = None) -> Path:
    backup_root.mkdir(parents=True, exist_ok=True)
    timestamp = stamp or datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    candidate = backup_root / f"release - before build - {timestamp}"
    counter = 2
    while candidate.exists():
        candidate = backup_root / f"release - before build - {timestamp} - {counter}"
        counter += 1
    return candidate


def publish_staged_release(
    stage_dir: Path,
    release_dir: Path,
    force_close: bool,
    project_dir: Path = PROJECT_DIR,
) -> Path | None:
    assert_within(stage_dir, project_dir / "build")
    assert_within(release_dir, project_dir / "release")
    shutdown_exact_release(release_dir / "CodexOrganizer.exe", force_close)
    release_dir.parent.mkdir(parents=True, exist_ok=True)
    backup: Path | None = None
    if release_dir.exists():
        backup = unique_release_backup(project_dir / "backups")
        release_dir.rename(backup)
    try:
        stage_dir.rename(release_dir)
    except Exception:
        if backup is not None and backup.exists() and not release_dir.exists():
            backup.rename(release_dir)
        raise
    return backup


def launch_release(executable: Path) -> int:
    process = subprocess.Popen([str(executable)], cwd=executable.parent, close_fds=True)
    time.sleep(1.5)
    return_code = process.poll()
    if return_code is not None:
        raise BuildFailure(f"The rebuilt Organizer exited during launch with code {return_code}.")
    return process.pid


def perform_build(arguments: argparse.Namespace, config: dict[str, Any]) -> int:
    banner("Codex Organizer native Windows build")
    version = validate_project_layout()
    cmake, cmake_version = resolve_cmake()
    jobs = arguments.jobs if arguments.jobs is not None else config["jobs"]
    if not 1 <= jobs <= 32:
        raise BuildFailure("--jobs must be from 1 through 32.")
    run_tests = config["run_tests"] and not arguments.skip_tests
    full_tests = (config["full_tests"] or arguments.full_tests) and run_tests
    launch_after_build = config["launch_after_build"]
    if arguments.run:
        launch_after_build = True
    if arguments.no_run:
        launch_after_build = False
    print(f"Project: {PROJECT_DIR}", flush=True)
    print(f"Version: {version}", flush=True)
    print(f"CMake:   {'.'.join(map(str, cmake_version))} ({cmake})", flush=True)
    print(f"Jobs:    {jobs}", flush=True)
    if arguments.check:
        banner("PREFLIGHT SUCCESSFUL")
        return 0

    configure = [cmake]
    if arguments.fresh:
        configure.append("--fresh")
    configure.extend(["-S", str(PROJECT_DIR), "-B", str(BUILD_DIR), "-G", "Visual Studio 17 2022", "-A", "x64"])
    banner("Configuring Visual Studio 2022 x64")
    run_command(configure, "CMake configure")
    if arguments.configure_only:
        banner("CONFIGURE SUCCESSFUL")
        return 0

    banner("Compiling Release")
    build_command: list[str | Path] = [
        cmake,
        "--build",
        BUILD_DIR,
        "--config",
        "Release",
        "--parallel",
        str(jobs),
    ]
    if arguments.clean:
        build_command.append("--clean-first")
    run_command(build_command, "Release build")
    if not BUILT_EXE.is_file():
        raise BuildFailure(f"Build returned success but did not produce {BUILT_EXE}.")

    if run_tests:
        banner("Running native regressions")
        run_command(
            [cmake, "--build", BUILD_DIR, "--target", "RUN_TESTS", "--config", "Release", "--parallel", str(jobs)],
            "CTest",
        )
        if full_tests:
            banner("Running project smoke tests")
            for relative in FULL_TEST_SCRIPTS:
                run_command(
                    ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", PROJECT_DIR / relative],
                    relative,
                )

    banner("Staging and verifying release")
    prepare_stage()
    run_command(
        [cmake, "--install", BUILD_DIR, "--config", "Release", "--prefix", STAGE_DIR],
        "Release install",
    )
    staged_hash = verify_staged_release(STAGE_DIR, BUILT_EXE, PROJECT_DIR, version)
    backup = publish_staged_release(STAGE_DIR, RELEASE_DIR, arguments.force_close)
    installed_hash = sha256_file(RELEASE_EXE)
    if installed_hash != staged_hash:
        raise BuildFailure("Installed executable hash does not match the verified staged executable.")

    launched_pid: int | None = None
    if launch_after_build:
        banner("Launching verified release")
        launched_pid = launch_release(RELEASE_EXE)

    banner("BUILD SUCCESSFUL")
    print(f"Application: {RELEASE_EXE}", flush=True)
    print(f"SHA-256:     {installed_hash}", flush=True)
    print(f"Backup:      {backup if backup is not None else 'No previous release'}", flush=True)
    print(f"Launched:    {f'PID {launched_pid}' if launched_pid is not None else 'No (--no-run)'}", flush=True)
    return 0


def write_crash_log(exception: BaseException, directory: Path = PROJECT_DIR) -> Path:
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    path = directory / f"crash_log_{timestamp}.log"
    detail = (
        f"Crash Log - {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
        f"Exception Type: {type(exception).__name__}\n"
        f"Exception Message: {exception}\n\n"
        f"Full Traceback:\n{traceback.format_exc()}"
    )
    path.write_text(detail, encoding="utf-8")
    return path


def pause_if_requested(no_pause: bool) -> None:
    if no_pause:
        return
    try:
        input("\nPress Enter to close...")
    except EOFError:
        pass


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    config = load_build_config()
    no_pause = arguments.no_pause or not config["pause_on_exit"]
    try:
        return perform_build(arguments, config)
    except Exception as exc:
        banner("BUILD FAILED")
        print(f"{type(exc).__name__}: {exc}", flush=True)
        print(traceback.format_exc(), flush=True)
        try:
            crash_log = write_crash_log(exc)
            print(f"Crash log: {crash_log}", flush=True)
        except Exception as log_error:
            print(f"Could not write the crash log: {log_error}", flush=True)
        return 1
    finally:
        pause_if_requested(no_pause)


if __name__ == "__main__":
    raise SystemExit(main())
