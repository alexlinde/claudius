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
import traceback
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
    """Import in main() (before any thread) so a miss exits with a pip hint."""
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

OAUTH_EXPIRY_SKEW = 300      # treat a token expiring within 5 min as stale
KEYCHAIN_BACKOFF = 900       # skip the keychain for 15 min after a failure
AUTH_FAIL_LIMIT = 3          # consecutive 401s before the bars are hidden

# Firmware buffer budgets (status.h sizes minus the NUL) — these are BYTES.
LEN_NAME = 39
LEN_KIND = 15
LEN_ID = 15
LEN_SESSION_ID = 39
LEN_STATE = 23
LEN_WAITING = 47
LEN_CWD = 39

# Status / state values that mean the session is actively doing work or
# blocked on the user — used for wake / activity heuristics on the screen.
ACTIVE_STATES = frozenset({"working", "blocked"})
ACTIVE_STATUSES = frozenset({"busy", "waiting"})


# ── Shared state ─────────────────────────────────────────────────────────────

_shutdown = threading.Event()
_lock = threading.RLock()
_agent_sessions: list[dict[str, Any]] = []
# Raw usage: pcts + resets_at ISO strings. Countdowns are rendered at snapshot
# time so they stay current between the 60s fetches.
_usage: dict[str, Any] = {}
_usage_loaded = False
_clients: set = set()
_subscribed: set = set()
_ws_loop: asyncio.AbstractEventLoop | None = None
_ws_serve = None
_secret = ""
_verbose = False
_creds_warned = False
_expiry_warned = False
_keychain_blocked_until = 0.0
_keychain_warned = False
_auth_failures = 0
_auth_warned = False
_failed_threads: set[str] = set()
_ws_ready = threading.Event()
_claude_bin = "claude"


def log(msg: str) -> None:
    print(msg, flush=True)


def vlog(msg: str) -> None:
    if _verbose:
        print(msg, flush=True)


# ── Thread supervision ───────────────────────────────────────────────────────

def _thread_body(name: str, fn, args: tuple, critical: bool) -> None:
    """Run a thread target; a critical thread dying takes the process down."""
    try:
        fn(*args)
    except Exception:
        log(f"[fatal] {name} thread died:\n{traceback.format_exc().rstrip()}")
        if critical:
            with _lock:
                _failed_threads.add(name)
            _shutdown.set()


def _spawn(name: str, fn, *args, critical: bool = True) -> threading.Thread:
    t = threading.Thread(target=_thread_body, args=(name, fn, args, critical),
                         name=name, daemon=True)
    t.start()
    return t


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
    """Read Claude Code's keychain item. A Keychain ACL prompt would block the
    usage thread forever, so time out and then back off instead of re-prompting
    every cycle."""
    global _keychain_blocked_until, _keychain_warned
    if sys.platform != "darwin":
        return None
    if time.monotonic() < _keychain_blocked_until:
        return None
    try:
        raw = subprocess.check_output(
            ["security", "find-generic-password",
             "-s", CLAUDE_KEYCHAIN_SERVICE, "-w"],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=5,
        ).strip()
        if not raw:
            return None
        data = json.loads(raw)
        _keychain_warned = False
        return data if isinstance(data, dict) else None
    except Exception as e:
        _keychain_blocked_until = time.monotonic() + KEYCHAIN_BACKOFF
        if not _keychain_warned:
            _keychain_warned = True
            log(f"[usage] keychain unavailable ({e}) — "
                f"retrying in {KEYCHAIN_BACKOFF // 60}m")
        return None


def _token_expired(oauth: dict) -> bool:
    """claudeAiOauth.expiresAt is epoch ms; Claude Code owns the refresh."""
    try:
        expires_ms = float(oauth.get("expiresAt") or 0)
    except (TypeError, ValueError):
        return False
    if expires_ms <= 0:
        return False
    return (expires_ms / 1000.0) - time.time() <= OAUTH_EXPIRY_SKEW


def load_claude_access_token() -> str | None:
    global _creds_warned, _expiry_warned
    expired = False
    for loader in (_creds_from_file, _creds_from_keychain):
        creds = loader()
        if not isinstance(creds, dict):
            continue
        oauth = creds.get("claudeAiOauth")
        if not isinstance(oauth, dict):
            continue
        token = str(oauth.get("accessToken") or "")
        if not token:
            continue
        if _token_expired(oauth):
            expired = True
            continue
        _creds_warned = _expiry_warned = False
        return token
    if expired:
        if not _expiry_warned:
            _expiry_warned = True
            log("[usage] Claude OAuth token expired — waiting for Claude Code "
                "to refresh it")
        return None
    if not _creds_warned:
        _creds_warned = True
        log("[usage] no Claude OAuth credentials found "
            f"(tried {CLAUDE_CREDS} and keychain "
            f"'{CLAUDE_KEYCHAIN_SERVICE}') — usage bars will stay empty")
    return None


