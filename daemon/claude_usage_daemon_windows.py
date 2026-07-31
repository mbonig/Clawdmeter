#!/usr/bin/env python3
"""Claude Usage Tracker Daemon — Windows (Phase 2).

Reads the Claude OAuth token from the native-Windows credentials path and
polls the Anthropic API for rate-limit utilization data. BLE glue added in
later plans.
"""

import asyncio
import calendar
import datetime
import html
import json
import logging
import logging.handlers
import os
import random
import re
import signal
import subprocess
import sys
import threading
import time
import unicodedata
from pathlib import Path

import httpx
from bleak import BleakClient
from bleak.backends.device import BLEDevice
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
CONNECT_RETRIES = 3        # D-01: attempts before giving up on a device
CONNECT_RETRY_DELAY = 2.0  # D-01: seconds between failed connect attempts
ZOMBIE_BREAK_LIMIT = 1     # D-03: consecutive write failures before abandoning a half-open link
                           # N=1: breaks at T=60s, leaves ~60s headroom for reconnect+poll inside 120s SLA
                           # N=2 would bust the 120s budget before reconnect even begins
RECONNECT_BACKOFF_CAP = 8  # D-05: fast-reconnect cap (seconds); keeps stacked retries inside 120s SLA
                           # ~5–10s band per CONTEXT.md Claude's Discretion; 8 chosen as middle ground

# Optional reset chime.
# Optional clock display. 
# Config lives under the same Clawdmeter dir as daemon.log.
CONFIG_FILE = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Clawdmeter" / "config"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

# Market quotes for the device's center rotator (opt-in via `tickers` in the
# config) — mirrors the macOS/Linux daemon; see its comments for why Yahoo's
# chart endpoint is used and why prices are formatted host-side.
QUOTE_URL = "https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=1d&interval=1d"
QUOTE_HEADERS = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
QUOTE_TTL = 300
MAX_TICKERS = 3
# Convenience aliases for company names that aren't their ticker (SpaceX listed
# on NasdaqGS as SPCX in June 2026). The device always shows the real symbol
# that was priced, never the alias it was typed as.
TICKER_ALIASES = {"spacex": "SPCX"}
# sym -> (fetched_at, payload dict)
_quote_cache: dict[str, tuple[float, dict]] = {}

# Software quotes scraped from softwarequotes.com — it publishes no RSS feed,
# JSON API or embed, so the page markup is the only interface. The patterns below
# are anchored on the class names wrapping each quote; if the site restyles,
# fetch_quote_pool() logs and returns nothing rather than sending junk.
QOD_URL = "https://softwarequotes.com/quote-of-the-day"
QOD_HOME_URL = "https://softwarequotes.com/"
QOD_TOPICS_URL = "https://softwarequotes.com/topics"
QOD_BLOCK_SPLIT_RE = re.compile(r'<div class="quote[ "]')
QOD_TEXT_RE = re.compile(r'quote__center.*?<p>(.*?)</p>', re.S)
QOD_AUTHOR_RE = re.compile(r'quote__author.*?<a[^>]*>(.*?)</a>', re.S)
QOD_TOPIC_RE = re.compile(r'href="https://softwarequotes\.com/(topic/[^"?#]+)"')
# The device shows a different quote every QOD_ROTATE seconds, which needs a pool
# of them: /quote-of-the-day serves one quote for the whole day, so rotating on
# that page alone would just re-send the same string. The site has no listing or
# random endpoint either, so the pool is assembled from topic pages. Those are a
# long tail — the ~265 topics in /topics mostly hold 1-6 quotes each, while the
# handful the site features on its home page hold 20+ — so the sample is seeded
# with the featured ones and topped up at random for variety. The actual quote of
# the day always leads the pool.
QOD_ROTATE = 300
QOD_POOL_TTL = 6 * 3600
QOD_FEATURED_TOPICS = 10   # from the home page — the densest topic pages
QOD_RANDOM_TOPICS = 6      # from /topics — rotates the mix between rebuilds
# A sanity bound, not a fitting mechanism: the device drops to a smaller font as
# the text grows, so anything up to ~300 characters wraps into its panel intact.
# Quotes are never shortened to fit a BLE write (see Session.write_payload) —
# only a genuinely essay-length one gets cut here, at a word boundary.
QOD_MAX_CHARS = 300
_qod_pool: list[dict] = []   # [{"t": text, "a": author}, ...], quote of the day first
_qod_pool_at = 0.0           # when the pool was built
_qod_index = 0               # which one is currently on the device
_qod_shown_at = 0.0          # when it went out, for the QOD_ROTATE timer

