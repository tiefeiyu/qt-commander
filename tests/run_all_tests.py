"""One-command full test runner.

Runs every test suite in the project, in order:

  1. Python unit tests (pytest over tests/)
  2. Library C++ unit tests  (.qt-commander/test-apps-build/tests/unit_library)
  3. Injector C++ unit tests (.qt-commander/injector-tests-build/tests)
  4. End-to-end preload verification (tests/verify_preload.py)

Usage:
    python tests/run_all_tests.py              # full run
    python tests/run_all_tests.py --skip-e2e   # skip the slow E2E scenarios
    python tests/run_all_tests.py -v           # show every test's output

Exit code 0 = everything passed.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
QTC = REPO / ".qt-commander"
QT_BIN = Path(r"C:\Software\Qt\5.15.2\msvc2019_64\bin")

LIB_TEST_DIR = QTC / "test-apps-build" / "tests" / "unit_library"
INJ_TEST_DIR = QTC / "injector-tests-build" / "tests"

LIB_TESTS = ["test_element_map.exe", "test_selector.exe",
             "test_handler.exe", "test_rpc_server.exe"]
INJ_TESTS = [  # injector suites (test_e2e needs the deployed app on PATH)
    "test_injector_logic.exe", "test_pe_parser.exe", "test_pe_real.exe",
    "test_import_parser.exe", "test_injector_cli.exe",
    "test_win32_coverage.exe", "test_socket_utils.exe",
    "test_framing_cross.exe", "test_port_poll.exe",
    "test_injector_errors.exe", "test_e2e_exit_codes.exe",
    "test_exit45.exe", "test_cli_integration.exe", "test_e2e.exe",
]

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


def run(cmd: list[str], env: dict, verbose: bool, timeout: int = 600) -> int:
    proc = subprocess.run(cmd, env=env, capture_output=not verbose,
                          text=True, timeout=timeout)
    if verbose:
        print(proc.stdout or "", end="")
        print(proc.stderr or "", end="")
    return proc.returncode


def section(title: str) -> None:
    print(f"\n{'=' * 66}\n{title}\n{'=' * 66}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--skip-e2e", action="store_true",
                    help="skip the E2E preload verification scenarios")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="show all test output instead of a summary")
    args = ap.parse_args()

    env = os.environ.copy()
    results: list[tuple[str, str, str]] = []  # (suite, outcome, detail)

    def add(suite: str, ok: bool, detail: str = "") -> None:
        results.append((suite, PASS if ok else FAIL, detail))
        print(f"  [{PASS if ok else FAIL}] {suite}"
              + (f"  ({detail})" if detail else ""))

    # ---- 1. Python ---------------------------------------------------------
    section("1/4 Python unit tests (pytest)")
    r = run([sys.executable, "-m", "pytest", "tests", "-q"], env,
            args.verbose)
    add("pytest tests", r == 0,
        f"exit {r}" if r else "")

    # ---- 2. Library C++ ----------------------------------------------------
    section("2/4 Library C++ unit tests")
    env["PATH"] = str(QT_BIN) + ";" + env.get("PATH", "")
    if not LIB_TEST_DIR.exists():
        add("library C++", False, "build dir missing: run qt_build + "
            "cmake configure first")
    else:
        for exe in LIB_TESTS:
            p = LIB_TEST_DIR / exe
            if not p.exists():
                add(exe, False, "not built")
                continue
            r = run([str(p)], env, args.verbose)
            add(exe, r == 0, f"exit {r}" if r else "")

    # ---- 3. Injector C++ ---------------------------------------------------
    section("3/4 Injector C++ unit tests")
    if not INJ_TEST_DIR.exists():
        add("injector C++", False, "build dir missing: run cmake configure "
            "with BUILD_INJECTOR=ON first")
    else:
        # qt-injector / test apps must be resolvable for the E2E suites.
        env["PATH"] = str(QTC / "bin") + ";" + str(QTC / "test-app" / "bin") \
            + ";" + env.get("PATH", "")
        for exe in INJ_TESTS:
            p = INJ_TEST_DIR / exe
            if not p.exists():
                add(exe, False, "not built")
                continue
            r = run([str(p)], env, args.verbose)
            add(exe, r == 0, f"exit {r}" if r else "")

    # ---- 4. E2E preload verification ---------------------------------------
    if args.skip_e2e:
        add("verify_preload", True, "skipped by --skip-e2e")
    else:
        section("4/4 E2E preload verification (verify_preload.py)")
        r = run([sys.executable, str(REPO / "tests" / "verify_preload.py")],
                env, args.verbose, timeout=900)
        add("verify_preload", r == 0, f"exit {r}" if r else "")

    # ---- Summary -----------------------------------------------------------
    print(f"\n{'=' * 66}\nSUMMARY\n{'=' * 66}")
    failed = 0
    for suite, outcome, detail in results:
        print(f"  [{outcome}] {suite}{'  (' + detail + ')' if detail else ''}")
        if outcome == FAIL:
            failed += 1
    print(f"\n{len(results) - failed}/{len(results)} suites passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
