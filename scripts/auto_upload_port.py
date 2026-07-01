import os
from typing import Any

from serial.tools import list_ports


Import("env")  # type: ignore


def _is_usb_serial_port(port: Any) -> bool:
    desc = (port.description or "").lower()
    hwid = (port.hwid or "").lower()

    # Skip classic motherboard serial ports like COM1.
    if "acpi" in hwid and "pnp0501" in hwid:
        return False

    # Common USB serial bridge indicators and native ESP USB VID/PID.
    markers = (
        "usb serial",
        "usb-serial",
        "ch340",
        "cp210",
        "ftdi",
        "vid:pid=303a:",  # Espressif
        "vid:pid=1a86:",  # CH340
        "vid:pid=10c4:",  # CP210x
        "vid:pid=0403:",  # FTDI
    )

    return any(m in desc or m in hwid for m in markers)


def _pick_upload_port() -> str | None:
    pioenv = env.get("PIOENV")
    if pioenv:
        try:
            project_config = env.GetProjectConfig()
            configured = project_config.get(f"env:{pioenv}", "upload_port")
            if configured:
                return str(configured)
        except Exception:
            pass

    configured = env.GetProjectOption("upload_port", "")
    if configured:
        return str(configured)

    override = os.environ.get("OPENSHOCK_UPLOAD_PORT")
    if override:
        return override

    candidates = [p for p in list_ports.comports() if _is_usb_serial_port(p)]
    if not candidates:
        return None

    # Prefer Espressif native USB first, then any USB serial bridge.
    def score(p: Any) -> int:
        hwid = (p.hwid or "").lower()
        desc = (p.description or "").lower()
        if "vid:pid=303a:" in hwid:
            return 0
        if "ch340" in desc or "vid:pid=1a86:" in hwid:
            return 1
        if "cp210" in desc or "vid:pid=10c4:" in hwid:
            return 2
        if "ftdi" in desc or "vid:pid=0403:" in hwid:
            return 3
        return 10

    candidates.sort(key=lambda p: (score(p), p.device))
    return candidates[0].device


port = _pick_upload_port()
if port:
    env.Replace(UPLOAD_PORT=port)
    print(f"[OpenShock] Auto-selected upload port: {port}")
else:
    print("[OpenShock] No USB serial upload port auto-detected; using PlatformIO default")
