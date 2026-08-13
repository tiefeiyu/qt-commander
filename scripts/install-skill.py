#!/usr/bin/env python3
"""One-click install of the qt-commander-ui skill to the user-level skills
directory (~/.claude/skills).  Cross-platform (Windows / Linux / macOS).

The skill is picked up by every Claude Code session of this user -- the
same scope as the qt-commander MCP server (configured at user level in
~/.claude.json).  Run once after cloning/distributing this repo:

    python scripts/install-skill.py            # copy mode (default)
    python scripts/install-skill.py --link     # symlink/junction mode

With --link, edits in the repo are picked up without re-running the script
(the link breaks if the repo is moved or deleted).
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

SKILL_NAME = "qt-commander-ui"


def user_skills_dir() -> Path:
    return Path.home() / ".claude" / "skills"


def _is_reparse_point(path: Path) -> bool:
    """True for Windows junctions/symlinks (FILE_ATTRIBUTE_REPARSE_POINT).

    Path.is_symlink() only reports junction targets on Python 3.12+;
    checking the attribute directly works on every supported Python.
    """
    if os.name != "nt":
        return False
    attrs = os.lstat(path).st_file_attributes
    return bool(attrs & 0x400)


def remove_path(path: Path) -> None:
    """Remove a previous install: directory tree, file, or symlink/junction."""
    if path.is_dir() and not _is_reparse_point(path):
        shutil.rmtree(path)
    else:
        # unlink() removes the link itself (never the link target).
        path.unlink()


def install_link(src: Path, dst: Path) -> None:
    if os.name == "nt":
        # Directory junction via cmd's mklink -- no admin rights needed
        # (unlike directory symlinks on Windows).
        subprocess.run(
            ["cmd", "/c", "mklink", "/J", str(dst), str(src)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
    else:
        dst.symlink_to(src, target_is_directory=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=f"Install the {SKILL_NAME} skill to ~/.claude/skills"
    )
    parser.add_argument(
        "--link",
        action="store_true",
        help="create a symlink/junction to the repo instead of copying",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    src = repo_root / ".claude" / "skills" / SKILL_NAME
    dst = user_skills_dir() / SKILL_NAME

    if not src.is_dir():
        print(f"error: source skill not found: {src}", file=sys.stderr)
        return 1

    user_skills_dir().mkdir(parents=True, exist_ok=True)

    # lexists() is True even for a broken junction/symlink, so a dangling
    # link from a moved repo gets cleaned up too.
    if os.path.lexists(dst):
        remove_path(dst)

    if args.link:
        install_link(src, dst)
        print(f"linked  {dst}  ->  {src}")
    else:
        shutil.copytree(src, dst)
        print(f"copied  {src}  ->  {dst}")

    print(f"{SKILL_NAME} skill installed. It is now available to all")
    print("Claude Code sessions of this user (same scope as the MCP server).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
