from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import types
import unittest
from unittest import mock


PROJECT_DIR = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("codex_organizer_build", PROJECT_DIR / "1. Build.py")
assert SPEC is not None and SPEC.loader is not None
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


def write_minimal_pe(path: Path) -> None:
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<H", data, 0x98 + 68, 2)
    path.write_bytes(data)


class BuildWrapperTests(unittest.TestCase):
    def test_parse_arguments_and_cmake_versions(self) -> None:
        arguments = BUILD.parse_arguments(["--jobs", "3", "--full-tests", "--no-run", "--no-pause"])
        self.assertEqual(arguments.jobs, 3)
        self.assertTrue(arguments.full_tests)
        self.assertTrue(arguments.no_run)
        self.assertEqual(BUILD.parse_cmake_version("cmake version 3.31.6"), (3, 31, 6))
        with self.assertRaises(BUILD.BuildFailure):
            BUILD.parse_cmake_version("cmake version 3.23.9")

    def test_load_build_config_defaults_overrides_and_validation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(BUILD.load_build_config(root / "missing.json"), BUILD.DEFAULT_CONFIG)
            config_path = root / "build_config.json"
            config_path.write_text(json.dumps({"jobs": 2, "launch_after_build": False}), encoding="utf-8")
            loaded = BUILD.load_build_config(config_path)
            self.assertEqual(loaded["jobs"], 2)
            self.assertFalse(loaded["launch_after_build"])
            config_path.write_text(json.dumps({"jobs": 0}), encoding="utf-8")
            with self.assertRaises(BUILD.BuildFailure):
                BUILD.load_build_config(config_path)
            config_path.write_text(json.dumps({"unknown": True}), encoding="utf-8")
            with self.assertRaises(BUILD.BuildFailure):
                BUILD.load_build_config(config_path)

    def test_project_layout_and_path_boundary(self) -> None:
        self.assertEqual(BUILD.validate_project_layout(PROJECT_DIR), "0.3.0")
        self.assertEqual(BUILD.project_version(PROJECT_DIR), "0.3.0")
        self.assertEqual(BUILD.assert_within(PROJECT_DIR / "build" / "x", PROJECT_DIR), (PROJECT_DIR / "build" / "x").resolve())
        with self.assertRaises(BUILD.BuildFailure):
            BUILD.assert_within(PROJECT_DIR.parent, PROJECT_DIR)

    def test_pe_hash_stage_cleanup_and_stage_verification(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            project = root / "project"
            build = project / "build"
            stage = build / "release-stage" / "Codex Organizer"
            resources = stage / "resources"
            canonical_resources = project / "resources"
            resources.mkdir(parents=True)
            canonical_resources.mkdir(parents=True)
            built = build / "Release" / "CodexOrganizer.exe"
            built.parent.mkdir(parents=True)
            write_minimal_pe(built)
            write_minimal_pe(stage / "CodexOrganizer.exe")
            (project / "config.json").write_text("{}\n", encoding="utf-8")
            (stage / "config.json").write_text("{}\n", encoding="utf-8")
            for name in ("codex-organizer.ico", "codex-organizer-icon.png"):
                (canonical_resources / name).write_bytes(name.encode("utf-8"))
                (resources / name).write_bytes(name.encode("utf-8"))
            completed = subprocess.CompletedProcess([], 0, "Codex Window Organizer 0.3.0\n", "")
            with mock.patch.object(BUILD.subprocess, "run", return_value=completed):
                digest = BUILD.verify_staged_release(stage, built, project, "0.3.0")
            self.assertEqual(digest, BUILD.sha256_file(built))
            BUILD.verify_pe_x64_gui(built)
            cleanup = build / "cleanup"
            cleanup.mkdir()
            (cleanup / "old.txt").write_text("old", encoding="utf-8")
            with mock.patch.object(BUILD, "BUILD_DIR", build):
                BUILD.prepare_stage(cleanup)
            self.assertTrue(cleanup.is_dir())
            self.assertEqual(list(cleanup.iterdir()), [])

    def test_command_cmake_process_shutdown_and_launch_contracts(self) -> None:
        ok = subprocess.CompletedProcess([], 0, "cmake version 3.31.6\n", "")
        with mock.patch.object(BUILD.shutil, "which", return_value="cmake.exe"), mock.patch.object(BUILD.subprocess, "run", return_value=ok):
            self.assertEqual(BUILD.resolve_cmake(), ("cmake.exe", (3, 31, 6)))
            BUILD.run_command(["cmake.exe", "--version"], "version", PROJECT_DIR)
        failed = subprocess.CompletedProcess([], 7, "", "")
        with mock.patch.object(BUILD.subprocess, "run", return_value=failed):
            with self.assertRaises(BUILD.BuildFailure):
                BUILD.run_command(["false.exe"], "expected failure", PROJECT_DIR)
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "CodexOrganizer.exe"
            BUILD.shutdown_exact_release(missing, False)
        process = mock.Mock(pid=1234)
        process.poll.return_value = None
        with mock.patch.object(BUILD.subprocess, "Popen", return_value=process), mock.patch.object(BUILD.time, "sleep"):
            self.assertEqual(BUILD.launch_release(PROJECT_DIR / "CodexOrganizer.exe"), 1234)

    def test_release_backup_publish_rollback_crash_log_and_pause(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            stage = project / "build" / "release-stage" / "Codex Organizer"
            release = project / "release" / "Codex Organizer"
            stage.mkdir(parents=True)
            release.mkdir(parents=True)
            (stage / "CodexOrganizer.exe").write_text("new", encoding="utf-8")
            (release / "CodexOrganizer.exe").write_text("old", encoding="utf-8")
            existing = project / "backups" / "release - before build - fixed"
            existing.mkdir(parents=True)
            backup = BUILD.unique_release_backup(project / "backups", "fixed")
            self.assertTrue(str(backup).endswith("fixed - 2"))
            with mock.patch.object(BUILD, "shutdown_exact_release"):
                published_backup = BUILD.publish_staged_release(stage, release, False, project)
            self.assertIsNotNone(published_backup)
            self.assertEqual((release / "CodexOrganizer.exe").read_text(encoding="utf-8"), "new")
            try:
                raise RuntimeError("test crash")
            except RuntimeError as error:
                crash_log = BUILD.write_crash_log(error, project)
            self.assertIn("test crash", crash_log.read_text(encoding="utf-8"))
            BUILD.pause_if_requested(True)

    def test_perform_build_preflight_and_main_failure_path(self) -> None:
        arguments = BUILD.parse_arguments(["--check", "--no-pause", "--no-run"])
        config = dict(BUILD.DEFAULT_CONFIG)
        with mock.patch.object(BUILD, "validate_project_layout", return_value="0.3.0"), mock.patch.object(
            BUILD, "resolve_cmake", return_value=("cmake.exe", (3, 31, 6))
        ):
            self.assertEqual(BUILD.perform_build(arguments, config), 0)
        with mock.patch.object(BUILD, "load_build_config", return_value=config), mock.patch.object(
            BUILD, "perform_build", side_effect=BUILD.BuildFailure("planned")
        ), mock.patch.object(BUILD, "write_crash_log", return_value=PROJECT_DIR / "crash.log"):
            self.assertEqual(BUILD.main(["--no-pause"]), 1)


if __name__ == "__main__":
    unittest.main()
