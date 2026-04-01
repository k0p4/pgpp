#!/usr/bin/env python3
"""pgpp build & test script. Cross-platform, single entry point."""

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PRESETS_FILE = SCRIPT_DIR / "CMakePresets.json"


def load_presets():
    """Load non-hidden configure presets from CMakePresets.json."""
    if not PRESETS_FILE.exists():
        return {}
    with open(PRESETS_FILE) as f:
        data = json.load(f)
    presets = {}
    for p in data.get("configurePresets", []):
        if not p.get("hidden", False):
            presets[p["name"]] = p.get("displayName", p["name"])
    return presets


def print_presets(presets):
    if not presets:
        log("  (no presets found in CMakePresets.json)")
        return
    max_name = max(len(n) for n in presets)
    for name, display in presets.items():
        log(f"  {name:<{max_name}}  {display}")


def pause(message, auto_yes):
    """Print warning and wait for confirmation. Returns False if user wants to quit."""
    log(f"\n  {message}")
    if auto_yes or not sys.stdin.isatty():
        return True
    try:
        resp = input("  Press Enter to continue, or 'q' to quit: ")
        return resp.strip().lower() != "q"
    except (EOFError, KeyboardInterrupt):
        return False


def log(msg=""):
    print(msg, flush=True)


def run(cmd, label):
    """Run a command, streaming output. Returns exit code."""
    log(f"\n{'='*60}")
    log(f"  {label}")
    log(f"{'='*60}\n")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        log(f"\n  FAILED: {label} (exit code {result.returncode})")
    return result.returncode


def main():
    presets = load_presets()

    parser = argparse.ArgumentParser(
        description="pgpp build & test script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Available presets:\n" + "\n".join(
            f"  {n:<16}{d}" for n, d in presets.items()
        ) if presets else None,
    )
    parser.add_argument("--preset", default="dev-debug", help="CMake preset (default: dev-debug)")
    parser.add_argument("--skip-build", action="store_true", help="Skip configure and build, only run tests")
    parser.add_argument("--skip-tests", action="store_true", help="Skip tests, only configure and build")
    parser.add_argument("--unit-only", action="store_true", help="Run only unit tests (skip integration)")
    parser.add_argument("--yes", action="store_true", help="Skip all confirmation pauses")
    parser.add_argument("--clean", action="store_true", help="Remove build directory before configure")
    args = parser.parse_args()

    # Validate preset
    if presets and args.preset not in presets:
        log(f"Error: unknown preset '{args.preset}'\n")
        log("Available presets:")
        print_presets(presets)
        return 1

    # --- Prerequisite checks ---

    log("Checking prerequisites...\n")

    # cmake
    if not shutil.which("cmake"):
        log("  ERROR: cmake not found in PATH.")
        log("  Install CMake 3.16+ and ensure it's in your PATH.")
        return 1
    log("  cmake: OK")

    # VCPKG_ROOT / libpq
    vcpkg_root = os.environ.get("VCPKG_ROOT", "")
    has_vcpkg = bool(vcpkg_root and Path(vcpkg_root).is_dir())
    has_pg = bool(shutil.which("pg_config"))
    if has_vcpkg:
        log(f"  VCPKG_ROOT: {vcpkg_root}")
    elif has_pg:
        log("  VCPKG_ROOT: not set (but pg_config found — libpq available system-wide)")
    else:
        log("  ERROR: Neither VCPKG_ROOT nor system PostgreSQL (pg_config) found.")
        log("  pgpp requires libpq. Either:")
        log("    - Set VCPKG_ROOT to your vcpkg installation (libpq will be installed automatically)")
        log("    - Install PostgreSQL development libraries system-wide")
        if not pause("Continue anyway? (build will likely fail)", args.yes):
            return 1

    # Docker
    has_docker = bool(shutil.which("docker"))
    skip_integration = args.unit_only
    if not has_docker and not args.skip_tests and not skip_integration:
        log("  docker: NOT FOUND")
        log("  Warning: integration tests require Docker and will be skipped.")
        if not pause("Continue without integration tests?", args.yes):
            return 1
        skip_integration = True
    elif has_docker:
        log("  docker: OK")

    log()

    # --- Determine build mode ---
    # Presets require vcpkg (toolchainFile references VCPKG_ROOT).
    # Without vcpkg, fall back to raw cmake invocation.
    use_presets = has_vcpkg
    if use_presets:
        build_dir = SCRIPT_DIR / "build" / args.preset
        build_type = "Release" if "release" in args.preset else "Debug"
    else:
        build_dir = SCRIPT_DIR / "build" / "dev-debug"
        build_type = "Debug"
        log("  (no vcpkg — using raw cmake without presets)\n")

    # --- Clean ---

    if args.clean and not args.skip_build:
        if build_dir.exists():
            log(f"Cleaning {build_dir}...")
            shutil.rmtree(build_dir)

    # --- Configure & Build ---

    if not args.skip_build:
        if use_presets:
            rc = run(["cmake", "--preset", args.preset], f"Configure ({args.preset})")
        else:
            rc = run([
                "cmake", "-S", str(SCRIPT_DIR), "-B", str(build_dir),
                f"-DCMAKE_BUILD_TYPE={build_type}",
                "-DPGPP_BUILD_TESTS=ON",
            ], f"Configure ({build_type})")
        if rc != 0:
            return rc

        if use_presets:
            rc = run(["cmake", "--build", "--preset", args.preset], f"Build ({args.preset})")
        else:
            rc = run([
                "cmake", "--build", str(build_dir), "--config", build_type,
            ], f"Build ({build_type})")
        if rc != 0:
            return rc

    # --- Tests ---

    if not args.skip_tests:
        if use_presets:
            rc = run(
                ["ctest", "--preset", args.preset, "-R", "pgpp_unit_tests"],
                "Unit tests",
            )
        else:
            rc = run(
                ["ctest", "--test-dir", str(build_dir), "-C", build_type,
                 "--output-on-failure", "-R", "pgpp_unit_tests"],
                "Unit tests",
            )
        if rc != 0:
            return rc

        if not skip_integration:
            if use_presets:
                rc = run(
                    ["ctest", "--preset", args.preset, "-R", "pgpp_integration_tests"],
                    "Integration tests",
                )
            else:
                rc = run(
                    ["ctest", "--test-dir", str(build_dir), "-C", build_type,
                     "--output-on-failure", "-R", "pgpp_integration_tests"],
                    "Integration tests",
                )
            if rc != 0:
                return rc

    # --- Summary ---

    log(f"\n{'='*60}")
    log("  All steps completed successfully.")
    log(f"{'='*60}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
