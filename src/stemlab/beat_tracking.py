"""Offline Beat This! inference and robust musical-time interpretation."""

from __future__ import annotations

import hashlib
import json
import os
import sys
import urllib.parse
import urllib.request
from collections import Counter
from collections.abc import Callable, Mapping
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

import numpy as np

from .runtime import CancellationToken, JobCancelled

if TYPE_CHECKING:
    import torch

# torch is imported inside the functions that run inference so that importing
# this module for its constants (BEAT_ALGORITHM_VERSION, MODEL_SPECS,
# BeatAnalysis) stays cheap: cache-hit analyses and the sqlite-only CLI paths
# must never pay for, or require, torch.

BEAT_THIS_VERSION = "1.1.0"
# Part of the analysis cache's primary key (kind, source_hash,
# algorithm_version, settings_hash), so it has to move whenever the numbers
# this module derives change. -2 is the tempo coming from the mean of the
# inlier intervals rather than their median; without the bump every track
# already analysed would keep serving the frame-quantised BPM it cached.
BEAT_ALGORITHM_VERSION = "beat-this-1.1.0-stemlab-6"

# Beat This!'s frame rate, and so the grid every beat it reports lands on.
_BEAT_FPS = 50.0


@dataclass(frozen=True)
class ModelSpec:
    name: str
    size: int
    sha256: str
    url: str


# The size and digest are the whole security story for these files: they are
# fetched over the network on first use, so nothing is trusted until it
# hashes to the value recorded here.
_BEAT_THIS_HOST = "https://cloud.cp.jku.at/public.php/dav/files/7ik4RrBKTS273gp"

MODEL_SPECS = {
    "fast": ModelSpec(
        "small0",
        8_451_101,
        "6074be2c4d490c5f6101fcc374a1ec72ae93456e23bb6019783b849f5dc7d47b",
        f"{_BEAT_THIS_HOST}/small0.ckpt",
    ),
    "accurate": ModelSpec(
        "final0",
        81_058_141,
        "8c328b45f59d8dd3dff219253ff6a8d6482be57d0133a29140e2febbf8eb8331",
        f"{_BEAT_THIS_HOST}/final0.ckpt",
    ),
}


@dataclass(frozen=True)
class BeatAnalysis:
    bpm: float
    detected_bpm: float
    half_time_bpm: float
    double_time_bpm: float
    beats: tuple[float, ...]
    downbeats: tuple[float, ...]
    meter_numerator: int | None
    meter_denominator: int
    bar_one: float
    confidence: float
    model: str
    model_version: str
    device: str
    # How well one constant tempo explains the whole track: the RMS distance
    # from every beat to the fitted grid, in seconds, and the fraction of
    # beats that grid explains. A track produced to a click sits at the
    # detector's own 20 ms quantisation floor (5.8 ms RMS, ratio near 1.0).
    # A played or drifting one does not, and no single host tempo will hold
    # alignment across it however well the tempo itself is measured.
    grid_rms: float = 0.0
    grid_ratio: float = 0.0
    # Every stretch one constant tempo explains, in order. One entry means
    # the track holds a single tempo; more than one means it does not, and
    # names where each holds.
    tempo_segments: tuple[TempoSegment, ...] = ()

    @property
    def tempo_is_steady(self) -> bool:
        """Whether one tempo holds the track well enough to set a host to it.

        The thresholds are the detector's own floor and a little slack. A
        track cut to a click sits at 5.8 ms RMS - 20 ms quantisation over
        sqrt(12) - with the grid explaining every beat; 12 ms is twice that,
        which nothing steady reaches. The 0.80 ratio is what separates a few
        loose beats at a fade-in from a tempo that moves: a track ramping
        1 BPM across its length explains 0.55 of its beats and no constant
        tempo will align it, however precisely the tempo is measured.
        """
        return self.grid_rms <= 0.012 and self.grid_ratio >= 0.80

    def to_dict(self) -> dict[str, object]:
        return asdict(self)

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "BeatAnalysis":
        """Rebuild an analysis from what ``to_dict`` wrote and the cache stored.

        The inverse is not ``cls(**payload)``. ``asdict`` flattens the tempo
        segments into plain dicts and the cache round-trips everything through
        JSON, which turns every tuple into a list - so an analysis rebuilt
        naively handed its callers dicts where they read ``segment.start``,
        and the second analysis of any track long enough to have a segment
        (sixteen beats is enough) died on it while the first succeeded.

        Segments that are already ``TempoSegment`` instances are passed
        through, so a caller holding an unserialised payload gets the same
        answer as one reading the cache.
        """
        fields = dict(payload)
        segments = fields.get("tempo_segments") or ()

        return cls(
            **{
                **fields,
                "beats": tuple(fields.get("beats") or ()),
                "downbeats": tuple(fields.get("downbeats") or ()),
                "tempo_segments": tuple(
                    segment if isinstance(segment, TempoSegment) else TempoSegment(**segment)
                    for segment in segments
                ),
            }
        )


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


