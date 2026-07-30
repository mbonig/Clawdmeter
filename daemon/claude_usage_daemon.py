#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import datetime
import getpass
import html
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time
import unicodedata
from pathlib import Path

import httpx
from bleak import BleakClient
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

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
# config). Yahoo's chart endpoint is used rather than /v7/finance/quote because
# the latter now requires a cookie+crumb handshake; with range=1d&interval=1d
# the response carries both the last price and the previous close, which is all
# the device needs. It does require a browser-ish User-Agent.
QUOTE_URL = "https://query1.finance.yahoo.com/v8/finance/chart/{sym}?range=1d&interval=1d"
QUOTE_HEADERS = {"User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"}
# Prices refresh on their own clock, not the API poll's: a desk ornament doesn't
# need tick-by-tick quotes, and this keeps us to ~12 requests/hour per symbol.
QUOTE_TTL = 300
# The device renders at most 3 cards' worth of quotes, and the whole JSON payload
# has to fit one BLE write (see MAX_QUOTES in firmware/src/data.h).
MAX_TICKERS = 3
# Convenience aliases for company names that aren't their ticker. SpaceX listed
# on NasdaqGS as SPCX in June 2026; the device always displays the real symbol
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


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


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
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_token_keychain() -> str | None:
    try:
        out = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except subprocess.CalledProcessError as e:
        log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
        return None
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        log(f"Keychain access error: {e}")
        return None
    return _extract_access_token(out.stdout)


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to generic services the OS indexes for any accessory, but
       ONLY trust a peripheral whose name matches DEVICE_NAME, since these
       also match unrelated keyboards/mice/headphones.

    Step 2 is not a nicety — it's the normal path once the device is paired
    as a BLE HID keyboard (which it must be, for its buttons to work at all;
    see the firmware's ble.cpp). Observed on macOS 26: with the device
    genuinely connected and held by the OS, retrieveConnectedPeripheralsWithServices_
    returns EMPTY for our custom service UUID and for 0x1812, but does return
    it for 0x180A (Device Information) and 0x180F (Battery). macOS only
    indexes services it has itself discovered, and it never does a full
    service discovery on behalf of an app — while 0x1812 is deliberately
    hidden from all app-level BLE clients. So the custom service is only
    visible *after* we connect and discover, which is exactly what we can't
    do until we've found the peripheral. Hence the generic-service fallback.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic services, name-matched. 0x1812 (HID) is listed first for the
    #    ideal case, but in practice macOS hides it from apps and only 0x180A /
    #    0x180F come back for a bonded HID keyboard — see the docstring.
    fallback = cm.retrieveConnectedPeripheralsWithServices_(
        [
            CBUUID.UUIDWithString_("1812"),  # HID
            CBUUID.UUIDWithString_("180A"),  # Device Information
            CBUUID.UUIDWithString_("180F"),  # Battery
        ]
    )
    for p in fallback or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    The daemon only ever targets the device this system already holds — it
    never scans for a nearby device by name, so it can't grab a stranger's or
    the wrong nearby unit. On macOS that's the system-connected peripheral (the
    firmware advertises as an HID keyboard, so once paired the OS auto-connects
    and holds it — HID-grabbed devices are invisible to scans anyway). On other
    platforms it's a previously-pinned address in the cache file. If the device
    isn't held/pinned, we log and wait rather than scanning. ``skip_addr`` skips
    a peripheral whose handle just failed to connect.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is None:
            log("Device not held by OS; waiting (not scanning by name)")
        return dev

    address = load_cached_address()
    if not address:
        log("No pinned address cached; waiting (not scanning by name)")
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
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

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
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


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


def read_tickers_setting() -> list[str]:
    """Read the `tickers` option: market symbols for the device's center rotator.

    Comma-separated; aliases (see TICKER_ALIASES) are resolved and the list is
    capped at MAX_TICKERS. Empty (default) means no quotes are sent, so existing
    setups are unaffected until the user opts in.
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
    """Fetch one symbol as the device's wire form, or None if it can't be priced.

    The price string is formatted here rather than on the device so currency and
    precision stay a host-side decision; only the percent change crosses as a
    number, because the firmware colors it by sign.
    """
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
    """Add "q":[{n,p,d}, ...] to the payload for each configured ticker.

    Cached for QUOTE_TTL, so a 60s API poll doesn't mean a 60s quote poll. A
    symbol that fails to fetch keeps serving its last good value until it
    succeeds again — and is simply omitted if it never did.
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
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
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

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
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

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    if period_end <= 0:
        # reset_ts defaults to "0" when the overage-reset header is absent.
        # fromtimestamp(0) is 1970; stepping a month back lands in 1969, and
        # datetime.timestamp() raises OSError for pre-1970 dates on Windows.
        # Benign on macOS/Linux, but guard here too to keep the daemons parallel.
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """Poll every configured config dir and return the active plan's payload.

    Returns None when no dir yields a usable payload this cycle. A single
    configured dir (the default) collapses to exactly the old single-poll path.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        payload = await poll_api(token)
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return payloads[active]


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

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
        except BleakError as e:
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


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    session = Session(client)
    # The write limit bounds the optional quote of the day, so it's worth seeing.
    log(f"Connected (payload limit {session.write_limit()}B)")
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                payload = await poll_active_payload()
                if payload is None:
                    log("No usable config dir this cycle")
                elif await session.write_payload(payload):
                    last_poll = time.time()
                    used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