def _build_file_logger() -> logging.Logger | None:
    """Create a rotating file logger for field diagnostics, or None.

    Autostart launches the tray under pythonw.exe, which has no console — stdout
    is discarded (and is in fact None, making print() unsafe). A rotating file is
    then the ONLY trail when the daemon stalls in the field. Windows-only: on the
    Linux dev box / CI the console print() suffices, and gating to win32 keeps the
    pure-helper unit tests from writing stray log files.
    """
    if sys.platform != "win32":
        return None
    logger = logging.getLogger("clawdmeter.daemon")
    if logger.handlers:
        return logger  # idempotent across re-import (tray imports this module)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    path = base / "Clawdmeter" / "daemon.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8"
        )
    except OSError:
        return None  # best-effort — logging setup must never stop the daemon
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


_FILE_LOGGER = _build_file_logger()


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    # Under pythonw sys.stdout is None and print() would raise — guard it so a
    # missing console can never crash the daemon thread (the silent-freeze mode).
    try:
        print(line, flush=True)
    except (OSError, ValueError, AttributeError, RuntimeError):
        pass
    if _FILE_LOGGER is not None:
        _FILE_LOGGER.info(msg)


class AuthError(Exception):
    """Raised by poll_api on a genuine 401/403 — the token really is expired or
    invalid and the user must re-run `claude login`. Distinct from a None return,
    which means a TRANSIENT failure (network/DNS, timeout, rate-limit, 5xx) that
    must NOT be mislabeled as a token problem (SC#5: a boot-time `getaddrinfo
    failed` DNS blip wrongly fired the 'token expired' toast)."""

def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" so the device stays silent until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_config_flag(key: str) -> str:
    """Read a plain on/off option from the config file, defaulting to "off".

    The older `chime`/`clock` readers each inline this parsing because they
    predate having more than one option; new boolean options use this.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                name, val = line.split("=", 1)
                if name.strip().lower() == key:
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" so existing setups keep showing "Usage" until opted in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def read_tickers_setting() -> list[str]:
    """Read the `tickers` option: market symbols for the device's center rotator.

    Comma-separated, aliases resolved, capped at MAX_TICKERS. Empty (default)
    means no quotes are sent.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "tickers":
                    raw = val.strip()
    except OSError:
        return []
    syms: list[str] = []
    for part in raw.split(","):
        sym = part.strip()
        if not sym:
            continue
        sym = TICKER_ALIASES.get(sym.lower(), sym).upper()
        if sym not in syms:
            syms.append(sym)
    return syms[:MAX_TICKERS]


async def fetch_quote(http: httpx.AsyncClient, sym: str) -> dict | None:
    """Fetch one symbol as the device's wire form, or None if it can't be priced."""
    try:
        resp = await http.get(QUOTE_URL.format(sym=sym), headers=QUOTE_HEADERS)
        resp.raise_for_status()
        meta = resp.json()["chart"]["result"][0]["meta"]
    except (httpx.HTTPError, ValueError, KeyError, IndexError, TypeError) as e:
        log(f"Quote fetch failed for {sym}: {e}")
        return None

    price = meta.get("regularMarketPrice")
    if not isinstance(price, (int, float)):
        log(f"Quote for {sym} has no price; skipping")
        return None
    prefix = "$" if meta.get("currency") == "USD" else ""
    quote = {"n": meta.get("symbol") or sym, "p": f"{prefix}{price:,.2f}"}
    prev = meta.get("chartPreviousClose")
    if isinstance(prev, (int, float)) and prev:
        quote["d"] = round((price - prev) / prev * 100.0, 2)
    return quote