# What a completed digest check is remembered as, beside the checkpoint it
# covers. Hashing a multi-hundred-megabyte file is slow enough to be felt at
# the start of every run, and the file does not change between them. This is
# a record of work already done, never a substitute for it: the size and
# mtime it was taken over must still hold, and the digest it names must be
# the one the spec pins, or the checkpoint goes back through the hash.
_VERIFICATION_SUFFIX = ".verified.json"


def _verification_path(checkpoint: Path) -> Path:
    return checkpoint.with_name(checkpoint.name + _VERIFICATION_SUFFIX)


def _already_verified(checkpoint: Path, info: os.stat_result, expected: str) -> bool:
    """Has this exact file already hashed to the digest the spec pins?"""
    try:
        record = json.loads(_verification_path(checkpoint).read_text(encoding="utf-8"))
        return (
            str(record["sha256"]).lower() == expected.lower()
            and int(record["size"]) == info.st_size
            and int(record["mtime_ns"]) == info.st_mtime_ns
        )
    except (OSError, ValueError, TypeError, KeyError):
        # No record, an unreadable one, or one that does not say what it must
        # say. All of them mean the same thing: this file is unverified.
        return False


def _record_verification(checkpoint: Path, info: os.stat_result, digest: str) -> None:
    """Note that ``checkpoint``, as ``info`` describes it, hashed to ``digest``.

    ``info`` is deliberately the stat taken before the digest rather than
    after: a checkpoint rewritten while it was being read carries a newer
    mtime than this, and is hashed again rather than accepted on a digest
    taken over bytes that no longer exist.
    """
    destination = _verification_path(checkpoint)
    temporary = destination.with_name(f"{destination.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(
            json.dumps(
                {
                    "sha256": digest.lower(),
                    "size": info.st_size,
                    "mtime_ns": info.st_mtime_ns,
                }
            ),
            encoding="utf-8",
        )
        os.replace(temporary, destination)
    except OSError:
        # A model directory that will not take the record costs later runs
        # the hash they would have skipped. It must not cost this one the
        # model, which is verified either way.
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _candidate_model_directories() -> list[Path]:
    candidates: list[Path] = []
    override = os.environ.get("STEMLAB_BEAT_THIS_MODEL_DIR")
    if override:
        candidates.append(Path(override).expanduser())

    executable_dir = Path(sys.executable).resolve().parent
    candidates.extend(
        (
            executable_dir / "Models" / "BeatThis",
            Path(sys.prefix).resolve() / "Models" / "BeatThis",
            Path(__file__).resolve().parents[1] / "models" / "beat_this",
        )
    )

    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates.append(Path(local) / "StemLab" / "Models" / "BeatThis")
    return candidates


def resolve_packaged_model(mode: str, model_dir: str | Path | None = None) -> Path:
    """Resolve and validate a packaged checkpoint without any network fallback."""
    if mode not in MODEL_SPECS:
        raise ValueError(f"Unknown Beat This! mode: {mode}")
    spec = MODEL_SPECS[mode]
    directories = [Path(model_dir).expanduser()] if model_dir else _candidate_model_directories()

    checked: list[str] = []
    for directory in directories:
        path = directory.resolve() / f"{spec.name}.ckpt"
        checked.append(str(path))
        if not path.is_file():
            continue
        info = path.stat()
        if info.st_size != spec.size:
            raise RuntimeError(f"Packaged Beat This! model has the wrong size: {path}")
        if not _already_verified(path, info, spec.sha256):
            digest = _sha256_file(path)
            if digest.lower() != spec.sha256:
                raise RuntimeError(f"Packaged Beat This! model failed SHA-256 validation: {path}")
            _record_verification(path, info, digest)
        return path

    locations = "\n  ".join(checked)
    raise FileNotFoundError(
        f"The packaged Beat This! {spec.name} model is missing. Checked:\n  {locations}"
    )


def _writable_model_directory() -> Path:
    """Pick where a downloaded checkpoint should land.

    The same preference order resolve_packaged_model searches, so whatever
    this writes is what that finds. In a portable bundle the first writable
    candidate is the Engine's own Models directory, which keeps the bundle
    self-contained after the first run; on a system-wide install it falls
    through to the per-user cache.
    """
    failures: list[str] = []

    for directory in _candidate_model_directories():
        try:
            directory.mkdir(parents=True, exist_ok=True)
            probe = directory / ".stemlab-write-test"
            probe.touch()
            probe.unlink()
        except OSError as exc:
            failures.append(f"{directory}: {exc}")
            continue
        return directory.resolve()

    locations = "\n  ".join(failures) or "no candidate directories"
    raise RuntimeError(
        f"Nowhere to store the Beat This! model. Tried:\n  {locations}"
    )


