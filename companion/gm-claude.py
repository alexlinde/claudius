#!/usr/bin/env python3
"""
gm-claude.py — minimal Claude Code companion for the GeekMagic-S3 screen.

Talks the same WebSocket protocol as henrikekblad/codelight's screen client:
  mDNS `_claudius._tcp` → ws://host:8765 → challenge/HMAC → subscribe →
  config + untyped status frames (working/waiting/idle + usage bars).

Claude-only. No remote control, multi-agent, D-Bus, or conversation feed.

Usage:
    pip install websockets zeroconf
    python3 companion/gm-claude.py --name my-laptop
    python3 companion/gm-claude.py --name my-laptop --secret mypassword
    python3 companion/gm-claude.py --uninstall
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import secrets
import shlex
import signal
import socket
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
    """Lazy — hook invocations must not require websockets installed."""
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
SOCKET_PATH = os.path.join(CONFIG_HOME, "gm-claude.sock")
MONITOR_DIR = os.path.join(CONFIG_HOME, "monitor_state")
CLAUDE_SETTINGS = os.path.expanduser("~/.claude/settings.json")
CLAUDE_CREDS = os.path.expanduser("~/.claude/.credentials.json")
# macOS Claude Code stores OAuth in the login keychain (not a file).
CLAUDE_KEYCHAIN_SERVICE = "Claude Code-credentials"
USAGE_API = "https://claude.ai/api/oauth/usage"
WS_PORT_DEFAULT = 8765
USAGE_INTERVAL = 60
IDLE_WINDOW = 600          # drop silent "working" sessions
IDLE_WINDOW_WAITING = 30   # drop silent "waiting" sessions
AGENT_ID = "claude"
AGENT_DISPLAY = "Claude"
AGENT_COLOR = "#DE7356"

# 48×48 1-bit Claude logo (same bitmap layout as upstream codelight screens).
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


# ── Shared state ─────────────────────────────────────────────────────────────

_shutdown = threading.Event()
_lock = threading.RLock()
_sessions: dict[str, dict[str, Any]] = {}
_usage: dict[str, Any] = dict(DEFAULT_USAGE)
_clients: set = set()
_ws_loop: asyncio.AbstractEventLoop | None = None
_secret = ""
_verbose = False
_creds_warned = False


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
    """Claude Code on macOS stores OAuth JSON in the login keychain."""
    if sys.platform != "darwin":
        return None
    try:
        import subprocess
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


# ── Session / status ─────────────────────────────────────────────────────────

def update_session(session_id: str, state: str) -> None:
    with _lock:
        if state == "ended":
            _sessions.pop(session_id, None)
        else:
            _sessions[session_id] = {"state": state, "time": time.time()}


def prune_sessions() -> None:
    now = time.time()
    with _lock:
        dead = []
        for sid, info in _sessions.items():
            window = IDLE_WINDOW_WAITING if info["state"] == "waiting" else IDLE_WINDOW
            if now - info["time"] > window:
                dead.append(sid)
        for sid in dead:
            del _sessions[sid]
            vlog(f"[state] pruned idle session {sid}")


def overall_status() -> tuple[int, str]:
    prune_sessions()
    with _lock:
        if not _sessions:
            return 0, "idle"
        states = [s["state"] for s in _sessions.values()]
        if "working" in states:
            return len(_sessions), "working"
        if "waiting" in states:
            return len(_sessions), "waiting"
        return len(_sessions), "idle"


def status_snapshot() -> dict[str, Any]:
    sessions, status = overall_status()
    with _lock:
        usage = dict(_usage)
    return {
        **usage,
        "sessions": sessions,
        "status": status,
        "agent_id": AGENT_ID,
        "agent_display": AGENT_DISPLAY,
        "weekly_title": f"{AGENT_DISPLAY} Weekly" if "weekly_pct" in usage else "",
        "session_title": f"{AGENT_DISPLAY} Session" if "session_pct" in usage else "",
        "per_agent_status": {AGENT_ID: status},
        "last_active_agent": AGENT_ID,
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


# ── Broadcast ────────────────────────────────────────────────────────────────

_last_broadcast_key: str = ""
_last_broadcast_mono: float = 0.0
_BROADCAST_MIN_INTERVAL = 1.0  # seconds — ESP client can't absorb a flood


def _status_key(payload: dict[str, Any]) -> str:
    """Stable key ignoring countdown strings that change every second."""
    return (
        f"{payload.get('status')}|{payload.get('sessions')}|"
        f"{payload.get('session_pct')}|{payload.get('weekly_pct')}"
    )


def broadcast_status() -> None:
    global _ws_loop, _last_broadcast_key, _last_broadcast_mono
    payload = status_snapshot()
    key = _status_key(payload)
    now = time.monotonic()
    if key == _last_broadcast_key and (now - _last_broadcast_mono) < 5.0:
        return
    if (now - _last_broadcast_mono) < _BROADCAST_MIN_INTERVAL:
        return
    _last_broadcast_key = key
    _last_broadcast_mono = now
    msg = json.dumps(payload)

    log(f"[status] {payload.get('status')} sessions={payload['sessions']} "
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
    last_status = "idle"
    # compression=None: ESP32 websocket client mishandles permessage-deflate.
    # Keep server pings so half-open ESP sockets get pruned.
    async with ws_serve(
        _handle_client, "0.0.0.0", port,
        compression=None, ping_interval=20, ping_timeout=20,
    ):
        log(f"[ws] listening on :{port}")
        while not _shutdown.is_set():
            await asyncio.sleep(2)
            _, status = overall_status()
            if status != last_status:
                last_status = status
                if _clients:
                    msg = json.dumps(status_snapshot())
                    log(f"[ws] timeout → {status}")
                    await asyncio.gather(
                        *[c.send(msg) for c in list(_clients)],
                        return_exceptions=True,
                    )


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
                # Explicit server= so A records use name.local, not the full
                # service name (…_claudius._tcp.local) which ESP-IDF often
                # fails to attach as an address on PTR results.
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


# ── Unix socket (hooks) ──────────────────────────────────────────────────────

def handle_hook_message(msg: dict) -> None:
    state = str(msg.get("state") or "")
    session_id = str(msg.get("session_id") or "unknown")
    if state not in ("working", "waiting", "ended"):
        return
    update_session(session_id, state)
    vlog(f"[hook] {state} session={session_id}")
    broadcast_status()


def socket_thread() -> None:
    os.makedirs(os.path.dirname(SOCKET_PATH), exist_ok=True)
    try:
        os.unlink(SOCKET_PATH)
    except FileNotFoundError:
        pass

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    server.listen(32)
    server.settimeout(1.0)
    log(f"[socket] {SOCKET_PATH}")

    try:
        while not _shutdown.is_set():
            try:
                conn, _ = server.accept()
            except socket.timeout:
                continue
            try:
                conn.settimeout(2.0)
                raw = b""
                while b"\n" not in raw and len(raw) < 8192:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    raw += chunk
                if raw.strip():
                    data = json.loads(raw.split(b"\n", 1)[0].decode())
                    if isinstance(data, dict):
                        handle_hook_message(data)
            except Exception as e:
                vlog(f"[socket] {e}")
            finally:
                conn.close()
    finally:
        server.close()
        try:
            os.unlink(SOCKET_PATH)
        except FileNotFoundError:
            pass


def send_hook_event(state: str, session_id: str, hook_event: str = "") -> bool:
    payload = {
        "state": state,
        "session_id": session_id,
        "agent_id": AGENT_ID,
        "hook_event": hook_event,
    }
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.5)
        sock.connect(SOCKET_PATH)
        sock.sendall(json.dumps(payload).encode())
        sock.close()
        return True
    except Exception:
        return False


def write_monitor_fallback(session_id: str, state: str) -> None:
    os.makedirs(MONITOR_DIR, exist_ok=True)
    path = os.path.join(MONITOR_DIR, f"{session_id}.json")
    if state == "ended":
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        return
    with open(path, "w") as f:
        json.dump({"state": state, "time": time.time(),
                   "session_id": session_id, "agent_id": AGENT_ID}, f)


def run_hook(state: str) -> None:
    """Invoked by Claude Code hooks: read stdin JSON, notify daemon."""
    try:
        data = json.loads(sys.stdin.read() or "{}")
        if not isinstance(data, dict):
            data = {}
    except Exception:
        data = {}
    session_id = str(
        data.get("session_id")
        or data.get("sessionId")
        or data.get("session")
        or "unknown"
    )
    hook_event = str(
        data.get("hook_event_name")
        or data.get("hookEventName")
        or ""
    )
    if not send_hook_event(state, session_id, hook_event):
        write_monitor_fallback(session_id, state)


# ── Usage poller ─────────────────────────────────────────────────────────────

def usage_thread() -> None:
    while not _shutdown.is_set():
        usage = fetch_claude_usage()
        if usage:
            with _lock:
                _usage.update(usage)
            # Refresh countdown strings on every push.
            broadcast_status()
        _shutdown.wait(USAGE_INTERVAL)


# ── Claude Code hook install ─────────────────────────────────────────────────

def self_path() -> str:
    return os.path.abspath(os.path.realpath(__file__))


def hook_cmd(state: str) -> str:
    interp = sys.executable
    return f"{shlex.quote(interp)} {shlex.quote(self_path())} --hook {state}"


def is_ours(cmd: str) -> bool:
    return "gm-claude" in cmd and "--hook" in cmd


def _strip_our_hooks(hooks: dict) -> None:
    for event in list(hooks.keys()):
        entries = hooks.get(event, [])
        if not isinstance(entries, list):
            continue
        cleaned = []
        for entry in entries:
            if not isinstance(entry, dict):
                cleaned.append(entry)
                continue
            inner = entry.get("hooks")
            if not isinstance(inner, list):
                if is_ours(str(entry.get("command") or "")):
                    continue
                cleaned.append(entry)
                continue
            kept = [h for h in inner
                    if not (isinstance(h, dict) and is_ours(h.get("command", "")))]
            if kept:
                cleaned.append({**entry, "hooks": kept})
        if cleaned:
            hooks[event] = cleaned
        else:
            del hooks[event]


def install_hooks() -> None:
    path = CLAUDE_SETTINGS
    try:
        with open(path) as f:
            doc = json.load(f)
        if not isinstance(doc, dict):
            doc = {}
    except FileNotFoundError:
        doc = {}
    except Exception as e:
        log(f"[hooks] could not read {path}: {e}")
        return

    hooks = doc.get("hooks")
    if not isinstance(hooks, dict):
        hooks = {}

    _strip_our_hooks(hooks)

    def entry(cmd: str) -> dict:
        return {"matcher": "", "hooks": [{"type": "command", "command": cmd}]}

    desired = {
        "PreToolUse":       entry(hook_cmd("working")),
        "PostToolUse":      entry(hook_cmd("working")),
        "UserPromptSubmit": entry(hook_cmd("working")),
        "PermissionRequest": entry(hook_cmd("waiting")),
        "PermissionDenied": entry(hook_cmd("working")),
        "Stop":             entry(hook_cmd("ended")),
        "SessionEnd":       entry(hook_cmd("ended")),
    }
    for event, slot in desired.items():
        hooks.setdefault(event, [])
        if not isinstance(hooks[event], list):
            hooks[event] = []
        hooks[event].append(slot)

    doc["hooks"] = hooks
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.tmp.{os.getpid()}"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)
    log(f"[hooks] installed in {path}")


def uninstall_hooks() -> None:
    path = CLAUDE_SETTINGS
    try:
        with open(path) as f:
            doc = json.load(f)
    except FileNotFoundError:
        log("[hooks] nothing to uninstall")
        return
    except Exception as e:
        log(f"[hooks] could not read {path}: {e}")
        return

    hooks = doc.get("hooks")
    if not isinstance(hooks, dict):
        log("[hooks] nothing to uninstall")
        return

    _strip_our_hooks(hooks)
    doc["hooks"] = hooks
    tmp = f"{path}.tmp.{os.getpid()}"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)
    log(f"[hooks] removed from {path}")


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    global _secret, _verbose

    parser = argparse.ArgumentParser(
        description="Claude-only companion for the GeekMagic-S3 claudius screen")
    parser.add_argument("--name", help="mDNS instance name (e.g. my-laptop)")
    parser.add_argument("--secret", default="", help="optional shared HMAC secret")
    parser.add_argument("--port", type=int, default=WS_PORT_DEFAULT,
                        help=f"WebSocket port (default {WS_PORT_DEFAULT})")
    parser.add_argument("--hook", choices=("working", "waiting", "ended"),
                        help=argparse.SUPPRESS)
    parser.add_argument("--agent", default=AGENT_ID, help=argparse.SUPPRESS)
    parser.add_argument("--uninstall", action="store_true",
                        help="remove Claude Code hooks and exit")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.uninstall:
        uninstall_hooks()
        return

    if args.hook:
        run_hook(args.hook)
        return

    if not args.name:
        parser.error("--name is required (e.g. --name my-laptop)")

    _secret = args.secret or ""
    _verbose = args.verbose
    os.makedirs(CONFIG_HOME, exist_ok=True)

    install_hooks()

    log(f"claudius  [ws://0.0.0.0:{args.port}]  name={args.name}"
        + ("  (secret set)" if _secret else ""))
    log("Ctrl-C to stop")

    threading.Thread(target=socket_thread, daemon=True).start()
    threading.Thread(target=usage_thread, daemon=True).start()
    threading.Thread(target=ws_thread, args=(args.port,), daemon=True).start()
    threading.Thread(target=mdns_thread, args=(args.port, args.name),
                     daemon=True).start()

    # Initial usage fetch soon after start.
    def _boot_usage() -> None:
        _shutdown.wait(2)
        if _shutdown.is_set():
            return
        usage = fetch_claude_usage()
        if usage:
            with _lock:
                _usage.update(usage)
            broadcast_status()

    threading.Thread(target=_boot_usage, daemon=True).start()

    signal.signal(signal.SIGTERM, lambda *_: (_shutdown.set(), sys.exit(0)))
    try:
        while not _shutdown.is_set():
            _shutdown.wait(1.0)
    except KeyboardInterrupt:
        _shutdown.set()
        log("bye")


if __name__ == "__main__":
    main()