async def add_quote_fields(payload: dict) -> None:
    """Add "q":[{n,p,d}, ...] for each configured ticker, cached for QUOTE_TTL.

    A symbol that fails to fetch keeps serving its last good value, and is
    omitted entirely if it never succeeded.
    """
    syms = read_tickers_setting()
    if not syms:
        return
    now = time.time()
    stale = [s for s in syms if now - _quote_cache.get(s, (0.0, {}))[0] >= QUOTE_TTL]
    if stale:
        async with httpx.AsyncClient(timeout=10.0, follow_redirects=True) as http:
            fetched = await asyncio.gather(*(fetch_quote(http, s) for s in stale))
        for sym, quote in zip(stale, fetched):
            if quote is not None:
                _quote_cache[sym] = (now, quote)
    quotes = [_quote_cache[s][1] for s in syms if s in _quote_cache]
    if quotes:
        payload["q"] = quotes


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection on Windows via the registry. Returns 12 or 24."""
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Control Panel\International") as k:
            # iTime: "1" = 24-hour, "0" = 12-hour.
            val, _ = winreg.QueryValueEx(k, "iTime")
            return 24 if str(val).strip() == "1" else 12
    except (ImportError, OSError):
        return 24


def add_zone_offset_fields(payload: dict, now: float) -> None:
    """Add "te"/"tu": minutes to add to the local clock for US Eastern and UTC.

    The device gets a pre-shifted local epoch rather than a real UTC one, so the
    secondary zones cross the wire as deltas from it — every timezone/DST rule
    stays host-side. An offset of 0 (the host is already in that zone) is
    omitted; the firmware would otherwise print the same time twice under two
    different labels.
    """
    local_off = time.localtime(now).tm_gmtoff or 0
    utc_delta = int(-local_off // 60)
    if utc_delta:
        payload["tu"] = utc_delta
    try:
        from zoneinfo import ZoneInfo

        et_off = ZoneInfo("America/New_York").utcoffset(
            datetime.datetime.fromtimestamp(now, datetime.timezone.utc)
        ).total_seconds()
    except (ImportError, KeyError, OSError, ValueError):
        return  # no tzdata (e.g. a bare Windows install) — UTC still goes out
    et_delta = int((et_off - local_off) // 60)
    if et_delta:
        payload["te"] = et_delta


def add_clock_fields(payload: dict) -> None:
    """Add "t" (local wall-clock epoch) + "tf" (12|24) when the config opts in.

    Also "te"/"tu" — minutes from that local time to US Eastern / UTC, for the
    small print under the device's clock (see add_zone_offset_fields).
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    now = time.time()
    payload["t"] = int(now) + time.localtime(now).tm_gmtoff
    payload["tf"] = tf
    add_zone_offset_fields(payload, now)


# The device's fonts are ASCII-only (0x20-0x7E) subsets, and typographic
# punctuation is exactly what a quotes site is full of. Fold the common
# offenders to their ASCII equivalents; anything still non-ASCII after NFKD
# decomposition (so accented letters keep their base form) is dropped.
_ASCII_SUBS = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",
    "“": '"', "”": '"', "„": '"', "«": '"', "»": '"',
    "–": "-", "—": "-", "‑": "-", "−": "-",
    "…": "...", " ": " ", "′": "'", "″": '"',
}


def _asciify(text: str) -> str:
    for src, dst in _ASCII_SUBS.items():
        text = text.replace(src, dst)
    return unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")


def _strip_markup(fragment: str) -> str:
    """Collapse an HTML fragment to a single line of ASCII-safe plain text."""
    text = html.unescape(re.sub(r"<[^>]+>", "", fragment))
    return re.sub(r"\s+", " ", _asciify(text)).strip()


def _shorten(text: str, limit: int) -> str:
    """Trim to `limit` characters on a word boundary, with a trailing "...".

    Three ASCII dots, not U+2026: the device's fonts are 0x20-0x7E subsets, so a
    real ellipsis has no glyph and renders as nothing.
    """
    if len(text) <= limit:
        return text
    cut = text[:limit - 3].rstrip()
    if " " in cut:
        cut = cut[:cut.rindex(" ")].rstrip()
    return cut.rstrip(",;:.") + "..."


def parse_quotes(page: str) -> list[dict]:
    """Every quote on a softwarequotes.com page, as device payload dicts.

    Pages are split into per-quote blocks first, so a text can't be paired with
    the next quote's author when one of them is missing.
    """
    quotes = []
    for block in QOD_BLOCK_SPLIT_RE.split(page)[1:]:
        text_m = QOD_TEXT_RE.search(block)
        if not text_m:
            continue
        text = _strip_markup(text_m.group(1))
        if not text:
            continue
        quote = {"t": _shorten(text, QOD_MAX_CHARS)}
        author_m = QOD_AUTHOR_RE.search(block)
        if author_m:
            author = _shorten(_strip_markup(author_m.group(1)), 30)
            if author:
                quote["a"] = author
        quotes.append(quote)
    return quotes