def download_packaged_model(
    mode: str,
    model_dir: str | Path | None = None,
    *,
    progress: Callable[[float, str], None] | None = None,
    cancellation: CancellationToken | None = None,
) -> Path:
    """Fetch a Beat This! checkpoint and verify it before it is usable.

    ``progress`` receives the fraction of the download completed, 0.0 to
    1.0, matching what the rest of this module reports rather than the
    0-100 the model CLIs use.

    The release bundles ship the Engine, not the weights, so the first run
    with Beat This! enabled downloads them. Nothing about the response is
    trusted: the file is written beside its destination and only moved into
    place once its length and SHA-256 match the recorded spec, so a
    truncated or substituted download cannot become the model that loads.
    """
    if mode not in MODEL_SPECS:
        raise ValueError(f"Unknown Beat This! mode: {mode}")

    spec = MODEL_SPECS[mode]

    # urlopen is happy to read file:// and hand the bytes back as though
    # they had been downloaded. The urls here are module constants, so this
    # is belt and braces - but it is the difference between a bad spec being
    # a failed download and a bad spec pulling something off the local disk
    # into the model directory. The transport is not what makes this safe;
    # the digest below is, which is why plain http is not refused outright.
    if urllib.parse.urlparse(spec.url).scheme not in ("http", "https"):
        raise ValueError(
            f"Beat This! {spec.name} model url must be http(s): {spec.url}"
        )

    directory = Path(model_dir).expanduser().resolve() if model_dir else _writable_model_directory()
    directory.mkdir(parents=True, exist_ok=True)

    destination = directory / f"{spec.name}.ckpt"
    # Per-process, because two downloads of one checkpoint would otherwise
    # open the same .partial "wb" and interleave into a corrupt file that
    # then gets replace()d into place as if it were whole.
    partial = destination.with_suffix(".ckpt.partial.%d" % os.getpid())
    token = cancellation or CancellationToken()

    if progress:
        progress(0.0, f"Downloading the Beat This! {spec.name} model (0%)")

    digest = hashlib.sha256()
    received = 0
    last_reported = -1

    try:
        with urllib.request.urlopen(spec.url, timeout=60) as response, \
                partial.open("wb") as handle:
            while True:
                token.raise_if_cancelled()
                chunk = response.read(1 << 20)
                if not chunk:
                    break

                handle.write(chunk)
                digest.update(chunk)
                received += len(chunk)

                if progress and spec.size:
                    fraction = min(1.0, received / spec.size)
                    percent = int(fraction * 100.0)
                    # Every report crosses a pipe into the plugin, and a
                    # chunked read redraws far faster than the number moves.
                    if percent != last_reported:
                        last_reported = percent
                        progress(
                            fraction,
                            f"Downloading the Beat This! {spec.name} model ({percent}%)",
                        )

        if received != spec.size:
            raise RuntimeError(
                f"Beat This! {spec.name} download is {received} bytes, expected {spec.size}"
            )

        actual = digest.hexdigest().lower()
        if actual != spec.sha256:
            raise RuntimeError(
                f"Beat This! {spec.name} download failed SHA-256 validation "
                f"(got {actual}, expected {spec.sha256})"
            )

        partial.replace(destination)

        # These bytes were hashed on the way in, so the resolution that
        # follows this download has nothing left to check.
        try:
            _record_verification(destination, destination.stat(), actual)
        except OSError:
            pass
    except BaseException:
        # A half-written file left behind would be found by the next run and
        # rejected on its size, which reads as a corrupt install rather than
        # an interrupted download.
        partial.unlink(missing_ok=True)
        raise

    return destination


def ensure_packaged_model(
    mode: str,
    model_dir: str | Path | None = None,
    *,
    progress: Callable[[float, str], None] | None = None,
    cancellation: CancellationToken | None = None,
) -> Path:
    """Return a validated checkpoint, downloading it the first time."""
    try:
        return resolve_packaged_model(mode, model_dir)
    except FileNotFoundError:
        return download_packaged_model(
            mode, model_dir, progress=progress, cancellation=cancellation
        )


def choose_device(requested: str = "auto") -> torch.device:
    """Use CUDA when available and otherwise return a safe CPU device."""
    import torch

    requested = requested.strip().lower()
    if requested in {"", "auto"}:
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    if requested.startswith("cuda") and not torch.cuda.is_available():
        return torch.device("cpu")
    return torch.device(requested)


