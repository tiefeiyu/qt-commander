"""RPC client: manage qt-injector subprocess and TCP connection lifecycle."""
import asyncio
import json
from pathlib import Path

from .errors import InjectionError
from .session import Session


async def inject_and_connect(
    pid: int,
    lib_path: Path,
    port_file: Path,
    injector_exe: Path,
    session: Session,
    connect_timeout: float = 60.0,
) -> None:
    """Run qt-injector subprocess, parse output, connect TCP, authenticate."""
    port_file.parent.mkdir(parents=True, exist_ok=True)

    proc = await asyncio.create_subprocess_exec(
        str(injector_exe),
        str(pid),
        str(lib_path),
        str(port_file),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    try:
        stdout, stderr = await asyncio.wait_for(proc.communicate(), timeout=connect_timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        try:
            eject_proc = await asyncio.create_subprocess_exec(
                str(injector_exe), "--eject", str(pid), str(lib_path),
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL,
            )
            await asyncio.wait_for(eject_proc.wait(), timeout=15.0)
        except Exception:
            pass
        raise InjectionError(f"Injector process timed out after {connect_timeout}s")

    if proc.returncode != 0:
        err_msg = stderr.decode("utf-8", errors="replace").strip()
        raise InjectionError(f"Injector exited with code {proc.returncode}: {err_msg}")

    try:
        result = json.loads(stdout.decode("utf-8").strip())
    except json.JSONDecodeError as e:
        raise InjectionError(f"Failed to parse injector output: {e}")

    port = result.get("port", 0)
    token = result.get("token", "")
    if not port or not token:
        raise InjectionError("Invalid injector output: missing port or token")

    last_error = None
    for attempt in range(3):
        try:
            await session.connect(port, token)
            session.write_metadata()
            return
        except (ConnectionRefusedError, OSError) as e:
            last_error = e
            if attempt < 2:
                await asyncio.sleep(0.5)
    raise InjectionError(f"Failed to connect after 3 retries: {last_error}")