async def _get_page(http: httpx.AsyncClient, url: str) -> str:
    """GET a page, returning "" on any transport or HTTP failure."""
    try:
        resp = await http.get(url, headers=QUOTE_HEADERS)
        resp.raise_for_status()
        return resp.text
    except httpx.HTTPError as e:
        log(f"Quote page fetch failed ({url}): {e}")
        return ""


def _pick_topics(home_page: str, topics_page: str) -> list[str]:
    """Topic paths to scrape: the home page's featured ones plus random others."""
    featured = list(dict.fromkeys(QOD_TOPIC_RE.findall(home_page)))[:QOD_FEATURED_TOPICS]
    others = [t for t in dict.fromkeys(QOD_TOPIC_RE.findall(topics_page))
              if t not in featured]
    extra = random.sample(others, min(QOD_RANDOM_TOPICS, len(others))) if others else []
    return featured + extra


async def fetch_quote_pool() -> list[dict]:
    """Build the rotation pool: the quote of the day, then a sample of topics.

    Deduplicated by text, and shuffled below the first entry so consecutive
    quotes don't all come from the same topic page. Empty if the site is
    unreachable or its markup moved — callers keep their previous pool.
    """
    async with httpx.AsyncClient(timeout=10.0, follow_redirects=True) as http:
        qod_page, home_page, topics_page = await asyncio.gather(
            _get_page(http, QOD_URL),
            _get_page(http, QOD_HOME_URL),
            _get_page(http, QOD_TOPICS_URL),
        )
        pool = parse_quotes(qod_page)
        topics = _pick_topics(home_page, topics_page)
        if topics:
            pages = await asyncio.gather(
                *(_get_page(http, f"https://softwarequotes.com/{t}") for t in topics))
            rest = [q for page in pages for q in parse_quotes(page)]
            random.shuffle(rest)
            pool += rest

    seen, unique = set(), []
    for quote in pool:
        if quote["t"] not in seen:
            seen.add(quote["t"])
            unique.append(quote)
    if not unique:
        log("Quote pool came back empty; site unreachable or markup changed")
    return unique


async def add_quote_of_day_field(payload: dict) -> None:
    """Add "qd":{"t","a"} to the payload when the config opts in.

    Advances through the pool every QOD_ROTATE seconds — in practice on the next
    poll after that elapses, since the device only hears from us once a minute.
    A failed rebuild keeps the existing pool rotating rather than going blank.
    """
    global _qod_pool, _qod_pool_at, _qod_index, _qod_shown_at
    if read_config_flag("quote_of_day") != "on":
        return
    now = time.time()
    rebuilt = False
    if not _qod_pool or now - _qod_pool_at >= QOD_POOL_TTL:
        pool = await fetch_quote_pool()
        if pool:
            _qod_pool, _qod_pool_at = pool, now
            _qod_index, _qod_shown_at = 0, now
            rebuilt = True
            log(f"Quote pool rebuilt: {len(pool)} quotes")
    if not _qod_pool:
        return
    # A fresh pool starts on its first entry — the actual quote of the day —
    # rather than immediately stepping off it.
    if not rebuilt and now - _qod_shown_at >= QOD_ROTATE:
        _qod_index = (_qod_index + 1) % len(_qod_pool)
        _qod_shown_at = now
    # A copy: the payload is handed to the writer, which may rewrite the field,
    # and the pool must stay pristine for the next poll.
    payload["qd"] = dict(_qod_pool[_qod_index])


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        # Network/DNS/timeout — transient. Return None (no toast), retry next tick.
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        # Genuine auth rejection — the ONLY case that warrants the actionable
        # "run claude login" toast.
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        raise AuthError(resp.status_code)
    if resp.status_code >= 400:
        # Other 4xx/5xx (rate-limit, server error) — transient, not a token issue.
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    now = time.time()

    def reset_minutes(reset_ts: str) -> int:
        try:
            r = float(reset_ts)
        except ValueError:
            return 0
        mins = (r - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(util: str) -> int:
        try:
            return int(round(float(util) * 100))
        except ValueError:
            return 0

    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    await add_quote_fields(payload)   # adds "q":[...] iff `tickers` is set
    await add_quote_of_day_field(payload)   # adds "qd":{...} iff the config opts in
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Monthly window is assumed (headers expose only reset_ts, not period). Per the
    Claude Enterprise Admin API reference, spend-limit period's "only value today
    is monthly" — see the macOS daemon for the full note.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30, "rd": ""}
    if period_end <= 0:
        # reset_ts defaults to "0" whenever the overage-reset header is absent
        # (e.g. a 200 that simply carries no billing headers). fromtimestamp(0)
        # is 1970; stepping one month back lands in 1969, and datetime.timestamp()
        # raises OSError for pre-1970 dates on Windows — taking the whole poll
        # loop down. Bail out to the neutral default instead.
        return {"tp": 0, "pd": 30, "rd": ""}
    try:
        dt_end = datetime.datetime.fromtimestamp(period_end)
        prev_month = dt_end.month - 1 or 12
        prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
        prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
        dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
        period_start = dt_start.timestamp()
    except (OSError, OverflowError, ValueError):
        # Belt-and-braces beyond the <= 0 guard above (#104): Windows
        # datetime.timestamp()/fromtimestamp() also raise OSError(22)/
        # OverflowError/ValueError for out-of-range NON-zero values (e.g. a
        # far-future "99999999999999" header, which overflows fromtimestamp).
        # Garbage must never crash the daemon thread — degrade to the safe
        # default instead (field report: OSError(22) killed the poll loop).
        return {"tp": 0, "pd": 30, "rd": ""}
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30, "rd": ""}
    pct_val = (now - period_start) / period_len * 100
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": int(round(period_len / 86400)),
        "rd": f"{dt_end.strftime('%b')} {dt_end.day}",
    }