def _note_unauthorized() -> None:
    """Stale token: Claude Code refreshes it, we just stop trusting our copy."""
    global _auth_failures, _auth_warned
    _auth_failures += 1
    vlog(f"[usage] HTTP 401 (x{_auth_failures})")
    if _auth_failures == AUTH_FAIL_LIMIT and not _auth_warned:
        _auth_warned = True
        log("[usage] repeated HTTP 401 — hiding usage bars until the "
            "credentials refresh")


def set_usage(usage: dict[str, Any]) -> None:
    global _usage_loaded
    with _lock:
        _usage.clear()
        _usage.update(usage)
        _usage_loaded = True


def drop_usage_if_unauthorized() -> bool:
    """Hide the bars after repeated 401s rather than freezing stale numbers."""
    global _usage_loaded
    if _auth_failures < AUTH_FAIL_LIMIT:
        return False
    with _lock:
        if not _usage_loaded:
            return False
        _usage.clear()
        _usage_loaded = False
        return True


def fetch_claude_usage() -> dict[str, Any] | None:
    global _auth_failures, _auth_warned
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
        # Shape handling stays inside the try: a non-dict body or a weird
        # utilization must not kill the poll loop.
        if not isinstance(data, dict):
            raise ValueError("response is not a JSON object")
        session = data.get("five_hour")
        weekly = data.get("seven_day")
        session = session if isinstance(session, dict) else {}
        weekly = weekly if isinstance(weekly, dict) else {}
        usage = {
            "session_pct": float(session.get("utilization") or 0.0) / 100.0,
            "weekly_pct": float(weekly.get("utilization") or 0.0) / 100.0,
            "session_resets_at": str(session.get("resets_at") or ""),
            "weekly_resets_at": str(weekly.get("resets_at") or ""),
        }
    except urllib.error.HTTPError as e:
        if e.code == 401:
            _note_unauthorized()
        else:
            log(f"[usage] HTTP {e.code}: {e.reason}")
        return None
    except Exception as e:
        log(f"[usage] {e}")
        return None

    _auth_failures = 0
    _auth_warned = False
    return usage


# ── Agents poll (`claude agents --json`) ─────────────────────────────────────

def _cwd_basename(cwd: str) -> str:
    if not cwd:
        return ""
    return os.path.basename(os.path.normpath(cwd)) or cwd


def _truncate(s: str, limit: int) -> str:
    """Fit into `limit` UTF-8 BYTES on a codepoint boundary — the firmware
    copies these into fixed char[] buffers (status.h) and would otherwise
    split a multi-byte glyph."""
    s = str(s or "")
    raw = s.encode("utf-8")
    if len(raw) <= limit:
        return s
    if limit <= 0:
        return ""
    ell = b"\xe2\x80\xa6"  # "…" — 3 bytes
    if limit < len(ell):
        return raw[:limit].decode("utf-8", "ignore")
    return raw[: limit - len(ell)].decode("utf-8", "ignore") + "…"


def normalize_session(raw: dict[str, Any]) -> dict[str, Any] | None:
    """Compact one `claude agents --json` entry for the screen."""
    if not isinstance(raw, dict):
        return None
    kind = str(raw.get("kind") or "")
    name = str(raw.get("name") or "") or _cwd_basename(str(raw.get("cwd") or ""))
    try:
        started = int(raw.get("startedAt") or 0)
    except (TypeError, ValueError):
        started = 0
    out: dict[str, Any] = {
        "cwd": _truncate(_cwd_basename(str(raw.get("cwd") or "")), LEN_CWD),
        "kind": _truncate(kind, LEN_KIND),
        "name": _truncate(name, LEN_NAME),
        "startedAt": started,
    }
    if raw.get("id"):
        out["id"] = _truncate(str(raw["id"]), LEN_ID)
    if raw.get("sessionId"):
        out["sessionId"] = _truncate(str(raw["sessionId"]), LEN_SESSION_ID)
    if raw.get("state"):
        out["state"] = _truncate(str(raw["state"]), LEN_STATE)
    if raw.get("status"):
        out["status"] = _truncate(str(raw["status"]), LEN_STATE)
    if raw.get("waitingFor"):
        out["waitingFor"] = _truncate(str(raw["waitingFor"]), LEN_WAITING)
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
        loaded = _usage_loaded
        sessions = list(_agent_sessions)
    snap: dict[str, Any] = {
        "sessions": sessions,
        "agent_id": AGENT_ID,
        "agent_display": AGENT_DISPLAY,
        # Empty titles hide the bars on the screen — no 0% flash before the
        # first successful fetch (and none at all without credentials).
        "weekly_title": "",
        "session_title": "",
    }
    if loaded:
        snap["session_pct"] = usage.get("session_pct", 0.0)
        snap["weekly_pct"] = usage.get("weekly_pct", 0.0)
        # Rendered here, not at fetch time, so the countdowns stay current.
        snap["session_reset"] = format_iso_countdown(
            usage.get("session_resets_at", ""))
        snap["weekly_reset"] = format_iso_countdown(
            usage.get("weekly_resets_at", ""))
        snap["weekly_title"] = f"{AGENT_DISPLAY} Weekly"
        snap["session_title"] = f"{AGENT_DISPLAY} Session"
    return snap


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
_BROADCAST_FLUSH_DELAY = 0.6
_broadcast_lock = threading.RLock()
_flush_timer: threading.Timer | None = None


