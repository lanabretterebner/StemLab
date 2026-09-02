"""Tests for the Beat This! first-use download.

The release bundles ship the Engine, not the weights, so this is the code
path that puts a model on disk for the first time. Everything it fetches is
untrusted until it hashes to the recorded value.
"""

from __future__ import annotations

import hashlib
import http.server
import threading
from pathlib import Path

import pytest

from stemlab.beat_tracking import (
    ModelSpec,
    download_packaged_model,
    ensure_packaged_model,
    resolve_packaged_model,
)

PAYLOAD = b"a plausible checkpoint" * 500
# Same length, different bytes: the size check cannot catch this one, so it
# is the digest or nothing.
SUBSTITUTED = b"a plausible checkpoinX" * 500


def serve(body: bytes):
    """Serve one fixed body on localhost, returning (url, shutdown)."""

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802 - http.server's required spelling
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    # serve_forever polls every 0.5 s by default and shutdown() blocks until
    # that loop next wakes, so every server here used to cost half a second of
    # the suite waiting for nothing. The interval only decides how often an
    # idle loop looks at the stop flag.
    threading.Thread(
        target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True
    ).start()
    host, port = server.server_address

    def shutdown() -> None:
        server.shutdown()
        # shutdown() stops the loop but leaves the listening socket open, and
        # a test file that starts one server per test would hold every one of
        # them until the interpreter exits.
        server.server_close()

    return f"http://{host}:{port}/model.ckpt", shutdown


@pytest.fixture
def hosted():
    shutdowns = []

    def start(body: bytes):
        url, shutdown = serve(body)
        shutdowns.append(shutdown)
        return url

    yield start
    for shutdown in shutdowns:
        shutdown()


def spec_for(body: bytes, url: str, *, size: int | None = None, sha256: str | None = None):
    return ModelSpec(
        "small0",
        len(body) if size is None else size,
        hashlib.sha256(body).hexdigest() if sha256 is None else sha256,
        url,
    )


class TestDownload:
    def test_a_verified_download_lands_on_disk(self, tmp_path: Path, hosted, monkeypatch):
        url = hosted(PAYLOAD)
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS", {"fast": spec_for(PAYLOAD, url)}
        )

        path = download_packaged_model("fast", tmp_path)

        assert path == tmp_path / "small0.ckpt"
        assert path.read_bytes() == PAYLOAD
        # And the result is immediately usable by the normal resolver.
        assert resolve_packaged_model("fast", tmp_path) == path.resolve()

    def test_a_substituted_body_is_rejected(self, tmp_path: Path, hosted, monkeypatch):
        # The server returns something other than what the spec pins. This is
        # the case the digest exists for, so it must not reach disk.
        url = hosted(SUBSTITUTED)
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS", {"fast": spec_for(PAYLOAD, url)}
        )

        with pytest.raises(RuntimeError, match="SHA-256"):
            download_packaged_model("fast", tmp_path)

        assert not (tmp_path / "small0.ckpt").exists()

    def test_a_non_http_url_is_refused(self, tmp_path: Path, monkeypatch):
        # urlopen would read this straight off the disk and call it a
        # download, landing unvetted local bytes in the model directory.
        planted = tmp_path / "planted.bin"
        planted.write_bytes(PAYLOAD)
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS",
            {"fast": spec_for(PAYLOAD, planted.as_uri())},
        )

        with pytest.raises(ValueError, match="http"):
            download_packaged_model("fast", tmp_path)

        assert not (tmp_path / "small0.ckpt").exists()

    def test_a_truncated_body_is_rejected(self, tmp_path: Path, hosted, monkeypatch):
        url = hosted(PAYLOAD)
        # The spec expects more than the server will send.
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS",
            {"fast": spec_for(PAYLOAD, url, size=len(PAYLOAD) + 1024)},
        )

        with pytest.raises(RuntimeError, match="bytes, expected"):
            download_packaged_model("fast", tmp_path)

        assert not (tmp_path / "small0.ckpt").exists()

    def test_a_rejected_download_leaves_nothing_behind(
        self, tmp_path: Path, hosted, monkeypatch
    ):
        # A stray .partial would be found by the next run and rejected on its
        # size, which reads as a corrupt install rather than a failed fetch.
        url = hosted(SUBSTITUTED)
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS", {"fast": spec_for(PAYLOAD, url)}
        )

        with pytest.raises(RuntimeError):
            download_packaged_model("fast", tmp_path)

        assert list(tmp_path.iterdir()) == []

    def test_progress_is_a_fraction_and_names_the_download(
        self, tmp_path: Path, hosted, monkeypatch
    ):
        url = hosted(PAYLOAD)
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS", {"fast": spec_for(PAYLOAD, url)}
        )

        seen: list[tuple[float, str]] = []
        download_packaged_model(
            "fast", tmp_path, progress=lambda value, stage: seen.append((value, stage))
        )

        assert seen, "the download reported no progress at all"
        fractions = [value for value, _stage in seen]
        # analyse_beats scales these into its own band, so anything above 1.0
        # would throw the plugin's bar off the end.
        assert all(0.0 <= value <= 1.0 for value in fractions)
        assert fractions == sorted(fractions)
        assert all("Downloading the Beat This!" in stage for _value, stage in seen)
        assert seen[-1][1].endswith("(100%)")


class TestEnsure:
    def test_an_existing_checkpoint_is_not_redownloaded(
        self, tmp_path: Path, monkeypatch
    ):
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS",
            {"fast": spec_for(PAYLOAD, "http://127.0.0.1:1/never.ckpt")},
        )
        (tmp_path / "small0.ckpt").write_bytes(PAYLOAD)

        # The url points nowhere; reaching for it would raise rather than
        # return, so this passing is the assertion.
        assert ensure_packaged_model("fast", tmp_path) == (tmp_path / "small0.ckpt")

    def test_a_missing_checkpoint_is_fetched(self, tmp_path: Path, hosted, monkeypatch):
        url = hosted(PAYLOAD)
        monkeypatch.setattr(
            "stemlab.beat_tracking.MODEL_SPECS", {"fast": spec_for(PAYLOAD, url)}
        )

        assert not (tmp_path / "small0.ckpt").exists()
        path = ensure_packaged_model("fast", tmp_path)
        assert path.read_bytes() == PAYLOAD