def _load_model(path: Path, device: torch.device) -> torch.nn.Module:
    """Load one Beat This! checkpoint onto a device.

    There is deliberately no process-wide cache behind this. One used to sit
    here, keyed on path and device, and it could never serve a hit: every
    analysis runs in its own worker process and calls analyse_beats once, and
    the only second call is the CUDA-to-CPU fallback, which asks for a
    different device and so misses by construction. All it did was hold a
    model that had already been used, in a process about to exit.
    """
    from beat_this.inference import load_model

    # The validated absolute path is deliberate: passing a short name would
    # make upstream Beat This! attempt a network download.
    return load_model(str(path), device=device)


def _robust_intervals(beats: np.ndarray) -> tuple[np.ndarray, float, float]:
    intervals = np.diff(np.asarray(beats, dtype=np.float64))
    intervals = intervals[np.isfinite(intervals) & (intervals >= 0.18) & (intervals <= 3.0)]
    if intervals.size < 2:
        raise RuntimeError("Beat This! did not detect enough stable beats")

    median = float(np.median(intervals))
    deviations = np.abs(intervals - median)
    mad = float(np.median(deviations))
    tolerance = max(0.035, median * 0.18, 3.5 * mad)
    inliers = intervals[deviations <= tolerance]
    if inliers.size < 2:
        inliers = intervals

    interval = float(np.median(inliers))
    robust_cv = float(np.median(np.abs(inliers - interval)) / max(interval, 1.0e-9))
    inlier_ratio = float(inliers.size / intervals.size)
    return inliers, robust_cv, inlier_ratio


def _sub_frame_events(events: np.ndarray, logits: np.ndarray) -> np.ndarray:
    """Move each event off the 20 ms frame grid, using the shape of its peak.

    50 fps is Beat This!'s own frame rate - the spectrogram hop it was
    trained on - so it cannot be raised without retraining the model, and
    every beat its postprocessor reports is an integer frame divided by it
    (postprocessor.py: beat_time = beat_frame / self.fps). That is 20 ms of
    quantisation, 882 samples at 44.1 kHz, on every beat.

    The frames are quantised; the network's output is not. It emits a logit
    per frame, and around a beat those form a peak whose centre lies between
    frames when the beat does. Fitting a parabola through the peak frame and
    its two neighbours recovers where that centre actually is.

    The fit is done on the logits rather than on the probabilities they
    become: a bump that is Gaussian in probability is exactly a parabola in
    log-odds, so this is the domain the three-point fit is least wrong in.

    A frame that is not a local maximum has no peak to interpolate - the
    curvature term comes out flat or convex - and is left where it is, as
    are events on the first and last frame, which have no neighbour on one
    side.

    Takes the logits as a plain array rather than a tensor: this module is
    imported for its constants by paths that must not pay for torch, and
    three-point interpolation is not a reason to break that.
    """
    values = np.asarray(logits, dtype=np.float64)
    events = np.asarray(events, dtype=np.float64)

    if values.size < 3 or events.size == 0:
        return events

    frames = np.rint(events * _BEAT_FPS).astype(int)
    interior = (frames >= 1) & (frames <= values.size - 2)
    refined = frames.astype(np.float64)

    if np.any(interior):
        centre = frames[interior]
        left, middle, right = (
            values[centre - 1],
            values[centre],
            values[centre + 1],
        )
        curvature = left - 2.0 * middle + right
        peak = curvature < -1.0e-9
        offset = np.zeros(centre.shape, dtype=np.float64)
        offset[peak] = 0.5 * (left[peak] - right[peak]) / curvature[peak]
        # A parabola through three samples cannot put its vertex outside the
        # middle one; anything that says otherwise is noise, not a peak.
        offset = np.clip(offset, -0.5, 0.5)
        refined[interior] = centre + offset

    return refined / _BEAT_FPS


# Half a frame either side of a beat, plus a frame of slack for a detector
# that is a little late on one. Wide enough to keep the beats a grid really
# does explain, narrow enough that a grid one beat out of phase keeps none.
_GRID_TOLERANCE = 1.5 / 50.0


@dataclass(frozen=True)
class _Grid:
    """One constant tempo laid over the whole track."""

    period: float
    phase: float
    rms: float
    ratio: float


def _fit_grid(beats: np.ndarray, period: float, phase: float) -> _Grid:
    relative = beats - beats[0]
    slot = np.round((relative - phase) / period)
    residual = relative - (phase + period * slot)
    inliers = np.abs(residual) <= _GRID_TOLERANCE

    if int(inliers.sum()) >= 8 and np.unique(slot[inliers]).size >= 2:
        fitted = np.polyfit(slot[inliers], relative[inliers], 1)
        if 0.9 * period < float(fitted[0]) < 1.1 * period:
            period, phase = float(fitted[0]), float(fitted[1])
            slot = np.round((relative - phase) / period)
            residual = relative - (phase + period * slot)
            inliers = np.abs(residual) <= _GRID_TOLERANCE

    kept = residual[inliers]
    rms = float(np.sqrt(np.mean(kept**2))) if kept.size else float("inf")
    return _Grid(period, phase, rms, float(inliers.sum() / max(beats.size, 1)))