def _status_key(payload: dict[str, Any]) -> str:
    sessions = payload.get("sessions") or []
    return (
        f"{_sessions_fingerprint(sessions)}|"
        f"{payload.get('session_pct')}|{payload.get('weekly_pct')}"
    )


def _arm_flush() -> None:
    """Trailing flush so a rate-limited change is deferred, never dropped.
    Callers broadcast only on change, so a dropped state is never re-sent.
    Caller holds _broadcast_lock; at most one timer is outstanding."""
    global _flush_timer
    if _flush_timer is not None or _shutdown.is_set():
        return
    t = threading.Timer(_BROADCAST_FLUSH_DELAY, _flush_broadcast)
    t.daemon = True
    _flush_timer = t
    t.start()


def _flush_broadcast() -> None:
    global _flush_timer
    with _broadcast_lock:
        _flush_timer = None
    try:
        broadcast_status()
    except Exception as e:
        log(f"[status] flush error: {e}")


def _cancel_flush() -> None:
    global _flush_timer
    with _broadcast_lock:
        t, _flush_timer = _flush_timer, None
    if t is not None:
        t.cancel()


def broadcast_status(*, force: bool = False) -> None:
    global _last_broadcast_key, _last_broadcast_mono
    payload = status_snapshot()
    key = _status_key(payload)
    now = time.monotonic()
    with _broadcast_lock:
        if not force:
            # Identical state: nothing new to send (re-sent every 5s so the
            # rendered countdowns stay fresh).
            if key == _last_broadcast_key and (now - _last_broadcast_mono) < 5.0:
                return
            if (now - _last_broadcast_mono) < _BROADCAST_MIN_INTERVAL:
                _arm_flush()
                return
        _last_broadcast_key = key
        _last_broadcast_mono = now
    msg = json.dumps(payload)

    sessions = payload.get("sessions") or []
    active = sum(1 for s in sessions if session_is_active(s))
    meters = ("usage=pending" if "session_pct" not in payload else
              f"session={payload['session_pct']:.0%} "
              f"weekly={payload['weekly_pct']:.0%}")
    log(f"[status] sessions={len(sessions)} active={active} {meters}")
    loop = _ws_loop
    if loop is None or not _subscribed:
        return

    async def _send_all() -> None:
        dead = []
        for c in list(_subscribed):
            try:
                await c.send(msg)
            except Exception:
                dead.append(c)
        for c in dead:
            _subscribed.discard(c)
            _clients.discard(c)

    try:
        asyncio.run_coroutine_threadsafe(_send_all(), loop)
    except RuntimeError:  # loop closed between the read and the submit
        pass


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
        # connect if a data frame arrives too early. Broadcasts therefore go
        # only to clients that have subscribed.
        async for raw in ws:
            try:
                message = json.loads(raw)
            except Exception:
                continue
            if not isinstance(message, dict):
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
            _subscribed.add(ws)
            log(f"[ws] subscribed as {client} ({len(_subscribed)} total)")
    except Exception:
        pass
    finally:
        _subscribed.discard(ws)
        _clients.discard(ws)
        log(f"[ws] client disconnected ({len(_subscribed)} subscribed)")


