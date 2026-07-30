#!/usr/bin/env python3
"""
claudius.py — Claude Code companion for the GeekMagic-S3 screen.

Polls `claude agents --json` for live session state and pushes usage +
per-session detail over WebSocket (mDNS `_claudius._tcp` → ws://host:8765).

Usage:
    pip install websockets zeroconf
    python3 companion/claudius.py --name my-laptop
    python3 companion/claudius.py --name my-laptop --secret mypassword
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from typing import Any

HAVE_ZEROCONF = False
try:
    from zeroconf import ServiceInfo, Zeroconf
    HAVE_ZEROCONF = True
except ImportError:
    ServiceInfo = None  # type: ignore
    Zeroconf = None  # type: ignore


def _import_websockets():
    """Lazy import so startup can fail with a clear pip hint."""
    try:
        from websockets.asyncio.server import serve as ws_serve
        return ws_serve
    except ImportError:
        pass
    try:
        from websockets.server import serve as ws_serve  # type: ignore
        return ws_serve
    except ImportError:
        print("Missing dependency: pip install websockets", file=sys.stderr)
        sys.exit(1)


# ── Constants ────────────────────────────────────────────────────────────────

CONFIG_HOME = os.path.expanduser(
    os.environ.get("GM_CLAUDE_CONFIG_HOME", "~/.config/gm-claude"))
CLAUDE_CREDS = os.path.expanduser("~/.claude/.credentials.json")
CLAUDE_KEYCHAIN_SERVICE = "Claude Code-credentials"
USAGE_API = "https://claude.ai/api/oauth/usage"
WS_PORT_DEFAULT = 8765
USAGE_INTERVAL = 60
AGENTS_INTERVAL = 2.0
MAX_SESSIONS = 8
AGENT_ID = "claude"
AGENT_DISPLAY = "Claude"
AGENT_COLOR = "#DE7356"

LOGO_BITMAP = (
    "AAAAAAAAAAYAAAAAAA+AGAAAAA+APAAAAA/APAAAAA/AOAAAAAfAOAcAAAfgOA8AAAPgOB+"
    "AAAPweB8AB4HweD8AB8D4cH4AB+D4cP4AB/B8cfwAAfx8c/gAAP4+c/gAAD8eZ/AAAB/ef"
    "+AAAAf//8AAAAP//8AMAAD//4H+AAB////8AAA////wf/8f//4AP////+AAAAD//wAAAAA"
    "f///AAAA////8AAD//w/+AAP3/4D+AAfO/8AIAB8c/+AAAH4c3vAAAPg5z3gAA/BxzxwAA"
    "8Dhx44AAQHhg8cAAAHBg8OAAAODgeDAAAcDgPBAAA4DgPAAAA4DgHAAABwDgDAAAAAHgAA"
    "AAAAHgAAAAAADgAAAAAADAAAAAAAAAAAA"
)

DEFAULT_USAGE = {
    "session_pct": 0.0,
    "weekly_pct": 0.0,
    "session_reset": "--",
    "weekly_reset": "--",
}

# Status / state values that mean the session is actively doing work or
# blocked on the user — used for wake / activity heuristics on the screen.
ACTIVE_STATES = frozenset({"working", "blocked"})
ACTIVE_STATUSES = frozenset({"busy", "waiting"})


# ── Shared state ─────────────────────────────────────────────────────────────

_shutdown = threading.Event()
_lock = threading.RLock()
_agent_sessions: list[dict[str, Any]] = []
_usage: dict[str, Any] = dict(DEFAULT_USAGE)
_clients: set = set()
_ws_loop: asyncio.AbstractEventLoop | None = None
_secret = ""
_verbose = False
_creds_warned = False
_claude_bin = "claude"


def log(msg: str) -> None:
    print(msg, flush=True)


def vlog(msg: str) -> None:
    if _verbose:
        print(msg, flush=True)


# ── Time / usage helpers ─────────────────────────────────────────────────────

def format_countdown(diff_secs: int) -> str:
    if diff_secs <= 0:
        return "--"
    days = diff_secs // 86400
    hours = (diff_secs % 86400) // 3600
    mins = (diff_secs % 3600) // 60
    if days > 0:
        return f"{days}d {hours}h"
    if hours > 0:
        return f"{hours}h {mins}m"
    return f"{mins}m"


def format_iso_countdown(iso_ts: str) -> str:
    if not iso_ts:
        return "--"
    try:
        target = datetime.fromisoformat(iso_ts.replace("Z", "+00:00"))
        diff = int((target - datetime.now(timezone.utc)).total_seconds())
        return format_countdown(diff)
    except Exception:
        return "--"


def _creds_from_file() -> dict | None:
    try:
        with open(CLAUDE_CREDS) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except FileNotFoundError:
        return None
    except Exception as e:
        vlog(f"[usage] credentials file: {e}")
        return None


def _creds_from_keychain() -> dict | None:
    if sys.platform != "darwin":
        return None
    try:
        raw = subprocess.check_output(
            ["security", "find-generic-password",
             "-s", CLAUDE_KEYCHAIN_SERVICE, "-w"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        if not raw:
            return None
        data = json.loads(raw)
        return data if isinstance(data, dict) else None
    except Exception as e:
        vlog(f"[usage] keychain: {e}")
        return None


def load_claude_access_token() -> str | None:
    global _creds_warned
    for loader in (_creds_from_file, _creds_from_keychain):
        creds = loader()
        if not creds:
            continue
        try:
            token = creds["claudeAiOauth"]["accessToken"]
            if token:
                return str(token)
        except (KeyError, TypeError):
            continue
    if not _creds_warned:
        _creds_warned = True
        log("[usage] no Claude OAuth credentials found "
            f"(tried {CLAUDE_CREDS} and keychain "
            f"'{CLAUDE_KEYCHAIN_SERVICE}') — usage bars will stay empty")
    return None


def fetch_claude_usage() -> dict[str, Any] | None:
    token = load_claude_access_token()
    if not token:
        return None

    req = urllib.request.Request(
        USAGE_API,
        headers={
            "Authorization": f"Bearer {token}",
            "User-Agent": "claude-code/1.0.0",
            "Accept": "application/json",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            data = json.loads(r.read())
    except urllib.error.HTTPError as e:
        log(f"[usage] HTTP {e.code}: {e.reason}")
        return None
    except Exception as e:
        log(f"[usage] {e}")
        return None

    session = data.get("five_hour") or {}
    weekly = data.get("seven_day") or {}
    return {
        "session_pct": float(session.get("utilization") or 0.0) / 100.0,
        "weekly_pct": float(weekly.get("utilization") or 0.0) / 100.0,
        "session_reset": format_iso_countdown(session.get("resets_at", "")),
        "weekly_reset": format_iso_countdown(weekly.get("resets_at", "")),
    }


# ── Agents poll (`claude agents --json`) ─────────────────────────────────────

def _cwd_basename(cwd: str) -> str:
    if not cwd:
        return ""
    return os.path.basename(os.path.normpath(cwd)) or cwd


def _truncate(s: str, n: int) -> str:
    s = str(s or "")
    if len(s) <= n:
        return s
    if n <= 1:
        return s[:n]
    return s[: n - 1] + "…"


def normalize_session(raw: dict[str, Any]) -> dict[str, Any] | None:
    """Compact one `claude agents --json` entry for the screen."""
    if not isinstance(raw, dict):
        return None
    kind = str(raw.get("kind") or "")
    name = str(raw.get("name") or "") or _cwd_basename(str(raw.get("cwd") or ""))
    out: dict[str, Any] = {
        "cwd": _truncate(_cwd_basename(str(raw.get("cwd") or "")), 40),
        "kind": _truncate(kind, 16),
        "name": _truncate(name, 40),
        "startedAt": int(raw.get("startedAt") or 0),
    }
    if raw.get("id"):
        out["id"] = _truncate(str(raw["id"]), 16)
    if raw.get("sessionId"):
        out["sessionId"] = _truncate(str(raw["sessionId"]), 40)
    if raw.get("state"):
        out["state"] = _truncate(str(raw["state"]), 24)
    if raw.get("status"):
        out["status"] = _truncate(str(raw["status"]), 24)
    if raw.get("waitingFor"):
        out["waitingFor"] = _truncate(str(raw["waitingFor"]), 48)
    if raw.get("pid") is not None:
        try:
            out["pid"] = int(raw["pid"])
        except (TypeError, ValueError):
            pass
    return out


def session_is_active(s: dict[str, Any]) -> bool:
    state = str(s.get("state") or "").lower()
    status = str(s.get("status") or "").lower()
    return state in ACTIVE_STATES or status in ACTIVE_STATUSES


def session_sort_key(s: dict[str, Any]) -> tuple:
    """Active first, then by startedAt descending (newest first)."""
    return (0 if session_is_active(s) else 1, -(int(s.get("startedAt") or 0)))


def fetch_agent_sessions() -> list[dict[str, Any]] | None:
    """Return normalized sessions, or None on hard failure (keep last)."""
    try:
        proc = subprocess.run(
            [_claude_bin, "agents", "--json"],
            capture_output=True,
            text=True,
            timeout=8,
        )
    except FileNotFoundError:
        log(f"[agents] `{_claude_bin}` not found on PATH")
        return []
    except subprocess.TimeoutExpired:
        log("[agents] timed out")
        return None
    except Exception as e:
        log(f"[agents] {e}")
        return None

    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip().splitlines()
        log(f"[agents] exit {proc.returncode}"
            + (f": {err[-1]}" if err else ""))
        return None

    try:
        data = json.loads(proc.stdout or "[]")
    except json.JSONDecodeError as e:
        log(f"[agents] bad JSON: {e}")
        return None
    if not isinstance(data, list):
        log("[agents] expected a JSON array")
        return None

    sessions: list[dict[str, Any]] = []
    for raw in data:
        s = normalize_session(raw)
        if s:
            sessions.append(s)
    sessions.sort(key=session_sort_key)
    return sessions[:MAX_SESSIONS]


def set_agent_sessions(sessions: list[dict[str, Any]]) -> bool:
    """Replace cached sessions. Returns True if the list changed."""
    global _agent_sessions
    with _lock:
        if sessions == _agent_sessions:
            return False
        _agent_sessions = list(sessions)
        return True


def status_snapshot() -> dict[str, Any]:
    with _lock:
        usage = dict(_usage)
        sessions = list(_agent_sessions)
    return {
        **usage,
        "sessions": sessions,
        "agent_id": AGENT_ID,
        "agent_display": AGENT_DISPLAY,
        "weekly_title": f"{AGENT_DISPLAY} Weekly" if "weekly_pct" in usage else "",
        "session_title": f"{AGENT_DISPLAY} Session" if "session_pct" in usage else "",
    }


def client_config() -> dict[str, Any]:
    return {
        "default_agent_id": AGENT_ID,
        "agents": {
            AGENT_ID: {
                "display": AGENT_DISPLAY,
                "color": AGENT_COLOR,
                "logo_bitmap": LOGO_BITMAP,
            }
        },
    }


def _sessions_fingerprint(sessions: list[dict[str, Any]]) -> str:
    parts = []
    for s in sessions:
        parts.append(
            f"{s.get('sessionId') or s.get('id') or s.get('name')}|"
            f"{s.get('state')}|{s.get('status')}|{s.get('waitingFor')}"
        )
    return ";".join(parts)


# ── Broadcast ────────────────────────────────────────────────────────────────

_last_broadcast_key: str = ""
_last_broadcast_mono: float = 0.0
_BROADCAST_MIN_INTERVAL = 0.5


def _status_key(payload: dict[str, Any]) -> str:
    sessions = payload.get("sessions") or []
    return (
        f"{_sessions_fingerprint(sessions)}|"
        f"{payload.get('session_pct')}|{payload.get('weekly_pct')}"
    )


def broadcast_status(*, force: bool = False) -> None:
    global _ws_loop, _last_broadcast_key, _last_broadcast_mono
    payload = status_snapshot()
    key = _status_key(payload)
    now = time.monotonic()
    if not force:
        if key == _last_broadcast_key and (now - _last_broadcast_mono) < 5.0:
            return
        if (now - _last_broadcast_mono) < _BROADCAST_MIN_INTERVAL:
            return
    _last_broadcast_key = key
    _last_broadcast_mono = now
    msg = json.dumps(payload)

    sessions = payload.get("sessions") or []
    active = sum(1 for s in sessions if session_is_active(s))
    log(f"[status] sessions={len(sessions)} active={active} "
        f"session={payload['session_pct']:.0%} weekly={payload['weekly_pct']:.0%}")
    loop = _ws_loop
    if loop is None or not _clients:
        return

    async def _send_all() -> None:
        dead = []
        for c in list(_clients):
            try:
                await c.send(msg)
            except Exception:
                dead.append(c)
        for c in dead:
            _clients.discard(c)

    asyncio.run_coroutine_threadsafe(_send_all(), loop)


# ── Auth ─────────────────────────────────────────────────────────────────────

def auth_hmac(secret: str, nonce: str) -> str:
    return hmac.new(secret.encode(), nonce.encode(), hashlib.sha256).hexdigest()


def valid_auth(data: dict, secret: str, nonce: str) -> bool:
    provided = str(data.get("auth_hmac") or "")
    expected = auth_hmac(secret, nonce)
    return bool(provided) and hmac.compare_digest(provided, expected)


# ── WebSocket server ─────────────────────────────────────────────────────────

async def _authenticate(ws, secret: str) -> bool:
    invalid = False
    try:
        nonce = secrets.token_hex(16)
        await ws.send(json.dumps({"type": "challenge", "nonce": nonce}))
        raw = await asyncio.wait_for(ws.recv(), timeout=5.0)
        data = json.loads(raw)
        if valid_auth(data, secret, nonce):
            return True
        invalid = True
        log(f"[ws] auth failed from {getattr(ws, 'remote_address', '?')}")
    except Exception:
        log("[ws] auth error / timeout")

    if invalid:
        try:
            await ws.send(json.dumps({
                "error": "unauthorized",
                "message": "Wrong password",
            }))
        except Exception:
            pass
        await ws.close(1008, "Unauthorized")
    else:
        await ws.close(1011, "Authentication timeout")
    return False


async def _handle_client(ws, path=None) -> None:  # path unused (websockets <13)
    global _secret
    if _secret and not await _authenticate(ws, _secret):
        return

    _clients.add(ws)
    log(f"[ws] client connected ({len(_clients)} total)")
    try:
        # Do not send immediately after the handshake — the ESP32 websocket
        # client can still be finishing upgrade parsing and will fail the
        # connect if a data frame arrives too early.
        async for raw in ws:
            try:
                message = json.loads(raw)
            except Exception:
                continue
            if message.get("type") != "subscribe":
                continue
            client = str(message.get("client") or "ws")
            utc_offset = int(datetime.now().astimezone().utcoffset().total_seconds())
            cfg = {
                "type": "config",
                "utc_offset": utc_offset,
                "remote_control": False,
                **client_config(),
            }
            await ws.send(json.dumps(cfg))
            await ws.send(json.dumps(status_snapshot()))
            log(f"[ws] subscribed as {client}")
    except Exception:
        pass
    finally:
        _clients.discard(ws)
        log(f"[ws] client disconnected ({len(_clients)} remaining)")


async def _ws_main(port: int) -> None:
    global _ws_loop
    ws_serve = _import_websockets()
    _ws_loop = asyncio.get_running_loop()
    async with ws_serve(
        _handle_client, "0.0.0.0", port,
        compression=None, ping_interval=20, ping_timeout=20,
    ):
        log(f"[ws] listening on :{port}")
        while not _shutdown.is_set():
            await asyncio.sleep(1)


def ws_thread(port: int) -> None:
    asyncio.run(_ws_main(port))


# ── mDNS ─────────────────────────────────────────────────────────────────────

def local_ip() -> str:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        return ip
    except Exception:
        return "127.0.0.1"


def mdns_thread(port: int, name: str) -> None:
    if not HAVE_ZEROCONF:
        log("[mdns] zeroconf not installed — skipping (pip install zeroconf)")
        return

    zc = None
    info = None
    current_ip: str | None = None
    while not _shutdown.is_set():
        ip = local_ip()
        if ip.startswith("127."):
            if current_ip is not None:
                try:
                    if info and zc:
                        zc.unregister_service(info)
                    if zc:
                        zc.close()
                except Exception:
                    pass
                zc = info = None
                current_ip = None
                log("[mdns] network lost…")
            _shutdown.wait(5)
            continue

        if ip != current_ip:
            try:
                if info and zc:
                    zc.unregister_service(info)
                if zc:
                    zc.close()
            except Exception:
                pass
            try:
                zc = Zeroconf(interfaces=[ip])
                info = ServiceInfo(
                    "_claudius._tcp.local.",
                    f"{name}._claudius._tcp.local.",
                    addresses=[socket.inet_aton(ip)],
                    port=port,
                    properties={},
                    server=f"{name}.local.",
                )
                zc.register_service(info)
                current_ip = ip
                log(f"[mdns] advertising {name} on {ip}:{port}")
            except Exception as e:
                log(f"[mdns] failed: {e}")
                zc = info = None
                _shutdown.wait(5)
                continue

        _shutdown.wait(10)

    try:
        if info and zc:
            zc.unregister_service(info)
        if zc:
            zc.close()
    except Exception:
        pass


# ── Pollers ──────────────────────────────────────────────────────────────────

def agents_thread() -> None:
    while not _shutdown.is_set():
        sessions = fetch_agent_sessions()
        if sessions is not None and set_agent_sessions(sessions):
            broadcast_status()
        _shutdown.wait(AGENTS_INTERVAL)


def usage_thread() -> None:
    while not _shutdown.is_set():
        usage = fetch_claude_usage()
        if usage:
            with _lock:
                _usage.update(usage)
            broadcast_status()
        _shutdown.wait(USAGE_INTERVAL)


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    global _secret, _verbose, _claude_bin

    parser = argparse.ArgumentParser(
        description="Claude-only companion for the GeekMagic-S3 claudius screen")
    parser.add_argument("--name", required=True,
                        help="mDNS instance name (e.g. my-laptop)")
    parser.add_argument("--secret", default="", help="optional shared HMAC secret")
    parser.add_argument("--port", type=int, default=WS_PORT_DEFAULT,
                        help=f"WebSocket port (default {WS_PORT_DEFAULT})")
    parser.add_argument("--claude", default="claude",
                        help="claude CLI binary (default: claude)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    _secret = args.secret or ""
    _verbose = args.verbose
    _claude_bin = args.claude
    os.makedirs(CONFIG_HOME, exist_ok=True)

    log(f"claudius  [ws://0.0.0.0:{args.port}]  name={args.name}"
        + ("  (secret set)" if _secret else ""))
    log("polling `claude agents --json` — Ctrl-C to stop")

    threading.Thread(target=agents_thread, daemon=True).start()
    threading.Thread(target=usage_thread, daemon=True).start()
    threading.Thread(target=ws_thread, args=(args.port,), daemon=True).start()
    threading.Thread(target=mdns_thread, args=(args.port, args.name),
                     daemon=True).start()

    def _boot() -> None:
        _shutdown.wait(1)
        if _shutdown.is_set():
            return
        sessions = fetch_agent_sessions()
        if sessions is not None:
            set_agent_sessions(sessions)
        usage = fetch_claude_usage()
        if usage:
            with _lock:
                _usage.update(usage)
        broadcast_status(force=True)

    threading.Thread(target=_boot, daemon=True).start()

    signal.signal(signal.SIGTERM, lambda *_: (_shutdown.set(), sys.exit(0)))
    try:
        while not _shutdown.is_set():
            _shutdown.wait(1.0)
    except KeyboardInterrupt:
        _shutdown.set()
        log("bye")


if __name__ == "__main__":
    main()