def _mac_from_pnp_instance_id(instance_id: str) -> str | None:
    """Recover a canonical BLE MAC ("AA:BB:CC:DD:EE:FF") from a PnP instance id.

    Windows encodes a paired BLE device's address in its PnP instance id as a
    12-hex run after a ``DEV_`` token, e.g.::

        BTHLE\\DEV_98A316A5D706\\7&B8081D1&0&98A316A5D706  ->  98:A3:16:A5:D7:06

    Returns None when no ``DEV_<12 hex>`` token is present. Pure — the
    subprocess that produces the instance id lives in discover_bonded_address().
    """
    m = re.search(r"DEV_([0-9A-Fa-f]{12})(?![0-9A-Fa-f])", instance_id)
    if not m:
        return None
    h = m.group(1).upper()
    return ":".join(h[i:i + 2] for i in range(0, 12, 2))


def discover_bonded_address() -> str | None:
    """Return the BLE address of the bonded Clawdmeter, or None.

    A device that is paired AND connected to Windows stops advertising, so
    BleakScanner can't see it (the steady state once paired — see
    README-windows.md). WinRT can still connect to it directly by address, so
    we recover that address from the OS:

    1. CLAWDMETER_BLE_ADDRESS env override (skips discovery — testing / pinning).
    2. Windows PnP table, filtered to the device's FriendlyName.

    Non-Windows or any failure returns None.
    """
    if override := os.environ.get("CLAWDMETER_BLE_ADDRESS"):
        return override.strip().upper()
    if sys.platform != "win32":
        return None
    command = (
        "Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | "
        f"Where-Object {{ $_.FriendlyName -eq '{DEVICE_NAME}' }} | "
        "Select-Object -ExpandProperty InstanceId"
    )
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
            capture_output=True,
            text=True,
            timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    except (OSError, subprocess.SubprocessError) as e:
        log(f"Bonded-address lookup failed: {e}")
        return None
    for line in result.stdout.splitlines():
        if mac := _mac_from_pnp_instance_id(line):
            return mac
    return None


