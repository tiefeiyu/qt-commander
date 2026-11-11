"""``qt-commander`` command line: serve, doctor, install-artifacts.

The MCP server is still reachable through the ``qt-commander-mcp`` console
script (stdio, what MCP clients launch). This module adds the human-facing
commands needed to install prebuilt injector artifacts and to diagnose a
machine before an agent ever tries to attach.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Iterable

from . import __version__, artifacts

USER_AGENT = f"qt-commander/{__version__}"
RELEASE_REPO = "tiefeiyu/qt-commander"
SUMS_NAMES = ("SHA256SUMS", "SHA256SUMS.txt", "sha256sums")
ASSET_PREFIX = "qt-commander-artifacts-"
ENV_OVERRIDES = (
    "QT_COMMANDER_ARTIFACTS",
    "QT_COMMANDER_INJECT_DLL",
    "QT_COMMANDER_INJECTOR",
    "QT_COMMANDER_RENDEZVOUS",
    "QT_COMMANDER_SOURCE_DIR",
    "QT_COMMANDER_PROCESS_MODULES",
)


# --------------------------------------------------------------------------
# install-artifacts: paths, checksums, extraction
# --------------------------------------------------------------------------

def user_artifact_root() -> Path:
    """Writable per-user artifact root, used when the default is read-only."""
    override = os.environ.get("QT_COMMANDER_ARTIFACTS")
    if override:
        return Path(override).expanduser()
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or str(
            Path.home() / "AppData" / "Local"
        )
    else:
        base = os.environ.get("XDG_DATA_HOME") or str(
            Path.home() / ".local" / "share"
        )
    return Path(base) / "qt-commander" / "artifacts"


def resolve_install_root(explicit: str | None = None) -> Path:
    """Pick the artifact root to install into.

    A source checkout keeps using ``artifacts/`` (so ``git status`` shows what
    was downloaded). An installed wheel has a read-only package directory, so
    fall back to the per-user root.
    """
    if explicit:
        return Path(explicit).expanduser()
    current = artifacts.default_root()
    try:
        current.mkdir(parents=True, exist_ok=True)
        probe = current / ".write-probe"
        probe.write_bytes(b"")
        probe.unlink()
        return current
    except OSError:
        return user_artifact_root()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_sums(text: str) -> dict[str, str]:
    """Parse ``sha256sum`` output into ``{filename: digest}``."""
    sums: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        digest, name = parts[0].lower(), parts[1].lstrip("*").strip()
        if len(digest) == 64 and all(c in "0123456789abcdef" for c in digest):
            sums[name] = digest
    return sums


def _unsafe_member(name: str) -> str | None:
    """Return a reason when a zip member must not be extracted."""
    normalized = name.replace("\\", "/")
    if normalized.startswith("/") or normalized.startswith("//"):
        return "absolute path"
    if len(normalized) > 1 and normalized[1] == ":":
        return "drive-letter path"
    parts = [part for part in normalized.split("/") if part not in ("", ".")]
    if any(part == ".." for part in parts):
        return "path traversal"
    if not parts:
        return "empty name"
    return None


def inspect_archive(zip_path: Path) -> tuple[list[str], list[str]]:
    """Return ``(accepted, rejected)`` member names for an artifact zip."""
    accepted: list[str] = []
    rejected: list[str] = []
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            reason = _unsafe_member(info.filename)
            if reason is None and (info.external_attr >> 16) & 0o170000 == 0o120000:
                reason = "symlink"
            if reason is not None:
                rejected.append(f"{info.filename} ({reason})")
            elif info.is_dir() or info.filename.endswith("/"):
                continue
            else:
                accepted.append(info.filename.replace("\\", "/"))
    return accepted, rejected


def extract_artifacts(
    zip_path: Path, root: Path, *, force: bool = False
) -> dict[str, Any]:
    """Extract one artifact archive under ``root`` after validating members.

    Every accepted member must live under an ``<os>-<arch>/`` directory: the
    archive is installed as data, never as arbitrary files on the machine.
    """
    accepted, rejected = inspect_archive(zip_path)
    if rejected:
        raise ValueError(
            "refusing to extract unsafe archive members: " + "; ".join(rejected[:5])
        )
    tops = {name.split("/", 1)[0] for name in accepted}
    invalid = [name for name in tops if "-" not in name]
    if invalid:
        raise ValueError(
            "archive must contain <os>-<arch>/... entries, found: "
            + ", ".join(sorted(invalid)[:5])
        )
    root.mkdir(parents=True, exist_ok=True)
    written: list[str] = []
    with zipfile.ZipFile(zip_path) as archive:
        for name in accepted:
            target = root / name
            if target.exists() and not force:
                raise FileExistsError(
                    f"{target} already exists; pass --force to replace it"
                )
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(name) as source, target.open("wb") as sink:
                while True:
                    chunk = source.read(1 << 20)
                    if not chunk:
                        break
                    sink.write(chunk)
            written.append(str(target))
    return {
        "installed": len(written),
        "root": str(root),
        "directories": sorted(tops),
        "files": written,
    }


# --------------------------------------------------------------------------
# install-artifacts: sources
# --------------------------------------------------------------------------

def _request(url: str) -> bytes:
    request = urllib.request.Request(url, headers={
        "User-Agent": USER_AGENT,
        "Accept": "application/vnd.github+json, application/octet-stream",
    })
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"HTTP {exc.code} for {url}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"cannot reach {url}: {exc.reason}") from exc


def release_assets(repo: str, ref: str) -> dict[str, Any]:
    """Return the release metadata for ``ref`` (``latest`` or a tag)."""
    url = f"https://api.github.com/repos/{repo}/releases/{ref}"
    data = json.loads(_request(url).decode("utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError(f"unexpected release metadata for {repo}@{ref}")
    return data


def asset_matches(name: str, pattern: str | None) -> bool:
    if pattern:
        from fnmatch import fnmatch

        return fnmatch(name, pattern)
    return name.startswith(ASSET_PREFIX) and name.endswith(".zip")


def host_arch() -> str:
    """Map ``platform.machine()`` onto the artifact arch slug (x64/x86/arm64)."""
    machine = (platform.machine() or "").lower()
    if machine in {"amd64", "x86_64", "x64", "em64t"}:
        return "x64"
    if machine in {"arm64", "aarch64"}:
        return "arm64"
    if machine in {"x86", "i386", "i486", "i586", "i686", "x86-64"}:
        return "x86"
    return machine or "unknown"


def platform_asset_pattern() -> str:
    """Default asset filter for this machine."""
    return f"{ASSET_PREFIX}{artifacts.os_slug()}-{host_arch()}*.zip"


def download_to(url: str, directory: Path, name: str) -> Path:
    path = directory / name
    path.write_bytes(_request(url))
    return path


def install_artifacts(args: argparse.Namespace) -> int:
    sum_names = []
    if args.from_dir:
        source = Path(args.from_dir).expanduser()
        zips = sorted(source.glob(args.asset or f"{ASSET_PREFIX}*.zip"))
        if not zips:
            print(f"error: no {ASSET_PREFIX}*.zip under {source}", file=sys.stderr)
            return 1
        sums_path = next(
            (source / name for name in SUMS_NAMES if (source / name).is_file()),
            None,
        )
        sums = parse_sums(sums_path.read_text(encoding="utf-8")) if sums_path else {}
        root = resolve_install_root(args.root)
        for zip_path in zips:
            code = _install_one(zip_path, root, sums, args)
            if code:
                return code
        print(f"installed {len(zips)} archive(s) into {root}")
        return 0

    if args.from_url:
        root = resolve_install_root(args.root)
        with tempfile.TemporaryDirectory(prefix="qtc-artifacts-") as temp:
            name = Path(args.from_url.split("?")[0]).name or "artifacts.zip"
            zip_path = download_to(args.from_url, Path(temp), name)
            sums = {}
            if args.sums_url:
                sums = parse_sums(_request(args.sums_url).decode("utf-8"))
            return _install_one(zip_path, root, sums, args)

    try:
        release = release_assets(args.repo, args.release)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        print(
            "hint: pass --from-dir <dir> or --from-url <zip> for offline installs",
            file=sys.stderr,
        )
        return 1
    assets = release.get("assets") or []
    names = [asset.get("name", "") for asset in assets]
    pattern = args.asset or platform_asset_pattern()
    selected = [asset for asset in assets
                if asset_matches(asset.get("name", ""), pattern)]
    if args.list:
        print(json.dumps({"release": release.get("tag_name"),
                          "assets": names}, indent=2))
        return 0
    if not selected:
        print(
            f"error: release {release.get('tag_name')} has no asset matching "
            f"{pattern!r}",
            file=sys.stderr,
        )
        print("available: " + ", ".join(names), file=sys.stderr)
        return 1
    sums_asset = next(
        (asset for asset in assets if asset.get("name") in SUMS_NAMES), None
    )
    root = resolve_install_root(args.root)
    with tempfile.TemporaryDirectory(prefix="qtc-artifacts-") as temp:
        sums: dict[str, str] = {}
        if sums_asset:
            text = _request(sums_asset["browser_download_url"]).decode("utf-8")
            sums = parse_sums(text)
        for asset in selected:
            name = asset.get("name", "")
            zip_path = download_to(
                asset["browser_download_url"], Path(temp), name
            )
            code = _install_one(zip_path, root, sums, args)
            if code:
                return code
    print(f"installed {len(selected)} archive(s) from {release.get('tag_name')} "
          f"into {root}")
    return 0


def _install_one(
    zip_path: Path,
    root: Path,
    sums: dict[str, str],
    args: argparse.Namespace,
) -> int:
    expected = sums.get(zip_path.name)
    actual = sha256_file(zip_path)
    if expected and expected != actual:
        print(
            f"error: checksum mismatch for {zip_path.name}\n"
            f"  expected {expected}\n  actual   {actual}",
            file=sys.stderr,
        )
        return 1
    if not expected:
        print(
            f"warning: no checksum published for {zip_path.name}; "
            "verifying archive structure only",
            file=sys.stderr,
        )
    try:
        result = extract_artifacts(zip_path, root, force=args.force)
    except (ValueError, FileExistsError, zipfile.BadZipFile, OSError) as exc:
        print(f"error: {zip_path.name}: {exc}", file=sys.stderr)
        return 1
    print(
        f"installed {zip_path.name}: {result['installed']} files into "
        f"{', '.join(result['directories'])}"
    )
    return 0


# --------------------------------------------------------------------------
# doctor
# --------------------------------------------------------------------------

def _check(name: str, status: str, detail: str,
           hint: str | None = None) -> dict[str, Any]:
    check: dict[str, Any] = {"name": name, "status": status, "detail": detail}
    if hint:
        check["hint"] = hint
    return check


def _module_version(name: str) -> tuple[str | None, str | None]:
    try:
        module = __import__(name)
    except Exception as exc:  # noqa: BLE001 - any import failure is the report
        return None, str(exc)
    version = getattr(module, "__version__", None)
    if version is None:
        try:
            from importlib.metadata import version as dist_version

            version = dist_version(name)
        except Exception:  # noqa: BLE001
            version = "installed"
    return str(version), None


def doctor_checks(pid: int | None = None) -> list[dict[str, Any]]:
    """Collect preflight checks without raising: every failure is a row."""
    checks: list[dict[str, Any]] = []

    ok_python = sys.version_info >= (3, 10)
    checks.append(_check(
        "python",
        "ok" if ok_python else "fail",
        platform.python_version(),
        None if ok_python else "qt-commander requires Python 3.10 or newer",
    ))
    for module in ("fastmcp", "psutil"):
        version, error = _module_version(module)
        checks.append(_check(
            module,
            "ok" if version else "fail",
            version or str(error),
            None if version else f"pip install {module}",
        ))

    root = artifacts.default_root()
    entries = artifacts.listing()
    if entries:
        ids = sorted({entry.get("build_id") or "unknown" for entry in entries})
        checks.append(_check(
            "artifacts", "ok",
            f"{len(entries)} in {root}: {', '.join(ids[:4])}",
        ))
    else:
        checks.append(_check(
            "artifacts", "warn", f"none under {root}",
            "run qt-commander install-artifacts (or qt_build for_pid=<pid>)",
        ))

    overrides = {name: os.environ[name] for name in ENV_OVERRIDES
                 if os.environ.get(name)}
    if overrides:
        checks.append(_check(
            "env-overrides", "warn",
            ", ".join(f"{key}={value}" for key, value in overrides.items()),
        ))

    try:
        from .environment import detect_qt_environments, detect_toolchains

        kits = detect_qt_environments()
        checks.append(_check(
            "qt-kits",
            "ok" if kits else "warn",
            ", ".join(
                f"{item['version']} ({', '.join(kit['name'] for kit in item['kits'])})"
                for item in kits
            ) or "none found",
            None if kits else "set QTDIR or install Qt 5/6",
        ))
        tools = detect_toolchains()
        names = sorted({tool.get("name", "?") for tool in tools})
        checks.append(_check(
            "toolchains",
            "ok" if tools else "warn",
            ", ".join(names) or "none found",
            None if tools else "install MSVC / MinGW / gcc to build artifacts",
        ))
    except Exception as exc:  # noqa: BLE001 - discovery is best effort
        checks.append(_check("qt-kits", "warn", f"discovery failed: {exc}"))

    try:
        from . import server

        descriptors = server._descriptor_candidates()
        alive = [
            path for path in descriptors
            if server._descriptor_summary(path).get("pid_alive") is True
        ]
        checks.append(_check(
            "descriptors",
            "ok" if alive else "warn",
            f"{len(alive)} live / {len(descriptors)} total in "
            f"{server._rendezvous_dir()}",
            None if alive else
            "no live target; start a linked app or use qt_attach(pid=..., "
            "mode=\"injection\")",
        ))
    except Exception as exc:  # noqa: BLE001
        checks.append(_check("descriptors", "warn", f"unavailable: {exc}"))

    if pid is not None:
        checks.extend(_target_checks(pid))
    return checks


def _target_checks(pid: int) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []
    try:
        from . import target

        info = target.target_info(pid=pid)
    except Exception as exc:  # noqa: BLE001
        checks.append(_check("target", "fail", f"pid {pid}: {exc}",
                             "check the pid with qt_processes"))
        return checks
    runtime_version = (info.get("descriptor") or {}).get("library_version")
    detail = (
        f"pid {pid} qt{info.get('qt_major')} {info.get('qt_version')} "
        f"{'debug' if info.get('debug') else 'release'} "
        f"{info.get('toolchain')} ({info.get('detected_from')})"
    )
    if runtime_version:
        detail += f", runtime {runtime_version}"
    checks.append(_check("target", "ok", detail))
    expected = info.get("expected_artifact")
    if expected:
        detail = f"{expected.get('build_id')} at {expected.get('directory')}"
        if expected.get("version"):
            detail += f" (built from {expected['version']}"
            if expected.get("commit"):
                detail += f", {expected['commit'][:8]}"
            detail += ")"
        checks.append(_check("target-artifact", "ok", detail))
    else:
        error = (info.get("expected_artifact_error") or {}).get("message", "")
        checks.append(_check(
            "target-artifact", "warn", error or "no artifact matches this target",
            f"run qt_build(for_pid={pid}) or qt-commander install-artifacts",
        ))
    state = info.get("injection_state")
    loaded = info.get("loaded_injector") or {}
    if loaded.get("loaded"):
        status = "warn" if info.get("restart_required") else "ok"
        detail = f"{state}: {loaded.get('build_id') or loaded.get('path')}"
        artifact = loaded.get("artifact") or {}
        if artifact.get("version"):
            detail += f" (v{artifact['version']}"
            if artifact.get("commit"):
                detail += f", {artifact['commit'][:8]}"
            detail += ")"
        if info.get("restart_required"):
            detail += ", restart required"
        checks.append(_check(
            "target-injector", status, detail,
            "restart the target before injecting again"
            if info.get("restart_required") else None,
        ))
    return checks


def doctor(args: argparse.Namespace) -> int:
    checks = doctor_checks(args.pid)
    failures = [check for check in checks if check["status"] == "fail"]
    warnings = [check for check in checks if check["status"] == "warn"]
    if args.json:
        print(json.dumps({
            "ok": not failures,
            "version": __version__,
            "checks": checks,
        }, indent=2))
        return 1 if failures else 0
    print(f"qt-commander {__version__} doctor")
    for check in checks:
        mark = {"ok": "[ok]  ", "warn": "[warn]", "fail": "[fail]"}[check["status"]]
        print(f"  {mark} {check['name']}: {check['detail']}")
        if check.get("hint"):
            print(f"         -> {check['hint']}")
    print(
        f"\n{len(checks) - len(failures) - len(warnings)} ok, "
        f"{len(warnings)} warning(s), {len(failures)} failure(s)"
    )
    return 1 if failures else 0


# --------------------------------------------------------------------------
# entry point
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="qt-commander",
        description="Live Qt UI automation for agents (MCP server + tooling).",
    )
    parser.add_argument("--version", action="version",
                        version=f"qt-commander {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    serve = sub.add_parser(
        "serve", help="run the MCP server on stdio (what MCP clients launch)"
    )
    serve.set_defaults(handler=_serve)

    doc = sub.add_parser("doctor", help="check this machine before attaching")
    doc.add_argument("--pid", type=int, default=None,
                     help="also inspect one live target process")
    doc.add_argument("--json", action="store_true", help="machine-readable output")
    doc.set_defaults(handler=doctor)

    install = sub.add_parser(
        "install-artifacts", help="install prebuilt injector artifacts"
    )
    install.add_argument("--root", default=None,
                         help="artifact root (default: checkout artifacts/ or a per-user dir)")
    install.add_argument("--repo", default=RELEASE_REPO,
                         help=f"GitHub repo with release assets (default: {RELEASE_REPO})")
    install.add_argument("--release", default="latest",
                         help="release tag, or 'latest'")
    install.add_argument("--asset", default=None,
                         help="asset glob (default: this OS/arch)")
    install.add_argument("--from-dir", default=None,
                         help="install from a local directory of artifact zips")
    install.add_argument("--from-url", default=None,
                         help="install from one release zip URL")
    install.add_argument("--sums-url", default=None,
                         help="SHA256SUMS URL for --from-url")
    install.add_argument("--list", action="store_true",
                         help="list release assets and exit")
    install.add_argument("--force", action="store_true",
                         help="replace existing files")
    install.set_defaults(handler=install_artifacts)
    return parser


def _serve(args: argparse.Namespace) -> int:  # noqa: ARG001 - argparse handler
    from .server import main as serve_main

    serve_main()
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    try:
        return int(args.handler(args) or 0)
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
