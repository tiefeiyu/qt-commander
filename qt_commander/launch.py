"""Launch a target with the runtime preloaded into it.

On Linux and macOS the dynamic loader can insert a library before ``main``:
``LD_PRELOAD`` and ``DYLD_INSERT_LIBRARIES`` respectively. That is the *only*
way to run the runtime inside a macOS application — macOS has no equivalent of
``CreateRemoteThread(LoadLibraryW)``, and ``task_for_pid`` is refused for
hardened or notarized builds. On Linux it also avoids ptrace entirely, so no
Yama configuration and no parent-process requirement.

Windows has no equivalent environment hook, which is why this module reports
itself unsupported there and the MCP tool is not registered.
"""

from __future__ import annotations

import os
import plistlib
import subprocess
import sys
from pathlib import Path
from typing import Mapping, Sequence

#: Directories whose binaries have their DYLD_* environment stripped by SIP.
SIP_PREFIXES = ("/usr/", "/System/", "/bin/", "/sbin/")

_PRELOAD_ENV = {"linux": "LD_PRELOAD", "darwin": "DYLD_INSERT_LIBRARIES"}


class LaunchError(RuntimeError):
    """A launch that could not be attempted; ``code`` is a stable string."""

    def __init__(self, code: str, message: str, hint: str | None = None):
        super().__init__(message)
        self.code = code
        self.hint = hint


def platform_key() -> str:
    """Return ``linux``/``darwin``/``windows``/other for the current host."""
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "darwin"
    return "windows" if sys.platform.startswith("win") else sys.platform


def supported() -> bool:
    """True when this host can preload the runtime at launch."""
    return platform_key() in _PRELOAD_ENV


def preload_variable() -> str | None:
    """The loader variable for this host, or None when unsupported."""
    return _PRELOAD_ENV.get(platform_key())


def resolve_executable(program: str | os.PathLike[str]) -> Path:
    """Resolve a program path, expanding a macOS ``.app`` bundle."""
    path = Path(program).expanduser()
    is_bundle = path.suffix.lower() == ".app" or (
        path.is_dir() and path.name.lower().endswith(".app")
    )
    if not is_bundle:
        return path
    contents = path / "Contents"
    executable: Path | None = None
    plist_path = contents / "Info.plist"
    try:
        with plist_path.open("rb") as stream:
            name = plistlib.load(stream).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException, ValueError):
        name = None
    if isinstance(name, str) and name:
        candidate = contents / "MacOS" / name
        if candidate.is_file():
            executable = candidate
    if executable is None:
        macos_dir = contents / "MacOS"
        candidates = []
        if macos_dir.is_dir():
            candidates = sorted(
                entry for entry in macos_dir.iterdir()
                if entry.is_file() and os.access(entry, os.X_OK)
            )
        if len(candidates) == 1:
            executable = candidates[0]
        elif candidates:
            names = ", ".join(entry.name for entry in candidates[:5])
            raise LaunchError(
                "E_LAUNCH_AMBIGUOUS",
                f"{path} has several executables in Contents/MacOS: {names}",
                "pass the executable inside the bundle instead of the .app",
            )
        else:
            raise LaunchError(
                "E_LAUNCH_NO_TARGET",
                f"{path} has no executable under Contents/MacOS",
                "pass the binary directly, or check the bundle layout",
            )
    return executable


def sip_protected(executable: str | os.PathLike[str]) -> bool:
    """True when SIP strips DYLD_* for this binary."""
    try:
        resolved = str(Path(executable).resolve())
    except OSError:
        resolved = str(executable)
    return any(resolved.startswith(prefix) for prefix in SIP_PREFIXES)


def build_environment(
    library: str | os.PathLike[str],
    *,
    base: Mapping[str, str] | None = None,
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Return an environment that preloads ``library``, keeping any existing entry."""
    variable = preload_variable()
    if variable is None:
        raise LaunchError(
            "E_LAUNCH_UNSUPPORTED",
            f"preloading is not available on {platform_key()}",
            "use qt_attach(mode=\"injection\") with a running target instead",
        )
    environment = dict(os.environ if base is None else base)
    existing = environment.get(variable, "")
    library_text = str(library)
    environment[variable] = (
        f"{library_text}{os.pathsep}{existing}" if existing else library_text
    )
    if extra:
        environment.update({str(key): str(value) for key, value in extra.items()})
    return environment


def launch(
    program: str | os.PathLike[str],
    args: Sequence[str] = (),
    *,
    library: str | os.PathLike[str],
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
    detach: bool = True,
) -> subprocess.Popen:
    """Start ``program`` with the runtime preloaded.

    ``detach`` puts the child in its own session so it outlives the MCP server
    (the agent usually wants the application to keep running).
    """
    if not supported():
        raise LaunchError(
            "E_LAUNCH_UNSUPPORTED",
            f"preloading is not available on {platform_key()}",
            "use qt_attach(mode=\"injection\") with a running target instead",
        )
    library_path = Path(library).expanduser()
    if not library_path.is_file():
        raise LaunchError(
            "E_LAUNCH_NO_LIBRARY",
            f"injection library not found: {library_path}",
            "build it (qt_build / scripts/package_artifacts.py) or pass library=",
        )
    executable = resolve_executable(program)
    if not executable.is_file():
        raise LaunchError(
            "E_LAUNCH_NO_TARGET",
            f"target not found: {executable}",
            "pass an executable path or a macOS .app bundle",
        )
    if platform_key() == "darwin" and sip_protected(executable):
        raise LaunchError(
            "E_LAUNCH_SIP",
            f"{executable} lives in a SIP-protected path; macOS strips "
            "DYLD_INSERT_LIBRARIES for it",
            "use a user-built binary outside /usr, /System, /bin and /sbin, "
            "or use linked mode",
        )
    environment = build_environment(library_path, extra=env)
    command = [str(executable), *[str(arg) for arg in args]]
    try:
        return subprocess.Popen(
            command,
            cwd=str(cwd) if cwd is not None else None,
            env=environment,
            start_new_session=detach,
            close_fds=True,
        )
    except OSError as exc:
        raise LaunchError(
            "E_LAUNCH_FAILED",
            f"cannot start {executable}: {exc}",
            "check the path, its permissions and the architecture",
        ) from exc
