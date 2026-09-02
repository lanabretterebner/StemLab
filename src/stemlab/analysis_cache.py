"""Local-only cache for source analysis."""

from __future__ import annotations

import hashlib
import json
import sqlite3
import time
from contextlib import contextmanager
from collections.abc import Callable, Iterator, Mapping
from pathlib import Path
from typing import Any

from .paths import analysis_dir
from .runtime import CancellationToken

_SCHEMA_VERSION = 2


def managed_analysis_dir() -> Path:
    """Return StemLab's private per-user analysis directory.

    The location lives in stemlab.paths now, with the recursive weights it
    used to share ``~/.stemlab`` with. Kept as a name here because it is what
    the rest of this module and its tests call.
    """
    return analysis_dir()


def source_identity(
    path: str | Path,
    cancellation: CancellationToken | None = None,
    progress: Callable[[float], None] | None = None,
) -> str:
    """Hash an audio file without loading it all into memory."""
    path = Path(path).expanduser().resolve()
    total = max(1, path.stat().st_size)
    consumed = 0
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            if cancellation:
                cancellation.raise_if_cancelled()
            digest.update(chunk)
            consumed += len(chunk)
            if progress:
                progress(min(1.0, consumed / total))
    return digest.hexdigest()


def settings_identity(settings: Mapping[str, Any]) -> str:
    """Return a stable cache key for analysis settings."""
    encoded = json.dumps(settings, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


class AnalysisCache:
    """Small SQLite store that never leaves the user's computer."""

    def __init__(self, path: str | Path | None = None) -> None:
        self.path = Path(path) if path is not None else managed_analysis_dir() / "analysis.sqlite3"
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._create_schema()

    @contextmanager
    def _connect(self) -> Iterator[sqlite3.Connection]:
        # No "PRAGMA foreign_keys = ON" here any more. Neither table declares
        # a foreign key, so it enforced nothing, and it ran before the try
        # below - a connection that failed on it was left open rather than
        # closed. It belongs back the moment a relation between the tables
        # does, inside the guard.
        connection = sqlite3.connect(self.path, timeout=10.0)
        try:
            with connection:
                yield connection
        finally:
            connection.close()

    def _create_schema(self) -> None:
        with self._connect() as connection:
            connection.executescript(
                """
                CREATE TABLE IF NOT EXISTS analysis_results (
                    kind TEXT NOT NULL,
                    source_hash TEXT NOT NULL,
                    algorithm_version TEXT NOT NULL,
                    settings_hash TEXT NOT NULL,
                    result_json TEXT NOT NULL,
                    created_at REAL NOT NULL,
                    PRIMARY KEY (kind, source_hash, algorithm_version, settings_hash)
                );

                CREATE TABLE IF NOT EXISTS cache_metadata (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
                """
            )
            # Schema 1 carried a "corrections" table of manual BPM/key
            # overrides. Nothing writes or reads them any more, so it is
            # dropped rather than left behind: a stale row in it used to
            # override the analysis on every run of that file, and a store
            # nothing can edit is worse than no store.
            connection.execute("DROP TABLE IF EXISTS corrections")
            connection.execute(
                "INSERT OR REPLACE INTO cache_metadata(key, value) VALUES ('schema', ?)",
                (str(_SCHEMA_VERSION),),
            )

    def get_result(
        self,
        kind: str,
        source_hash: str,
        algorithm_version: str,
        settings: Mapping[str, Any],
    ) -> dict[str, Any] | None:
        settings_hash = settings_identity(settings)
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT result_json FROM analysis_results
                WHERE kind = ? AND source_hash = ? AND algorithm_version = ? AND settings_hash = ?
                """,
                (kind, source_hash, algorithm_version, settings_hash),
            ).fetchone()
        return json.loads(row[0]) if row else None

    def put_result(
        self,
        kind: str,
        source_hash: str,
        algorithm_version: str,
        settings: Mapping[str, Any],
        result: Mapping[str, Any],
    ) -> None:
        settings_hash = settings_identity(settings)
        payload = json.dumps(result, sort_keys=True, separators=(",", ":"))
        with self._connect() as connection:
            connection.execute(
                """
                INSERT OR REPLACE INTO analysis_results
                    (kind, source_hash, algorithm_version, settings_hash, result_json, created_at)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (kind, source_hash, algorithm_version, settings_hash, payload, time.time()),
            )

    def clear(self) -> int:
        """Clear cached analyses while retaining the schema."""
        with self._connect() as connection:
            result_count = connection.execute("SELECT COUNT(*) FROM analysis_results").fetchone()[0]
            connection.execute("DELETE FROM analysis_results")
        return int(result_count)


def cleanup_stale_midi_drag_files(max_age_days: int = 7) -> int:
    """Delete only old temporary MIDI files in StemLab's managed drag directory."""
    directory = managed_analysis_dir() / "MidiDrag"
    if not directory.is_dir():
        return 0

    cutoff = time.time() - max(1, max_age_days) * 24 * 60 * 60
    removed = 0
    for path in directory.glob("*.mid"):
        try:
            if path.is_file() and path.stat().st_mtime < cutoff:
                path.unlink()
                removed += 1
        except OSError:
            continue
    return removed