def _dominant_grid(beats: np.ndarray, seed: float) -> _Grid:
    """The tempo that explains the most beats end to end.

    Interval statistics cannot find this, however they are averaged. A track
    that runs 16 s at 170 before settling at 174 shifts each of those early
    intervals by 8 ms - less than the 20 ms Beat This! quantises to, and far
    inside the tolerance _robust_intervals allows (18% of the period). The
    two tempos are indistinguishable one beat at a time, so the intro is
    averaged in and a 174 track reads 173.73. Measured: 16 s of 170 gave
    173.73, 24 s gave 173.59, 16 s of 172 gave 173.87.

    Over distance they separate completely: across a 16 s intro a 170 grid
    and a 174 grid walk more than a whole beat apart. So the tempo is chosen
    by how many beats a grid explains from the first to the last, and the
    intro simply fails to be explained by the one that wins. The same
    property handles dropped beats (a gap in the slots, which costs nothing)
    and spurious ones (a slot that no grid explains).

    The sweep is +/-3% of the interval mean, which is where a contaminated
    seed can land while the real tempo is still in range, at a resolution of
    0.01% - two orders below the smallest tempo difference worth reporting.
    """
    if beats.size < 8:
        return _fit_grid(beats, seed, 0.0)

    relative = beats - beats[0]
    best: _Grid | None = None

    for factor in np.linspace(0.97, 1.03, 601):
        period = seed * factor
        slot = np.round(relative / period)
        # The phase every candidate deserves: a grid is only as good as its
        # best alignment, so it is offered the offset most beats agree on
        # rather than being pinned to the first one.
        phase = float(np.median(relative - period * slot))
        residual = relative - (phase + period * slot)
        inliers = np.abs(residual) <= _GRID_TOLERANCE
        count = int(inliers.sum())

        if best is None or count > best.ratio * beats.size:
            best = _Grid(period, phase, 0.0, count / beats.size)

    assert best is not None
    return _fit_grid(beats, best.period, best.phase)


# The largest amount a reported tempo is ever moved to reach a round one.
# 0.05 BPM at 174 is 0.4 ms across a bar - below anything a listener or a
# producer separates - so a difference bigger than this is the track's, not
# the fit's, and is reported as it was measured.
_TEMPO_SNAP_LIMIT = 0.05

# ...and the round tempo has to stay inside one frame of the fitted one from
# the first beat to the last. The beats arrive on a 20 ms grid, so two
# tempos whose grids never walk a frame apart across the whole track are not
# tempos these beats can tell apart. Long tracks constrain the fit harder and
# so snap less, which is the right way round.
_TEMPO_DRIFT_BUDGET = 1.0 / _BEAT_FPS


def _snap_tempo(grid: _Grid, beats: np.ndarray) -> _Grid:
    """Report a round tempo where the beats cannot distinguish it from the fit.

    Tempo is chosen, not measured: a producer sets 174 and the machine plays
    174. What comes back here is a least-squares slope through several
    hundred beat times that each carry a few milliseconds of the network's
    own error, and that slope lands a hundredth of a BPM off a round number
    often enough to matter - a 174 track reading 173.99.

    A hundredth of a BPM is not a display problem. The plugin draws its grid
    as bar one plus a beat period, so a tempo read 0.01 low lays every line
    19 us late, and by the end of a four minute track the grid sits 14 ms
    behind the music: exactly the "slightly delayed" a listener notices at
    the end of a track and not at the start.

    So the round tempo is preferred where the beats cannot argue with it -
    where its grid stays inside a frame of the fitted one end to end - and
    the phase is refitted at the round period, which keeps the error centred
    over the track instead of piling it all at one end. Neither gate is a
    round-off: a track really playing 173.5 walks two thirds of a second away
    from a 174 grid over four minutes, and keeps the tempo it was fitted.
    """
    if beats.size < 8 or not (grid.period > 0.0):
        return grid

    fitted_bpm = 60.0 / grid.period
    candidate = float(np.round(fitted_bpm))
    difference = abs(candidate - fitted_bpm)

    if candidate <= 0.0 or difference == 0.0 or difference > _TEMPO_SNAP_LIMIT:
        return grid

    span = float(beats[-1] - beats[0])
    if span <= 0.0 or span * difference / fitted_bpm > _TEMPO_DRIFT_BUDGET:
        return grid

    # The same beats in the same slots at a different period. The snap is a
    # reading of one grid, not a second chance to re-count the track: which
    # beats the grid explains was settled by the fit, and the drift budget
    # keeps every slot inside half a frame of where the fit put it.
    relative = beats - beats[0]
    fitted_slot = np.round((relative - grid.phase) / grid.period)
    explained = (
        np.abs(relative - (grid.phase + grid.period * fitted_slot)) <= _GRID_TOLERANCE
    )
    if int(explained.sum()) < 8:
        return grid

    period = 60.0 / candidate
    offsets = relative - period * np.round((relative - grid.phase) / period)

    # Refitted, not carried over: the round period's best phase centres the
    # remaining difference over the track rather than leaving it to pile up
    # at the far end, which is the half of this that the grid is drawn from.
    phase = float(np.mean(offsets[explained]))
    residual = offsets[explained] - phase
    rms = float(np.sqrt(np.mean(residual**2)))

    if rms > _GRID_TOLERANCE:
        return grid

    return _Grid(period, phase, rms, grid.ratio)


