"""PlatformIO pre-upload hook: prefer OTA, fall back to USB serial.

Before an upload, probe for the device on the LAN (mDNS host, default
``s-term.local``). If it answers, switch the upload to ArduinoOTA over WiFi;
otherwise leave the default esptool serial upload in place. This makes
``pio run -t upload`` do the right thing with no env switch or flag.

Environment overrides:
  STERM_OTA_HOST      mDNS host to probe (default ``s-term.local``)
  STERM_OTA_PASSWORD  OTA push password, if one is set in the device /CONFIG
  STERM_FORCE_USB     set to ``1`` to skip the probe and always use USB
"""

import os
import socket
import subprocess
import sys

Import("env")  # noqa: F821 — injected by PlatformIO

# Default the OTA host per board so `pio run -e rt` can't accidentally flash a
# T-Deck that is also on the LAN as s-term.local. Override with STERM_OTA_HOST.
_pioenv = env["PIOENV"]  # noqa: F821
_default_host = "reterm.local" if ("reterminal" in _pioenv or _pioenv in ("rt", "rtdebug")) else "s-term.local"
OTA_HOST = os.environ.get("STERM_OTA_HOST", _default_host)
OTA_PASSWORD = os.environ.get("STERM_OTA_PASSWORD", "")
RESOLVE_TIMEOUT_S = 2.0


def _resolve(host):
    """Resolve host to an IPv4 address, bounded by a short timeout.

    mDNS (.local) lookups can hang, so run getaddrinfo in a thread and give up
    if it doesn't answer quickly — a non-answer means "device not present".
    """
    result = {}

    def worker():
        try:
            infos = socket.getaddrinfo(host, None, family=socket.AF_INET)
            result["ip"] = infos[0][4][0] if infos else None
        except OSError:
            result["ip"] = None

    import threading

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(RESOLVE_TIMEOUT_S)
    return result.get("ip")


def _ping(ip):
    if sys.platform == "darwin":
        cmd = ["ping", "-c", "1", "-W", "1000", ip]  # macOS: -W is milliseconds
    elif sys.platform.startswith("linux"):
        cmd = ["ping", "-c", "1", "-W", "1", ip]  # linux: -W is seconds
    else:
        cmd = ["ping", "-n", "1", "-w", "1000", ip]  # windows
    try:
        return subprocess.run(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        ).returncode == 0
    except OSError:
        return False


def _use_ota():
    if os.environ.get("STERM_FORCE_USB") == "1":
        print("upload: STERM_FORCE_USB=1 — using USB serial")
        return False
    ip = _resolve(OTA_HOST)
    if ip and _ping(ip):
        print("upload: %s reachable at %s — uploading over WiFi (OTA)" % (OTA_HOST, ip))
        return True
    print("upload: %s not reachable — falling back to USB serial" % OTA_HOST)
    return False


if _use_ota():
    env.Replace(UPLOAD_PROTOCOL="espota", UPLOAD_PORT=OTA_HOST)  # noqa: F821
    if OTA_PASSWORD:
        env.Append(UPLOAD_FLAGS=["--auth=%s" % OTA_PASSWORD])  # noqa: F821
