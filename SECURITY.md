# Security policy

## Supported versions

Security fixes land on the latest release line (currently `2.x`). The `0.x`
tags were pre-release milestones built from source and are not supported.

## What this tool can do to your machine

qt-commander is an automation runtime, not a sandbox. Understand the two
things it does before using it:

1. **It injects a shared library into a running process.** `qt_attach(mode="injection")`
   loads `qt_commander_inject.dll` / `libqt_commander_inject.so` into the target
   (Windows: `CreateRemoteThread(LoadLibraryW)`; Linux: `ptrace` + `dlopen`).
   That is the same capability class as a debugger or an instrumentation
   profiler, and endpoint security products are entitled to treat it as such.
2. **It opens a local control channel.** The injected library listens on
   `127.0.0.1` with a random port and a random token, and writes a descriptor
   file into the rendezvous directory (`%TEMP%\qt-commander` by default,
   `~/.local/state` style paths on Linux, user-only permissions where the OS
   supports it). The token is stored in that file; treat it like a password for
   the session.

Neither of these is hidden: the descriptor, the listening socket, and the loaded
module are all discoverable by the operating system and by the target's own
process listing.

## Authorized use

Only attach to processes you own or are explicitly authorized to instrument.
Injecting into third-party software may violate its licence terms or local law,
and running automation against a production system can cause real, irreversible
changes (`qt_act` can click "delete", "upload", "exit"). Get consent before
attaching to a system that matters, keep a human in the loop for destructive
actions, and prefer a test instance.

## Launch-time injection (Linux and macOS)

`qt_launch` does not inject into a running process; it starts the application
with the runtime in the loader's preload list:

- **Linux** uses `LD_PRELOAD`. The variable is inherited by the child, ignored
  for setuid binaries, and needs no ptrace permission.
- **macOS** uses `DYLD_INSERT_LIBRARIES`. System Integrity Protection strips it
  for binaries under `/usr`, `/System`, `/bin` and `/sbin`; an app built with
  the hardened runtime ignores it unless the dylib is signed by the same Team
  and library validation is disabled
  (`com.apple.security.cs.disable-library-validation`). A downloaded dylib also
  carries the quarantine attribute — clear it (`xattr -dr com.apple.quarantine`)
  or sign and notarize your own build.
- **Windows** has no equivalent hook, so the tool is not registered there.

Screen-level screenshots on macOS need the Screen Recording permission
(`QScreen::grabWindow`); widget-level captures (`QWidget::grab`) do not.

## Hardening checklist

- Install artifacts from releases published in this repository; every archive
  is covered by `SHA256SUMS`, and `qt-commander install-artifacts` verifies it.
  Do not set `QT_COMMANDER_INJECT_DLL` to a library you did not build or verify.
- Run the MCP server as the same user as the target; do not elevate unless the
  target itself is elevated.
- Allow-list the injector in your EDR/AV tooling rather than disabling
  protection, and code-sign the binaries you redistribute.
- Keep the rendezvous directory on a local filesystem, and delete descriptors
  for processes you have finished with (`qt_detach(purge=True)`).
- Prefer linked mode (`qt_commander::start()`) when you control the source:
  nothing is injected, and the descriptor is written by the application itself.

## Reporting a vulnerability

Email the maintainer at the address in `pyproject.toml` (`TieFeiyu`), or open a
private security advisory on GitHub. Please include the affected version, the
platform, and a minimal reproduction.

Do not open a public issue for a vulnerability. Expect an acknowledgement
within a few days and a coordinated disclosure within 90 days; credit will be
given unless you prefer otherwise.
