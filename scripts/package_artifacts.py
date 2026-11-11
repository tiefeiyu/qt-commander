#!/usr/bin/env python3
"""Package a CMake build tree into the qt-commander artifact layout.

Used by the release workflow to turn each CI build into a downloadable
archive, and available to anyone who builds the runtime by hand:

    cmake -S . -B build/release -G Ninja -DQT_MAJOR_VERSION=5 \\
          -DCMAKE_BUILD_TYPE=Release
    cmake --build build/release

    python scripts/package_artifacts.py \\
        --build-dir build/release --qt-version 5.15.2 \\
        --toolchain msvc --build-type Release

The resulting directory matches what ``qt_attach(mode="injection")`` expects:
``artifacts/<os>-<arch>/<compiler>-<major.minor>/qt-<version>-<debug|release>/``
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from qt_commander import artifacts  # noqa: E402


def write_archive(root: Path, top: str, archive_path: Path) -> Path:
    """Zip one ``<os>-<arch>/`` subtree, preserving that directory at the top.

    ``qt-commander install-artifacts`` rejects archives whose top level is not
    an ``<os>-<arch>/`` directory, so the layout is part of the contract.
    """
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted((root / top).rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(root).as_posix())
    return archive_path


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="CMake build directory containing the built runtime")
    parser.add_argument("--qt-version", required=True,
                        help="full Qt version, e.g. 5.15.2")
    parser.add_argument("--toolchain", required=True,
                        choices=["msvc", "mingw", "gcc", "clang"],
                        help="compiler family used for the build")
    parser.add_argument("--compiler-version", default=None,
                        help="compiler version (default: probe the compiler)")
    parser.add_argument("--compiler", default=None,
                        help="compiler executable to probe for the version")
    parser.add_argument("--build-type", default="Release",
                        choices=["Debug", "Release", "RelWithDebInfo", "MinSizeRel"])
    parser.add_argument("--qt-debug", dest="qt_debug", action="store_true",
                        help="the runtime links the debug Qt (Qt5Cored.dll)")
    parser.add_argument("--qt-release", dest="qt_debug", action="store_false",
                        help="the runtime links the release Qt (default)")
    parser.add_argument("--arch", default="x64", help="target architecture slug")
    parser.add_argument("--os", dest="os_name", default=None,
                        help="target OS slug (default: this host)")
    parser.add_argument("--root", default=None, type=Path,
                        help="artifact root (default: artifacts/)")
    parser.add_argument("--archive-dir", default=None, type=Path,
                        help="also write <artifact>.zip here (CI release assets)")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--github-output", action="store_true",
                        help="append results to $GITHUB_OUTPUT (CI usage)")
    parser.set_defaults(qt_debug=None)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    build = args.build_dir.resolve()
    if not build.is_dir():
        print(f"error: build dir not found: {build}", file=sys.stderr)
        return 1
    os_name = args.os_name or artifacts.os_slug()
    compiler_version = args.compiler_version or artifacts.compiler_version(
        args.toolchain, args.compiler
    )
    qt_debug = args.qt_debug
    if qt_debug is None:
        qt_debug = args.build_type in {"Debug", "RelWithDebInfo"}
        print(
            "note: --qt-debug/--qt-release not given; assuming "
            f"{'debug' if qt_debug else 'release'} Qt from build type "
            f"{args.build_type}",
            file=sys.stderr,
        )
    installed = artifacts.install_from_build(
        build,
        os_name=os_name,
        arch=args.arch,
        compiler=args.toolchain,
        compiler_version=compiler_version,
        qt_version=args.qt_version,
        qt_debug=qt_debug,
        build_type=args.build_type,
        root=args.root,
    )
    if installed["manifest"] is None:
        print(
            "error: no qtc-inject/qt_commander_inject binary under "
            f"{build}; did the build succeed?",
            file=sys.stderr,
        )
        return 1
    payload = {
        "artifact_dir": str(installed["artifact_dir"]),
        "build_id": installed["build_id"],
        "manifest": str(installed["manifest"]),
        "injector": str(installed["injector"]),
        "library": str(installed["library"]),
        "compiler_version": compiler_version,
    }
    root = args.root or artifacts.default_root()
    relative = installed["artifact_dir"].relative_to(root)
    payload["relative_dir"] = relative.as_posix()
    payload["archive_name"] = (
        f"qt-commander-artifacts-{relative.parts[0]}-{relative.parts[-1]}.zip"
    )
    if args.github_output:
        output = os.environ.get("GITHUB_OUTPUT")
        if output:
            with open(output, "a", encoding="utf-8") as stream:
                stream.write(f"artifact_dir={payload['artifact_dir']}\n")
                stream.write(f"relative_dir={payload['relative_dir']}\n")
                stream.write(f"archive_name={payload['archive_name']}\n")
                stream.write(f"build_id={payload['build_id']}\n")
    if args.archive_dir:
        archive_path = args.archive_dir / payload["archive_name"]
        payload["archive"] = str(write_archive(root, relative.parts[0], archive_path))
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"artifact: {payload['artifact_dir']}")
        print(f"build_id: {payload['build_id']}")
        print(f"manifest: {payload['manifest']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