def _snap_to_grid(seconds: float, grid: _Grid, origin: float) -> float:
    """The grid slot nearest a reported beat, in source seconds."""
    relative = seconds - origin
    slot = round((relative - grid.phase) / grid.period)
    return float(origin + grid.phase + grid.period * slot)


@dataclass(frozen=True)
class TempoSegment:
    """A stretch of the track that one constant tempo does explain."""

    start: float
    end: float
    bpm: float
    beats: int


# A tempo has to hold for this many beats before it is a section rather than
# a stumble. Two bars of four.
_MIN_SEGMENT_BEATS = 8

# Local period estimates disagree by more than this, and the track changed
# tempo rather than the detector wobbling. 1.2% is four times the spread a
# clean track shows at 174 BPM, where 20 ms quantisation over an 8-beat
# window is 0.3%.
_SEGMENT_THRESHOLD = 0.012

# Shorter than this and it is the window crossing a change, not a section
# of music. Four seconds is under two bars at any tempo StemLab reports.
_MIN_SEGMENT_SECONDS = 4.0

# How much of a piece its own tempo has to explain before the piece is
# believed. A real section sits near 1.0; a window straddling a change,
# or one thrown by missing beats, does not come close.
_SEGMENT_MIN_RATIO = 0.85


def _seed_period(beats: np.ndarray) -> float | None:
    """A starting period the grid sweep can reach the real one from.

    The mean of the raw intervals will not do. A dropped beat leaves an
    interval of two, and 5% of them drags the mean 5% long - past the edge
    of the +/-3% the sweep looks in, so the true period is never tried and a
    174 track seeds at 171.5. _robust_intervals throws those out first,
    which is the whole reason it exists.
    """
    try:
        intervals, _, _ = _robust_intervals(beats)
    except RuntimeError:
        return None

    return float(np.mean(intervals))


def _absorb_straddles(pieces: list[TempoSegment]) -> list[TempoSegment]:
    """Fold pieces too short to be a section into the neighbour they match."""
    if len(pieces) < 3:
        return pieces

    kept: list[TempoSegment] = []
    for index, piece in enumerate(pieces):
        short = piece.end - piece.start < _MIN_SEGMENT_SECONDS
        interior = 0 < index < len(pieces) - 1
        if short and interior and kept:
            following = pieces[index + 1]
            nearer_previous = abs(piece.bpm - kept[-1].bpm) <= abs(piece.bpm - following.bpm)
            if nearer_previous:
                kept[-1] = TempoSegment(
                    kept[-1].start, piece.end, kept[-1].bpm, kept[-1].beats + piece.beats
                )
                continue
            pieces[index + 1] = TempoSegment(
                piece.start, following.end, following.bpm, following.beats + piece.beats
            )
            continue
        kept.append(piece)

    return kept


