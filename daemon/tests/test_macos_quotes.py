#!/usr/bin/env python3
"""Unit tests for the macOS/Linux daemon's market-quote support.

Covers read_tickers_setting, fetch_quote's wire form, and add_quote_fields'
caching / degradation behaviour (the fields feeding the device's center rotator).

Run: python -m pytest daemon/tests/test_macos_quotes.py -x -q
"""
import asyncio
import json
import time
from unittest.mock import AsyncMock, patch

from bleak.exc import BleakError

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import (
    add_quote_fields,
    fetch_quote,
    read_tickers_setting,
)


def _run(coro):
    return asyncio.run(coro)


class _FakeResponse:
    def __init__(self, payload):
        self._payload = payload

    def raise_for_status(self):
        pass

    def json(self):
        return self._payload


class _FakeClient:
    """Stands in for httpx.AsyncClient, serving one canned meta dict per symbol."""

    def __init__(self, metas):
        self._metas = metas
        self.calls = []

    async def get(self, url, headers=None):
        self.calls.append(url)
        sym = url.split("/chart/")[1].split("?")[0]
        return _FakeResponse({"chart": {"result": [{"meta": self._metas[sym]}]}})


def _meta(**kw):
    base = {"symbol": "AMZN", "regularMarketPrice": 237.73,
            "chartPreviousClose": 226.65, "currency": "USD"}
    base.update(kw)
    return base


# ---------------------------------------------------------------------------
# read_tickers_setting
# ---------------------------------------------------------------------------

def test_tickers_absent_config_is_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    assert read_tickers_setting() == []