async def acquire_target():
    """Return a connectable handle for the Clawdmeter, or None.

    Targets only the device bonded to THIS machine (via the PnP table /
    CLAWDMETER_BLE_ADDRESS) — it never scans for a nearby device by name, so it
    can't grab a stranger's or the wrong nearby unit. The device must be paired
    with Windows once first (the documented setup). Returns a BLEDevice or None.
    """
    address = discover_bonded_address()
    if not address:
        return None
    log(f"Not advertising; connecting to bonded address {address}")
    # CRITICAL: hand BleakClient a BLEDevice, not the bare address string. WinRT's
    # connect() resolves a bare string via an advertisement scan (find_device_by_address)
    # — which always fails for a bonded device that has stopped advertising, the very
    # case we are handling. A BLEDevice sets _device_info directly, so WinRT connects
    # via from_bluetooth_address_with_bluetooth_address_type_async and skips the scan.
    return BLEDevice(address, DEVICE_NAME, None)


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # The refresh subscription is optional — the 60s poll loop works without it.
        # WinRT's start_notify() CCCD write can raise a raw OSError/WinError (not
        # wrapped as BleakError) when the peer GATT server is transiently unavailable,
        # e.g. a just-power-cycled ESP32 whose server is not yet ready (G-03-01, SC#3).
        # Degrade gracefully instead of crashing the daemon so it stays single-process
        # across a power-cycle reconnect (SC#4, no restart).
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError, OSError) as e:
            log(f"Refresh subscription unavailable: {e}")

    def write_limit(self) -> int:
        """Bytes that fit one write-without-response on this link.

        The firmware's RX buffer is 512B, but a single unacknowledged write can
        only carry ATT_MTU-3 — so the real ceiling is whatever MTU the host and
        device negotiated (macOS commonly lands well under 512). bleak exposes
        it per-backend and can raise if the link went away mid-call, hence the
        conservative fallback.
        """
        try:
            return max(20, self.client.mtu_size - 3)
        except Exception:  # noqa: BLE001 - backend-specific failures, all non-fatal
            return 180

    async def _write(self, data: bytes) -> bool:
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except (BleakError, OSError) as e:
            # WinRT can raise a raw OSError/WinError (NOT wrapped as BleakError)
            # when the peer GATT server goes transiently unavailable mid-write —
            # the same failure class setup_refresh_subscription() guards against.
            # Returning False trips the zombie-link break -> clean reconnect,
            # rather than an uncaught exception killing the daemon thread (the
            # silent-freeze failure mode, SC#2 field report).
            log(f"Write failed: {e}")
            return False

    async def write_payload(self, payload: dict) -> bool:
        """Write the payload, splitting the quote out if it won't fit in one go.

        A write-without-response can't span packets, so an over-MTU payload has
        to lose something. The quote is never shortened to make room — half an
        aphorism is worse than none — so instead the usage payload goes out with
        `"qd":1`, telling the firmware "your quote still stands, an update is
        coming", and the quote follows as its own `{"qd":{...}}` write that the
        firmware merges without touching the usage numbers.
        """
        limit = self.write_limit()
        quote = payload.get("qd")
        data = json.dumps(payload, separators=(",", ":")).encode()
        if len(data) <= limit or not isinstance(quote, dict):
            return await self._write(data)

        solo = json.dumps({"qd": quote}, separators=(",", ":")).encode()
        if len(solo) > limit:
            # Nothing sensible left to do: dropping the quote entirely at least
            # keeps the usage numbers whole and honest.
            log(f"Quote needs {len(solo)}B but only {limit}B fit a write; skipping it")
            payload.pop("qd", None)
            return await self._write(json.dumps(payload, separators=(",", ":")).encode())

        payload["qd"] = 1   # sentinel: keep the current quote, update follows
        first = json.dumps(payload, separators=(",", ":")).encode()
        return await self._write(first) and await self._write(solo)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _windows_credential_candidates() -> list[Path]:
    """Return the ordered list of credential file paths to probe (first hit wins).

    Priority:
    1. CLAUDE_CREDENTIALS_PATH env override (D-03, project-specific)
    2. CLAUDE_CONFIG_DIR env override (official Claude override)
    3. D-02 candidate list: home/.claude, LOCALAPPDATA/Claude, APPDATA/Claude
    """
    # Priority 1: project-specific env override (D-03)
    if override := os.environ.get("CLAUDE_CREDENTIALS_PATH"):
        return [Path(override)]
    # Priority 2: official CLAUDE_CONFIG_DIR env override
    if config_dir := os.environ.get("CLAUDE_CONFIG_DIR"):
        return [Path(config_dir) / ".credentials.json"]
    # Priority 3: D-02 candidate list — first hit wins
    home = Path.home()
    local_appdata = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
    appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
    return [
        home / ".claude" / ".credentials.json",          # primary (confirmed by docs)
        local_appdata / "Claude" / ".credentials.json",  # fallback 2
        appdata / "Claude" / ".credentials.json",        # fallback 3
    ]