async def _ws_main(port: int) -> None:
    global _ws_loop
    _ws_loop = asyncio.get_running_loop()
    try:
        async with _ws_serve(
            _handle_client, "0.0.0.0", port,
            compression=None, ping_interval=20, ping_timeout=20,
        ):
            log(f"[ws] listening on :{port}")
            _ws_ready.set()
            while not _shutdown.is_set():
                await asyncio.sleep(1)
    except OSError as e:
        log(f"[ws] cannot listen on :{port} — {e}")
        raise
    finally:
        _ws_loop = None


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


def _mdns_teardown(zc, info) -> None:
    """Send the goodbye and close the engine — close() owns threads + sockets,
    so it must run on every path or we leak FDs every retry."""
    try:
        if zc and info:
            zc.unregister_service(info)
    except Exception:
        pass
    try:
        if zc:
            zc.close()
    except Exception:
        pass


def mdns_thread(port: int, name: str) -> None:
    if not HAVE_ZEROCONF:
        log("[mdns] zeroconf not installed — skipping (pip install zeroconf)")
        return

    # Never advertise before the server is listening: a record pointing at a
    # port we do not own just makes screens dial a host that will not serve.
    while not _ws_ready.is_set() and not _shutdown.is_set():
        _ws_ready.wait(0.5)
    if _shutdown.is_set():
        return

    zc = None
    info = None
    current_ip: str | None = None
    while not _shutdown.is_set():
        ip = local_ip()
        if ip.startswith("127."):
            if current_ip is not None:
                _mdns_teardown(zc, info)
                zc = info = None
                current_ip = None
                log("[mdns] network lost…")
            _shutdown.wait(5)
            continue

        if ip != current_ip:
            _mdns_teardown(zc, info)
            zc = info = None
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
                _mdns_teardown(zc, info)
                zc = info = None
                _shutdown.wait(5)
                continue

        _shutdown.wait(10)

    # Goodbye on the way out — stale PTR records (75 min TTL) keep screens
    # dialing a dead host, so main() joins this thread before exiting.
    _mdns_teardown(zc, info)
    if current_ip is not None:
        log("[mdns] unregistered")


# ── Pollers ──────────────────────────────────────────────────────────────────

def agents_thread() -> None:
    while not _shutdown.is_set():
        # Per-iteration guard: one bad poll must not end the loop (the thread
        # supervisor is only the outer backstop).
        try:
            sessions = fetch_agent_sessions()
            if sessions is not None and set_agent_sessions(sessions):
                broadcast_status()
        except Exception as e:
            log(f"[agents] poll error: {e}")
            vlog(traceback.format_exc().rstrip())
        _shutdown.wait(AGENTS_INTERVAL)


def usage_thread() -> None:
    while not _shutdown.is_set():
        try:
            usage = fetch_claude_usage()
            if usage is not None:
                set_usage(usage)
                broadcast_status()
            elif drop_usage_if_unauthorized():
                broadcast_status(force=True)
        except Exception as e:
            log(f"[usage] poll error: {e}")
            vlog(traceback.format_exc().rstrip())
        _shutdown.wait(USAGE_INTERVAL)


# ── Main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    global _secret, _verbose, _claude_bin, _ws_serve

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
    # Import before any thread starts: the sys.exit(1) hint has to reach the
    # shell, and a websockets miss must not leave a healthy-looking daemon.
    _ws_serve = _import_websockets()
    os.makedirs(CONFIG_HOME, exist_ok=True)

    log(f"claudius  [ws://0.0.0.0:{args.port}]  name={args.name}"
        + ("  (secret set)" if _secret else ""))
    log("polling `claude agents --json` — Ctrl-C to stop")

    _spawn("agents", agents_thread)
    _spawn("usage", usage_thread)
    _spawn("ws", ws_thread, args.port)
    mdns = _spawn("mdns", mdns_thread, args.port, args.name)

    def _boot() -> None:
        _shutdown.wait(1)
        if _shutdown.is_set():
            return
        sessions = fetch_agent_sessions()
        if sessions is not None:
            set_agent_sessions(sessions)
        usage = fetch_claude_usage()
        if usage is not None:
            set_usage(usage)
        broadcast_status(force=True)

    _spawn("boot", _boot, critical=False)

    # Only flag shutdown — sys.exit(0) here would skip the mDNS goodbye.
    signal.signal(signal.SIGTERM, lambda *_: _shutdown.set())
    try:
        while not _shutdown.is_set():
            _shutdown.wait(1.0)
    except KeyboardInterrupt:
        _shutdown.set()

    _cancel_flush()
    mdns.join(timeout=3.0)
    with _lock:
        failed = sorted(_failed_threads)
    if failed:
        log(f"exiting: {', '.join(failed)} thread(s) failed")
        sys.exit(1)
    log("bye")


if __name__ == "__main__":
    main()
