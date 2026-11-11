#!/usr/bin/env python3
"""Cut a release: verify the tree, tag, push, and follow the Release workflow.

    python scripts/release.py 0.9.1
    python scripts/release.py 0.9.2 --dry-run
    python scripts/release.py 1.0.0 --skip-tests --no-wait

The tag *is* the version: this script decides the tag, and CI derives both the
wheel version and the compiled-in runtime version from it. It refuses to run on
a dirty tree, off ``master``, or when the tag already exists locally or on the
remote.

After pushing, it follows the ``Release`` workflow with ``gh`` and prints the
release URLs when the workflow finishes.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(?:[.-]?(?:a|b|rc)\d+)?$")
WORKFLOW = "Release"
WAIT_SECONDS = 1200


def run(args: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        args, cwd=ROOT, text=True, capture_output=True, check=check,
        encoding="utf-8", errors="replace",
    )


def git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return run(["git", *args], check=check)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("version", help="version to release, for example 0.9.1")
    parser.add_argument("--dry-run", action="store_true",
                        help="run every check, then stop before tagging")
    parser.add_argument("--skip-tests", action="store_true",
                        help="do not run the unit tests first")
    parser.add_argument("--no-wait", action="store_true",
                        help="do not follow the workflow after pushing")
    args = parser.parse_args()

    version = args.version.strip().lstrip("v")
    if not VERSION_RE.match(version):
        print(f"error: {version!r} is not a release version", file=sys.stderr)
        return 1
    tag = f"v{version}"

    status = git("status", "--porcelain").stdout.strip()
    if status:
        print("error: working tree is not clean:", file=sys.stderr)
        print(status, file=sys.stderr)
        return 1
    branch = git("rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    if branch != "master":
        print(f"error: releases are cut from master, not {branch}",
              file=sys.stderr)
        return 1
    if git("rev-parse", "-q", "--verify", f"refs/tags/{tag}",
           check=False).returncode == 0:
        print(f"error: tag {tag} already exists", file=sys.stderr)
        return 1
    remote_tag = git("ls-remote", "--tags", "origin", f"refs/tags/{tag}",
                     check=False).stdout.strip()
    if remote_tag:
        print(f"error: tag {tag} already exists on origin", file=sys.stderr)
        return 1

    unpushed = git("rev-list", "--count", "origin/master..master").stdout.strip()
    if unpushed not in {"", "0"}:
        print(f"error: master has {unpushed} unpushed commit(s); push them "
              "first", file=sys.stderr)
        return 1

    changelog = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    if version not in changelog:
        print(f"warning: CHANGELOG.md does not mention {version}",
              file=sys.stderr)

    if not args.skip_tests:
        print("running unit tests...")
        tests = run([sys.executable, "-m", "pytest", "-q"], check=False)
        if tests.returncode != 0:
            tail = "\n".join((tests.stdout or "").splitlines()[-40:])
            print(f"error: tests failed\n{tail}", file=sys.stderr)
            return 1
        print("tests passed")

    if args.dry_run:
        print(f"dry run: would tag {tag} on master and push it")
        return 0

    git("tag", "-a", tag, "-m", f"{tag}: release")
    print(f"tagged {tag}")

    push = git("push", "origin", tag, check=False)
    if push.returncode != 0:
        git("tag", "-d", tag, check=False)
        print("error: pushing the tag failed; the local tag was removed",
              file=sys.stderr)
        print((push.stderr or push.stdout).strip(), file=sys.stderr)
        return 1
    print(f"pushed {tag}")

    if args.no_wait or shutil.which("gh") is None:
        print("follow the workflow at "
              "https://github.com/tiefeiyu/qt-commander/actions/workflows/"
              "release.yml")
        return 0

    print(f"waiting for the {WORKFLOW} workflow...")
    deadline = time.monotonic() + WAIT_SECONDS
    run_id: str | None = None
    conclusion: str | None = None
    while time.monotonic() < deadline:
        time.sleep(15)
        listed = run(["gh", "run", "list", "--workflow", WORKFLOW, "--limit", "1",
                      "--json", "databaseId,status,conclusion,headBranch"],
                     check=False)
        if listed.returncode != 0:
            continue
        try:
            runs = json.loads(listed.stdout or "[]")
        except json.JSONDecodeError:
            continue
        if not runs:
            continue
        latest = runs[0]
        run_id = str(latest.get("databaseId"))
        if latest.get("status") == "completed":
            conclusion = latest.get("conclusion")
            break
        print(f"  {latest.get('status')} ...")

    if run_id is None:
        print("warning: could not find the workflow run; check the Actions tab",
              file=sys.stderr)
        return 0

    print(f"workflow {run_id}: {conclusion}")
    print(f"  https://github.com/tiefeiyu/qt-commander/actions/runs/{run_id}")
    if conclusion == "success":
        print(f"  https://github.com/tiefeiyu/qt-commander/releases/tag/{tag}")
        print(f"  https://pypi.org/project/qt-commander-mcp/{version}/")
        return 0
    print("workflow failed; inspect the failing job and rerun with "
          "workflow_dispatch (it accepts a tag input)",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