def test_tickers_key_absent_is_empty(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\nchime = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_tickers_setting() == []


def test_tickers_parses_list_uppercases_and_strips_comment(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("tickers = amzn, MSFT  # two symbols\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_tickers_setting() == ["AMZN", "MSFT"]


def test_tickers_resolves_spacex_alias_and_dedupes(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("tickers = SpaceX, spcx, AMZN\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    # spacex -> SPCX, so the explicit SPCX collapses into it.
    assert read_tickers_setting() == ["SPCX", "AMZN"]


def test_tickers_capped_at_max(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("tickers = A, B, C, D, E\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_tickers_setting() == ["A", "B", "C"][:mod.MAX_TICKERS]
    assert len(read_tickers_setting()) == mod.MAX_TICKERS


# ---------------------------------------------------------------------------
# fetch_quote
# ---------------------------------------------------------------------------

def test_fetch_quote_formats_price_and_percent_change():
    q = _run(fetch_quote(_FakeClient({"AMZN": _meta()}), "AMZN"))
    assert q == {"n": "AMZN", "p": "$237.73", "d": 4.89}


def test_fetch_quote_thousands_separator_and_non_usd():
    client = _FakeClient({"X": _meta(symbol="X", regularMarketPrice=1234.5,
                                     chartPreviousClose=1234.5, currency="EUR")})
    assert _run(fetch_quote(client, "X"))["p"] == "1,234.50"


def test_fetch_quote_without_previous_close_omits_change():
    client = _FakeClient({"X": _meta(symbol="X", chartPreviousClose=None)})
    assert "d" not in _run(fetch_quote(client, "X"))


def test_fetch_quote_without_price_is_none():
    client = _FakeClient({"X": _meta(symbol="X", regularMarketPrice=None)})
    assert _run(fetch_quote(client, "X")) is None


def test_fetch_quote_swallows_transport_errors():
    class Boom:
        async def get(self, url, headers=None):
            raise mod.httpx.ConnectError("nope")

    assert _run(fetch_quote(Boom(), "AMZN")) is None


# ---------------------------------------------------------------------------
# add_quote_fields
# ---------------------------------------------------------------------------

def _configure(monkeypatch, tmp_path, tickers):
    cfg = tmp_path / "config"
    cfg.write_text(f"tickers = {tickers}\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    monkeypatch.setattr(mod, "_quote_cache", {})


def test_add_quote_fields_absent_when_unconfigured(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    payload = {"s": 12}
    _run(add_quote_fields(payload))
    assert "q" not in payload


def test_add_quote_fields_populates_in_config_order(tmp_path, monkeypatch):
    _configure(monkeypatch, tmp_path, "AMZN, SPCX")
    quotes = {"AMZN": {"n": "AMZN", "p": "$237.73", "d": 4.89},
              "SPCX": {"n": "SPCX", "p": "$113.06", "d": 0.45}}
    with patch.object(mod, "fetch_quote", AsyncMock(side_effect=lambda _h, s: quotes[s])):
        payload = {"s": 12}
        _run(add_quote_fields(payload))
    assert payload["q"] == [quotes["AMZN"], quotes["SPCX"]]


def test_add_quote_fields_serves_cache_within_ttl(tmp_path, monkeypatch):
    _configure(monkeypatch, tmp_path, "AMZN")
    fetch = AsyncMock(return_value={"n": "AMZN", "p": "$237.73", "d": 4.89})
    with patch.object(mod, "fetch_quote", fetch):
        first, second = {}, {}
        _run(add_quote_fields(first))
        _run(add_quote_fields(second))
    assert fetch.await_count == 1          # second call came from the cache
    assert second["q"] == first["q"]


def test_add_quote_fields_refetches_after_ttl(tmp_path, monkeypatch):
    _configure(monkeypatch, tmp_path, "AMZN")
    monkeypatch.setattr(mod, "QUOTE_TTL", 0)
    fetch = AsyncMock(return_value={"n": "AMZN", "p": "$237.73", "d": 4.89})
    with patch.object(mod, "fetch_quote", fetch):
        _run(add_quote_fields({}))
        _run(add_quote_fields({}))
    assert fetch.await_count == 2


def test_add_quote_fields_keeps_last_good_value_on_failure(tmp_path, monkeypatch):
    _configure(monkeypatch, tmp_path, "AMZN")
    monkeypatch.setattr(mod, "QUOTE_TTL", 0)
    good = {"n": "AMZN", "p": "$237.73", "d": 4.89}
    with patch.object(mod, "fetch_quote", AsyncMock(return_value=good)):
        _run(add_quote_fields({}))
    with patch.object(mod, "fetch_quote", AsyncMock(return_value=None)):
        payload = {}
        _run(add_quote_fields(payload))
    assert payload["q"] == [good]


def test_add_quote_fields_omits_symbol_never_fetched(tmp_path, monkeypatch):
    _configure(monkeypatch, tmp_path, "AMZN, NOPE")
    quotes = {"AMZN": {"n": "AMZN", "p": "$237.73", "d": 4.89}, "NOPE": None}
    with patch.object(mod, "fetch_quote", AsyncMock(side_effect=lambda _h, s: quotes[s])):
        payload = {}
        _run(add_quote_fields(payload))
    assert payload["q"] == [quotes["AMZN"]]


def test_full_payload_fits_one_ble_write(tmp_path, monkeypatch):
    """The device's RX buffer is 512B and the daemon writes without response, so a
    fully-loaded payload (usage + clock + max tickers) must stay comfortably small."""
    _configure(monkeypatch, tmp_path, "AMZN, SPCX, MSFT")
    quotes = {s: {"n": s, "p": "$1,234.56", "d": -12.34} for s in ("AMZN", "SPCX", "MSFT")}
    with patch.object(mod, "fetch_quote", AsyncMock(side_effect=lambda _h, s: quotes[s])):
        payload = {"s": 100, "sr": 300, "w": 100, "wr": 10080, "st": "allowed",
                   "acct": "pro", "ok": True, "c": 1, "t": 1785432596, "tf": 12}
        _run(add_quote_fields(payload))
    wire = json.dumps(payload, separators=(",", ":"))
    assert len(wire) < 250, wire


# ---------------------------------------------------------------------------
# Software quotes (softwarequotes.com scrape + 5-minute rotation)
# ---------------------------------------------------------------------------

def _quote_block(text, author="Anonymous"):
    return f"""
  <div class="quote mt60 wow zoomIn mb20">
    <div class="quote__center">
      <a href="https://softwarequotes.com/quote/x"><p>{text}
</p></a>
    </div>
    <div class="quote__author"><a href="https://softwarequotes.com/author/a">
        {author}
    </a></div>
  </div>
"""


QOD_PAGE = "<main>" + _quote_block("With enough practice, any interface is intuitive.") + "</main>"
TOPIC_LINKS = "".join(
    f'<a href="https://softwarequotes.com/topic/{t}">{t}</a>'
    for t in ("code", "design", "funny", "bug", "obscure1", "obscure2"))
HOME_PAGE = "<main>" + TOPIC_LINKS[:3 * len('<a href="https://softwarequotes.com/topic/code">code</a>')] + "</main>"
TOPICS_PAGE = "<main>" + TOPIC_LINKS + "</main>"


class _FakeSite:
    """Serves canned pages per URL, recording what was asked for."""

    def __init__(self, pages=None, exc=None):
        self.pages = pages or {}
        self.exc = exc
        self.requested = []

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    async def get(self, url, headers=None):
        self.requested.append(url)
        if self.exc:
            raise self.exc
        for key, body in self.pages.items():
            if url.endswith(key):
                return _FakeTextResponse(body)
        # An unlisted topic page: two quotes named after it.
        slug = url.rstrip("/").rsplit("/", 1)[-1]
        return _FakeTextResponse(
            "<main>" + _quote_block(f"{slug} one") + _quote_block(f"{slug} two") + "</main>")


class _FakeTextResponse:
    def __init__(self, text):
        self.text = text

    def raise_for_status(self):
        pass


def _patch_site(monkeypatch, pages=None, exc=None):
    site = _FakeSite(pages, exc)
    monkeypatch.setattr(mod.httpx, "AsyncClient", lambda *a, **kw: site)
    return site


DEFAULT_PAGES = {"quote-of-the-day": QOD_PAGE, "topics": TOPICS_PAGE,
                 "softwarequotes.com/": HOME_PAGE}


def test_parse_quotes_pairs_each_text_with_its_own_author():
    page = "<main>" + _quote_block("First one.", "Dijkstra") + _quote_block("Second one.", "Knuth") + "</main>"
    assert mod.parse_quotes(page) == [
        {"t": "First one.", "a": "Dijkstra"},
        {"t": "Second one.", "a": "Knuth"},
    ]


def test_parse_quotes_folds_typographic_punctuation_to_ascii():
    page = "<main>" + _quote_block("Don\u2019t \u201cfix\u201d it \u2014 rewrite it\u2026", "Ada Lovel\u00e1ce") + "</main>"
    q = mod.parse_quotes(page)[0]
    # The device's fonts are ASCII-only subsets: a stray glyph renders as a blank.
    assert q == {"t": 'Don\'t "fix" it - rewrite it...', "a": "Ada Lovelace"}
    assert all(0x20 <= ord(c) <= 0x7E for c in q["t"] + q["a"])


def test_parse_quotes_keeps_a_normal_length_quote_whole():
    body = "Any fool can write code that a computer can understand. " * 2
    assert mod.parse_quotes("<main>" + _quote_block(body) + "</main>")[0]["t"] == body.strip()


def test_parse_quotes_cuts_only_essay_length_text_on_a_word_boundary():
    # QOD_MAX_CHARS is a sanity bound, not a fitting mechanism — the device
    # shrinks its font for long quotes, so only something absurd gets cut.
    text = mod.parse_quotes("<main>" + _quote_block("word " * 120) + "</main>")[0]["t"]
    assert len(text) <= mod.QOD_MAX_CHARS
    assert text.endswith("word...")


def test_parse_quotes_ignores_unknown_markup():
    assert mod.parse_quotes("<div class='brand-new-markup'>hello</div>") == []


def test_pool_leads_with_the_quote_of_the_day_and_adds_topics(monkeypatch):
    _patch_site(monkeypatch, DEFAULT_PAGES)
    pool = _run(mod.fetch_quote_pool())
    assert pool[0] == {"t": "With enough practice, any interface is intuitive.",
                       "a": "Anonymous"}
    assert len(pool) > 1
    assert len({q["t"] for q in pool}) == len(pool)   # deduplicated


def test_pool_is_empty_when_the_site_is_unreachable(monkeypatch):
    _patch_site(monkeypatch, exc=mod.httpx.ConnectError("nope"))
    assert _run(mod.fetch_quote_pool()) == []


def test_pool_prefers_the_featured_topics_then_samples_others(monkeypatch):
    site = _patch_site(monkeypatch, DEFAULT_PAGES)
    monkeypatch.setattr(mod, "QOD_FEATURED_TOPICS", 2)
    monkeypatch.setattr(mod, "QOD_RANDOM_TOPICS", 1)
    _run(mod.fetch_quote_pool())
    topic_urls = [u for u in site.requested if "/topic/" in u]
    assert len(topic_urls) == 3
    assert topic_urls[0].endswith("/topic/code")      # featured, in page order
    assert topic_urls[1].endswith("/topic/design")


def _enable_quotes(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("quote_of_day = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    monkeypatch.setattr(mod, "_qod_pool", [])
    monkeypatch.setattr(mod, "_qod_pool_at", 0.0)
    monkeypatch.setattr(mod, "_qod_index", 0)
    monkeypatch.setattr(mod, "_qod_shown_at", 0.0)


POOL = [{"t": "one", "a": "A"}, {"t": "two", "a": "B"}, {"t": "three", "a": "C"}]


def test_add_qod_absent_unless_opted_in(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    payload = {}
    _run(mod.add_quote_of_day_field(payload))
    assert "qd" not in payload


def test_add_qod_holds_one_quote_until_the_rotation_elapses(tmp_path, monkeypatch):
    _enable_quotes(tmp_path, monkeypatch)
    with patch.object(mod, "fetch_quote_pool", AsyncMock(return_value=list(POOL))):
        first, second = {}, {}
        _run(mod.add_quote_of_day_field(first))
        _run(mod.add_quote_of_day_field(second))
    assert first["qd"] == POOL[0] == second["qd"]   # same quote within QOD_ROTATE


def test_add_qod_advances_every_rotation_and_wraps(tmp_path, monkeypatch):
    _enable_quotes(tmp_path, monkeypatch)
    monkeypatch.setattr(mod, "QOD_ROTATE", 0)   # every poll counts as elapsed
    fetch = AsyncMock(return_value=list(POOL))
    seen = []
    with patch.object(mod, "fetch_quote_pool", fetch):
        for _ in range(4):
            payload = {}
            _run(mod.add_quote_of_day_field(payload))
            seen.append(payload["qd"]["t"])
    assert seen == ["one", "two", "three", "one"]   # rotates, then wraps
    assert fetch.await_count == 1                    # pool built once, not per poll


def test_add_qod_keeps_rotating_when_a_rebuild_fails(tmp_path, monkeypatch):
    _enable_quotes(tmp_path, monkeypatch)
    with patch.object(mod, "fetch_quote_pool", AsyncMock(return_value=list(POOL))):
        _run(mod.add_quote_of_day_field({}))
    monkeypatch.setattr(mod, "QOD_POOL_TTL", 0)     # force a rebuild attempt
    with patch.object(mod, "fetch_quote_pool", AsyncMock(return_value=[])):
        payload = {}
        _run(mod.add_quote_of_day_field(payload))
    assert payload["qd"] == POOL[0]                 # old pool still serving


# ---------------------------------------------------------------------------
# Session.write_payload — one unacknowledged BLE write, quote never truncated
# ---------------------------------------------------------------------------

class _StubClient:
    def __init__(self, mtu=None):
        self._mtu = mtu
        self.writes = []

    @property
    def mtu_size(self):
        if self._mtu is None:
            raise BleakError("no MTU on this backend")
        return self._mtu

    async def write_gatt_char(self, uuid, data, response=False):
        self.writes.append(data)


def _session(mtu=None):
    return mod.Session(_StubClient(mtu))


def test_write_limit_is_mtu_minus_three():
    assert _session(512).write_limit() == 509


def test_write_limit_falls_back_when_backend_has_no_mtu():
    assert _session().write_limit() == 180


def _usage_payload(**extra):
    p = {"s": 60, "sr": 261, "w": 55, "wr": 1101, "st": "allowed",
         "acct": "pro", "ok": True, "c": 1, "t": 1785408238, "tf": 24,
         "q": [{"n": "AMZN", "p": "$237.31", "d": 4.7},
               {"n": "SPCX", "p": "$112.23", "d": -0.28}]}
    p.update(extra)
    return p


LONG_QUOTE = {"t": "Any fool can write code that a computer can understand. Good "
                   "programmers write code that humans can understand, which is the "
                   "only audience that has ever really mattered.",
              "a": "Martin Fowler"}


def test_write_sends_one_write_when_everything_fits():
    s = _session(512)
    payload = _usage_payload(qd=dict(LONG_QUOTE))
    assert _run(s.write_payload(payload))
    assert len(s.client.writes) == 1
    assert json.loads(s.client.writes[0])["qd"] == LONG_QUOTE


def test_write_splits_instead_of_truncating_when_over_the_limit():
    """The quote must arrive whole: the usage write carries a sentinel and the
    quote follows in its own write, rather than being cut to fit."""
    s = _session(256)   # 253 usable — usage + two tickers + a long quote won't fit
    payload = _usage_payload(qd=dict(LONG_QUOTE))
    assert _run(s.write_payload(payload))
    assert len(s.client.writes) == 2
    first, second = (json.loads(w) for w in s.client.writes)
    assert first["qd"] == 1              # "keep your quote, update follows"
    assert first["s"] == 60              # usage untouched
    assert second == {"qd": LONG_QUOTE}  # verbatim, not shortened
    for w in s.client.writes:
        assert len(w) <= 253


def test_write_never_shortens_the_quote_text():
    s = _session(120)
    payload = _usage_payload(qd=dict(LONG_QUOTE))
    _run(s.write_payload(payload))
    for w in s.client.writes:
        body = json.loads(w)
        qd = body.get("qd")
        if isinstance(qd, dict):
            assert qd["t"] == LONG_QUOTE["t"]
        assert "..." not in w.decode()


def test_write_drops_the_quote_only_when_it_cannot_fit_alone():
    s = _session(60)    # 57 usable — even the quote on its own is too big
    payload = _usage_payload(qd=dict(LONG_QUOTE))
    _run(s.write_payload(payload))
    assert len(s.client.writes) == 1
    assert "qd" not in json.loads(s.client.writes[0])


def test_write_leaves_the_cached_quote_untouched(tmp_path, monkeypatch):
    """The writer may rewrite payload["qd"] to a sentinel; the pool entry it came
    from must survive intact, or the next poll would send a mutilated quote."""
    _enable_quotes(tmp_path, monkeypatch)
    monkeypatch.setattr(mod, "_qod_pool", [dict(LONG_QUOTE)])
    monkeypatch.setattr(mod, "_qod_pool_at", time.time())
    s = _session(256)
    for _ in range(2):
        payload = _usage_payload()
        _run(mod.add_quote_of_day_field(payload))
        _run(s.write_payload(payload))
    last = json.loads(s.client.writes[-1])
    assert last == {"qd": LONG_QUOTE}
