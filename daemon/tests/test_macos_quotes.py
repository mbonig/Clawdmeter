#!/usr/bin/env python3
"""Unit tests for the macOS/Linux daemon's market-quote support.

Covers read_tickers_setting, fetch_quote's wire form, and add_quote_fields'
caching / degradation behaviour (the fields feeding the device's center rotator).

Run: python -m pytest daemon/tests/test_macos_quotes.py -x -q
"""
import asyncio
import json
from unittest.mock import AsyncMock, patch

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