def _tempo_segments(beats: np.ndarray) -> tuple[TempoSegment, ...]:
    """Split the track where one constant tempo stops explaining it.

    Local period over a sliding window of eight beats, cut where that moves
    by more than the quantisation noise can account for, then each piece
    measured properly with the same grid fit the whole track gets. Pieces
    whose tempos agree are merged back, so a track that never changes comes
    out as one segment rather than as noise-driven confetti.

    Eight beats because a window has to be long enough that 20 ms of
    quantisation on each end is small against the period it measures - at
    174 BPM eight beats is 2.8 s, and 20 ms across that is 0.7% - and short
    enough to place a change within a bar or two.
    """
    if beats.size < 2 * _MIN_SEGMENT_BEATS:
        return ()

    window = _MIN_SEGMENT_BEATS
    seed_interval = _seed_period(beats)
    if seed_interval is None:
        return ()

    seed = _dominant_grid(beats, seed_interval).period

    # Span over the number of beats the span actually holds, not over the
    # number of entries in it. A dropped beat makes eight entries cover nine
    # beats, and dividing by eight then reports a tempo 12% slow - enough to
    # cut a false boundary at every drop. Measured before this: a constant
    # 174 track with 5% of its beats missing came out as eleven segments
    # ranging from 152 to 174.
    span = beats[window:] - beats[:-window]
    covered = np.maximum(np.rint(span / seed), 1.0)
    local = span / covered

    # Where the local period steps, rather than where it is merely noisy.
    reference = float(np.median(local))
    boundaries = [0]
    for index in range(1, local.size):
        if abs(local[index] - reference) / max(reference, 1e-9) > _SEGMENT_THRESHOLD:
            if index - boundaries[-1] >= _MIN_SEGMENT_BEATS:
                boundaries.append(index)
                reference = float(np.median(local[index : index + window]))
    boundaries.append(beats.size)

    pieces: list[TempoSegment] = []
    for first, last in zip(boundaries, boundaries[1:], strict=False):
        span = beats[first:last]
        if span.size < _MIN_SEGMENT_BEATS:
            continue
        piece_seed = _seed_period(span)
        if piece_seed is None:
            continue
        grid = _snap_tempo(_dominant_grid(span, piece_seed), span)
        # A piece is only a section if one tempo explains it. Where the
        # sliding window crosses a change it reports something in between,
        # and where beats are missing it can misjudge how many a span holds -
        # both produce a piece no grid fits. Those are folded into a
        # neighbour rather than reported as tempo the track never plays.
        if grid.ratio < _SEGMENT_MIN_RATIO:
            continue
        pieces.append(
            TempoSegment(
                start=float(span[0]),
                end=float(span[-1]),
                bpm=round(60.0 / grid.period, 3),
                beats=int(span.size),
            )
        )

    # The window straddles a tempo change for as long as it takes to cross
    # it, and reports something in between the whole way. That is an artefact
    # of the window, not a section of the track, so a piece too short to be
    # music is folded into whichever neighbour it is closer to in tempo.
    pieces = _absorb_straddles(pieces)

    # A track that never changes tempo must come out as one segment, not as
    # however many the local window happened to cut it into.
    merged: list[TempoSegment] = []
    for piece in pieces:
        if merged and abs(piece.bpm - merged[-1].bpm) / max(merged[-1].bpm, 1e-9) <= 0.005:
            previous = merged[-1]
            total = previous.beats + piece.beats
            merged[-1] = TempoSegment(
                start=previous.start,
                end=piece.end,
                bpm=round(
                    (previous.bpm * previous.beats + piece.bpm * piece.beats) / total, 3
                ),
                beats=total,
            )
        else:
            merged.append(piece)

    return tuple(merged)


def _estimate_meter(beats: np.ndarray, downbeats: np.ndarray) -> tuple[int | None, float]:
    if beats.size < 3 or downbeats.size < 2:
        return None, 0.0

    indices = []
    for downbeat in downbeats:
        index = int(np.argmin(np.abs(beats - downbeat)))
        if abs(float(beats[index] - downbeat)) <= 0.12:
            indices.append(index)
    differences = [b - a for a, b in zip(indices, indices[1:], strict=False) if 2 <= b - a <= 12]
    if not differences:
        return None, 0.0
    meter, count = Counter(differences).most_common(1)[0]
    return int(meter), float(count / len(differences))


def derive_musical_time(
    beats: np.ndarray,
    downbeats: np.ndarray,
    model_confidence: float,
    *,
    model: str,
    device: str,
) -> BeatAnalysis:
    """Derive tempo, meter, and bar alignment from Beat This! event positions."""
    beats = np.unique(np.asarray(beats, dtype=np.float64))
    downbeats = np.unique(np.asarray(downbeats, dtype=np.float64))
    _intervals, robust_cv, inlier_ratio = _robust_intervals(beats)
    grid = _snap_tempo(_dominant_grid(beats, float(np.mean(_intervals))), beats)
    detected_bpm = 60.0 / grid.period
    meter, meter_confidence = _estimate_meter(beats, downbeats)
    stability = float(np.exp(-8.0 * robust_cv))
    confidence = float(
        np.clip(
            0.50 * stability
            + 0.30 * inlier_ratio
            + 0.15 * model_confidence
            + 0.05 * meter_confidence,
            0.0,
            1.0,
        )
    )
    # The grid's own position, not one reported beat. Every beat Beat This!
    # emits is an integer frame, so a raw downbeat is only ever accurate to
    # half a frame - 10 ms, 441 samples at 44.1 kHz - and that is what a host
    # would place bar 1 on. The fitted phase is the average of every beat the
    # grid explains, so its error falls as the track gets longer: measured at
    # 0.2 ms over 240 s rather than 10 ms. Snapped to the grid slot nearest
    # the first downbeat, so bar 1 still lands on a downbeat.
    bar_one = _snap_to_grid(
        float(downbeats[0]) if downbeats.size else float(beats[0]), grid, float(beats[0])
    )
    return BeatAnalysis(
        tempo_segments=_tempo_segments(beats),
        grid_rms=round(grid.rms, 6),
        grid_ratio=round(grid.ratio, 4),
        bpm=round(detected_bpm, 3),
        detected_bpm=round(detected_bpm, 3),
        half_time_bpm=round(detected_bpm * 0.5, 3),
        double_time_bpm=round(detected_bpm * 2.0, 3),
        beats=tuple(round(float(value), 6) for value in beats),
        downbeats=tuple(round(float(value), 6) for value in downbeats),
        meter_numerator=meter,
        meter_denominator=4,
        bar_one=round(bar_one, 6),
        confidence=round(confidence, 5),
        model=model,
        model_version=BEAT_THIS_VERSION,
        device=device,
    )