def read_token() -> str | None:
    """Read the Claude OAuth access token from the first available credential file."""
    for path in _windows_credential_candidates():
        try:
            return _extract_access_token(path.read_text(encoding="utf-8"))
        except OSError:
            continue
    return None


def _read_expiry() -> str:
    """Return human-readable expiry from the first-hit credentials file.

    Reads claudeAiOauth.expiresAt (epoch milliseconds — JS convention).
    Divides by 1000 before passing to fromtimestamp (Python expects seconds).
    Returns 'expiry unknown' on any parse failure.
    """
    for path in _windows_credential_candidates():
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            continue
        try:
            data = json.loads(raw)
            oauth = data.get("claudeAiOauth", {})
            expires_ms = oauth.get("expiresAt")
            if expires_ms is None:
                return "expiry unknown"
            # CRITICAL: expiresAt is JS-convention epoch milliseconds; divide by 1000
            # before fromtimestamp (Python expects seconds). Raw value -> year ~57000.
            dt = datetime.datetime.fromtimestamp(
                expires_ms / 1000, tz=datetime.timezone.utc
            )
            return dt.strftime("%Y-%m-%d %H:%M UTC")
        except (TypeError, ValueError, OSError, AttributeError, json.JSONDecodeError):
            return "expiry unknown"
    return "expiry unknown"


