#!/usr/bin/env python3
"""Run all qt-commander tests — Python + C++ (g++ or MSVC).

Usage:
    python run_all_tests.py              # all tests
    python run_all_tests.py --skip-cpp   # Python only
    python run_all_tests.py --skip-e2e   # skip E2E (needs Qt)
    python run_all_tests.py --quick      # Python + MSVC ctest only (fast)
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent
BUILD = ROOT / "build"
MSVC = BUILD / "msvc"

# ── helpers ──

passed, failed, skipped = 0, 0, 0

def run(name: str, cmd: list[str], cwd: Path = ROOT, env: dict | None = None,
        ok_exit: int = 0, skip_reason: str = "") -> bool:
    global passed, failed, skipped
    if skip_reason:
        print(f"  SKIP ({skip_reason}): {name}")
        skipped += 1
        return False

    print(f"  {name} ...", end=" ", flush=True)
    try:
        e = os.environ.copy()
        if env:
            e.update(env)
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=600, env=e)
    except subprocess.TimeoutExpired:
        print("TIMEOUT")
        failed += 1
        return False

    if r.returncode == ok_exit:
        print("PASS")
        passed += 1
        return True
    else:
        print("FAIL")
        if r.stderr.strip():
            print(f"    stderr: {r.stderr.strip()[-200:]}")
        if r.stdout.strip():
            print(f"    stdout: {r.stdout.strip()[-200:]}")
        failed += 1
        return False


def gpp(src: str, out: str, extra_libs: str = "") -> bool:
    """Compile C++ source with g++."""
    cmd = (
        f"g++ -std=c++17 -static -o {out} {src} "
        f"-I src/injector -I src/common -I src/library "
        f"-lws2_32 -lpsapi -lbcrypt {extra_libs}"
    )
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        print(f"    BUILD FAIL: {out}")
        if r.stderr.strip():
            print(f"    {r.stderr.strip()[-200:]}")
    return r.returncode == 0


# ── main ──

def main():
    global passed, failed, skipped
    p = argparse.ArgumentParser(description="Run all qt-commander tests")
    p.add_argument("--skip-cpp", action="store_true", help="Skip g++ tests")
    p.add_argument("--skip-e2e", action="store_true", help="Skip E2E tests")
    p.add_argument("--quick", action="store_true", help="Python + MSVC ctest only")
    args = p.parse_args()

    print("=== Python (pytest) ===")
    run("pytest", ["python", "-m", "pytest", "tests/unit_server/", "-q"])

    print("\n=== MSVC ctest ===", flush=True)
    if MSVC.exists():
        qt_bin = r"C:\Software\Qt\5.15.2\msvc2019_64\bin"
        run("ctest", ["ctest", "--output-on-failure"], cwd=MSVC,
            env={"PATH": f"{qt_bin};{os.environ['PATH']}"},
            skip_reason="" if MSVC.exists() else "build/msvc not found")
    else:
        print("  SKIP: build/msvc not found")
        skipped += 1

    if args.quick:
        print(f"\n{'='*40}\n  Passed: {passed}  Failed: {failed}  Skipped: {skipped}\n{'='*40}")
        return 0 if failed == 0 else 1

    # ── g++ tests ──
    if args.skip_cpp:
        print("\n=== g++ tests: SKIPPED ===")
    elif not subprocess.run(["g++", "--version"], capture_output=True).returncode == 0:
        print("\n=== g++ tests: g++ not found, skipping ===")
        skipped += 1
    else:
        BUILD.mkdir(exist_ok=True)
        print("\n=== Building g++ binaries ===", flush=True)

        targets = {
            "test_di.exe":     "tests/unit_injector/test_injector_logic.cpp  src/injector/injector_di.cpp",
            "test_pe.exe":     "tests/unit_injector/test_pe_real.cpp",
            "test_int.exe":    "tests/unit_injector/test_cli_integration.cpp src/injector/injector_di.cpp",
            "test_w32.exe":    "tests/unit_injector/test_win32_coverage.cpp",
            "test_sock.exe":   "tests/unit_injector/test_socket_utils.cpp",
            "test_injerr.exe": "tests/unit_injector/test_injector_errors.cpp src/injector/injector_win.cpp src/common/socket_utils.cpp",
            "test_e2e.exe":    "tests/unit_injector/test_e2e.cpp",
            "test_exit.exe":   "tests/unit_injector/test_e2e_exit_codes.cpp",
            "test_exit45.exe": "tests/unit_injector/test_exit45.cpp",
        }

        for out, src in targets.items():
            ok = gpp(src, f"build/{out}")
            if not ok:
                failed += 1

        # Run
        print("\n=== g++ tests ===", flush=True)
        tests = [
            ("DI injector logic", "build/test_di.exe"),
            ("PE parser (real)", "build/test_pe.exe"),
            ("Win32ProcessOps + CLI", "build/test_int.exe"),
            ("injector_win helpers", "build/test_w32.exe"),
            ("Socket utils", "build/test_sock.exe"),
        ]
        for name, exe in tests:
            run(name, [exe], skip_reason="" if Path(exe).exists() else "not built")

        # E2E
        if not args.skip_e2e:
            qt_bin = r"C:\Software\Qt\5.15.2\msvc2019_64\bin"
            env = {"PATH": f"{qt_bin};{MSVC / 'src' / 'library'};{os.environ['PATH']}"}
            print("\n=== E2E tests ===", flush=True)
            for name, exe in [
                ("E2E full lifecycle", "build/test_e2e.exe"),
                ("Exit codes 3/6", "build/test_exit.exe"),
                ("Exit codes 4/5", "build/test_exit45.exe"),
                ("Injector error path", "build/test_injerr.exe"),
            ]:
                run(name, [exe], env=env, skip_reason="" if Path(exe).exists() else "not built")

    print(f"\n{'='*40}")
    print(f"  Passed: {passed}  Failed: {failed}  Skipped: {skipped}")
    print("=" * 40)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