def analyse_beats(
    audio: np.ndarray,
    sample_rate: int,
    *,
    mode: str = "fast",
    device: str = "auto",
    model_dir: str | Path | None = None,
    cancellation: CancellationToken | None = None,
    progress: Callable[[float, str], None] | None = None,
) -> BeatAnalysis:
    """Run packaged Beat This! inference with progress between real chunks."""
    import torch
    from beat_this.inference import aggregate_prediction, split_piece
    from beat_this.model.postprocessor import Postprocessor
    from beat_this.preprocessing import LogMelSpect
    import soxr

    token = cancellation or CancellationToken()
    token.raise_if_cancelled()
    spec = MODEL_SPECS[mode]
    # The download gets the sliver below the 0.05 the load step starts at,
    # so a first run creeps through it instead of the bar standing still.
    model_path = ensure_packaged_model(
        mode,
        model_dir,
        progress=(
            (lambda fraction, stage: progress(0.04 * fraction, stage))
            if progress
            else None
        ),
        cancellation=token,
    )
    selected_device = choose_device(device)
    if progress:
        progress(0.05, f"Loading Beat This! {spec.name} on {selected_device.type}")
    try:
        model = _load_model(model_path, selected_device)
    except RuntimeError:
        if selected_device.type != "cuda":
            raise
        torch.cuda.empty_cache()
        selected_device = torch.device("cpu")
        if progress:
            progress(0.05, f"CUDA unavailable; loading Beat This! {spec.name} on CPU")
        model = _load_model(model_path, selected_device)
    token.raise_if_cancelled()

    mono = np.asarray(audio, dtype=np.float32)
    if mono.ndim == 2:
        mono = np.mean(mono, axis=1)
    if sample_rate != 22_050:
        mono = soxr.resample(mono, in_rate=sample_rate, out_rate=22_050).astype(np.float32)

    if progress:
        progress(0.12, "Computing Beat This! spectrogram")
    signal = torch.as_tensor(mono, dtype=torch.float32, device=selected_device)
    spectrogram = LogMelSpect(device=selected_device)(signal)
    chunks, starts = split_piece(spectrogram, 1500, border_size=6, avoid_short_end=True)
    predictions = []
    use_float16 = selected_device.type == "cuda"

    try:
        with torch.inference_mode():
            for index, chunk in enumerate(chunks):
                token.raise_if_cancelled()
                with torch.autocast(enabled=use_float16, device_type=selected_device.type):
                    prediction = model(chunk.unsqueeze(0))
                predictions.append(
                    {"beat": prediction["beat"][0], "downbeat": prediction["downbeat"][0]}
                )
                if progress:
                    progress(
                        0.15 + 0.72 * ((index + 1) / max(1, len(chunks))),
                        f"Beat This! chunk {index + 1}/{len(chunks)}",
                    )
        token.raise_if_cancelled()
        beat_logits, downbeat_logits = aggregate_prediction(
            predictions,
            starts,
            spectrogram.shape[0],
            1500,
            6,
            "keep_first",
            selected_device,
        )
        beats, downbeats = Postprocessor(type="minimal")(
            beat_logits.float(), downbeat_logits.float()
        )
        beat_frames = np.clip(
            np.rint(np.asarray(beats) * _BEAT_FPS).astype(int),
            0,
            beat_logits.numel() - 1,
        )
        probabilities = torch.sigmoid(beat_logits[beat_frames]).detach().cpu().numpy()
        model_confidence = float(np.mean(probabilities)) if probabilities.size else 0.0
        if progress:
            progress(0.92, "Interpreting tempo, meter, and downbeats")
        # Off the frame grid before anything reads them: the tempo fit, the
        # anchor, the overlay and the MIDI export all inherit whatever
        # resolution the beats arrive with.
        beat_curve = beat_logits.detach().float().cpu().numpy()
        downbeat_curve = downbeat_logits.detach().float().cpu().numpy()
        return derive_musical_time(
            _sub_frame_events(np.asarray(beats), beat_curve),
            _sub_frame_events(np.asarray(downbeats), downbeat_curve),
            model_confidence,
            model=spec.name,
            device=selected_device.type,
        )
    except JobCancelled:
        if selected_device.type == "cuda":
            torch.cuda.empty_cache()
        raise