async def _wait_first(*events: asyncio.Event, timeout: float) -> None:
    """Return when any of `events` is set, or after `timeout` seconds.

    Lets the poll loop's TICK wait wake immediately on a stop signal (clean,
    responsive Quit) without losing the refresh-request wakeup — instead of
    waiting only on refresh_requested and re-checking stop_event up to TICK
    later. Cancels and drains the loser tasks so they don't warn.
    """
    tasks = [asyncio.ensure_future(e.wait()) for e in events]
    try:
        await asyncio.wait(tasks, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def connect_and_run(device, stop_event: asyncio.Event, tray_state=None) -> bool:
    """Connect to device and poll until disconnected or stopped.

    Returns True if at least one successful write occurred.

    `device` is a BLEDevice — either from an advertisement scan or built from the
    bonded address by acquire_target(). The getattr keeps the log line robust if a
    bare address string is ever passed in.
    """
    log(f"Connecting to {getattr(device, 'address', device)}...")
    # D-01: retry wrapper — defeats WinRT post-wake failure modes
    # (Could not get GATT services: Unreachable, stale is_connected).
    # Rebuild a fresh BleakClient each attempt (locked D-05 recipe).
    client = None
    for attempt in range(CONNECT_RETRIES):
        # D-05: pass BLEDevice (not address string), address_type="random" (NimBLE
        # static-random), use_cached_services=False (DIY firmware — WinRT GATT cache
        # may be stale after firmware reflash).
        client = BleakClient(
            device,
            address_type="random",
            use_cached_services=False,
        )
        try:
            await client.connect()
        except (BleakError, OSError, asyncio.TimeoutError, AssertionError) as e:
            # WinRT service discovery inside connect() can surface a raw OSError
            # (WinError) or even a bare AssertionError from bleak's FutureLike
            # (assert self._result) when the peer drops the link mid-discovery —
            # neither is wrapped as BleakError. Treat them as a normal failed
            # attempt so the D-01 retry loop handles them, instead of letting an
            # uncaught exception kill the daemon thread (the "daemon crashed"
            # tray toast + silent polling stop, field report).
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed: {type(e).__name__}: {e}")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        if not client.is_connected:
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed (not connected)")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        # Connected successfully
        break
    else:
        log(f"Connection failed after {CONNECT_RETRIES} attempts")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0  # D-03: poll immediately on first connect
    used_successfully = False
    consecutive_failures = 0  # D-03: zombie-link break counter
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()  # D-09: fresh each cycle
                if not token:
                    log("No token; skipping poll")
                    if tray_state:
                        tray_state.set_error("token expired — run claude login")
                else:
                    try:
                        payload = await poll_api(token)
                    except AuthError:
                        # Real 401/403 — token genuinely needs a refresh.
                        if tray_state:
                            tray_state.set_error("token expired — run claude login")
                        payload = None
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True
                            consecutive_failures = 0  # D-03: reset on success
                            if tray_state:
                                tray_state.set_connected(time.time())
                        else:
                            consecutive_failures += 1
                            if consecutive_failures >= ZOMBIE_BREAK_LIMIT:
                                log(
                                    f"Zombie link detected ({consecutive_failures} consecutive"
                                    f" write failures); abandoning connection"
                                )
                                break
                    # else: payload is None from a TRANSIENT failure (network/DNS,
                    # timeout, rate-limit, 5xx). poll_api already logged it; do NOT
                    # toast "token expired" — that mislabeled a boot-time DNS blip
                    # as an auth problem (SC#5). Leave tray state unchanged; the next
                    # tick retries and set_connected() recovers it.

            # Wake on a refresh request OR a stop, whichever comes first. Waking
            # promptly on stop_event is what lets the finally below run
            # client.disconnect() before the process exits, so the peer gets a
            # clean GATT disconnect (returns to its waiting screen) instead of
            # being left frozen on stale data after Quit (SC#3 graceful shutdown).
            await _wait_first(session.refresh_requested, stop_event, timeout=TICK)
    finally:
        # Clean GATT disconnect on the way out — this is what tells the peripheral
        # the link is gone. WinRT can surface a raw OSError (not BleakError) here,
        # so swallow both; the link tears down regardless once we exit.
        try:
            await client.disconnect()
        except (BleakError, OSError, AssertionError):
            # bleak's WinRT disconnect() also has bare asserts (e.g. assert char
            # while tearing down notifications on an already-gone peer); swallow
            # it too — the link tears down regardless once we exit.
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


def _next_backoff(current: int, cap: int) -> int:
    """D-05: double current backoff value, clamped to cap.

    Pure helper — unit-testable without driving the main loop.
    Used by both slow-search (cap=60) and fast-reconnect (cap=RECONNECT_BACKOFF_CAP) regimes.
    """
    return min(current * 2, cap)


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    # Populate the shared state object so the tray can route Quit through
    # loop.call_soon_threadsafe (RESEARCH Pitfall 2).  Additive — the existing
    # stop_event = asyncio.Event() line above is unchanged.
    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    # OS signal handlers can only be installed from the main thread, and
    # loop.add_signal_handler is unsupported on Windows. When running under the
    # tray (04-03) the loop lives in a background thread and the tray owns clean
    # shutdown via stop_event (loop.call_soon_threadsafe), so skip silently there.
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                # Windows: add_signal_handler not supported; fall back to signal.signal
                try:
                    signal.signal(sig, _stop)
                except ValueError:
                    # Not the main thread of the main interpreter — tray owns shutdown.
                    pass

    log("=== Claude Usage Tracker Daemon (BLE, Windows) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # D-05: two distinct backoff regimes — slow-search (device absent) vs fast-reconnect (link dropped)
    search_backoff = 1     # caps at 60s — gentle, for a device that is genuinely absent/off
    reconnect_backoff = 1  # caps at RECONNECT_BACKOFF_CAP — fast, to clear the 120s SLA after a drop
    while not stop_event.is_set():
        device = await acquire_target()
        if not device:
            # Slow-search regime: device was not found by scan — back off gently
            if tray_state:
                tray_state.set_scanning()
            log(f"Device not found, retrying in {search_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=search_backoff)
            except asyncio.TimeoutError:
                pass
            search_backoff = _next_backoff(search_backoff, 60)
            continue

        ok = await connect_and_run(device, stop_event, tray_state)
        if not ok:
            # Fast-reconnect regime: had/attempted a link that dropped — retry quickly
            if tray_state:
                tray_state.set_scanning()
            log(f"Connection lost, reconnecting in {reconnect_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=reconnect_backoff)
            except asyncio.TimeoutError:
                pass
            reconnect_backoff = _next_backoff(reconnect_backoff, RECONNECT_BACKOFF_CAP)
        else:
            # Successful session — reset reconnect counter to floor; search_backoff also reset
            reconnect_backoff = 1
            search_backoff = 1


if __name__ == "__main__":
    if sys.platform != "win32":
        print(
            "Warning: running under Linux/WSL — WinRT BLE will not be available.",
            file=sys.stderr,
        )
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
