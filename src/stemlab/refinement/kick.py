"""Build a kick reference and remove matching bleed from another stem."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .adaptive_cancel import CancelConfig, adaptive_cancel
from .events import Event, detect_kick_events


@dataclass
class KickRefinementConfig:
    """Timing, confidence, and cancellation settings for kick refinement."""

    reference_pre_ms: float = 8.0
    reference_post_ms: float = 180.0

    # Only strong drum events are used to construct the kick prototype.
    min_event_confidence: float = 0.45
    max_reference_events: int = 12

    # Events weaker than this are not even allowed to alter another stem.
    # This is separate from the spectral-match threshold in CancelConfig.
    min_apply_event_confidence: float = 0.30

    # Correction is faded in/out instead of hard-spliced.
    edge_fade_ms: float = 8.0

    cancel: CancelConfig = field(default_factory=CancelConfig)


@dataclass
class KickRefinementStats:
    """Counters reported after refining one target stem."""

    events_detected: int
    cancellations_attempted: int
    cancellations_applied: int
    rejected_event_confidence: int
    rejected_match_confidence: int
    mean_confidence: float


def _extract_region(
    audio: np.ndarray,
    center: int,
    pre: int,
    post: int,
) -> tuple[np.ndarray, int, int]:
    start = max(0, center - pre)
    end = min(audio.shape[-1], center + post)

    region = audio[:, start:end]

    desired = pre + post
    if region.shape[-1] < desired:
        region = np.pad(
            region,
            ((0, 0), (0, desired - region.shape[-1])),
        )

    return region.astype(np.float32), start, end


def _edge_window(length: int, fade_samples: int) -> np.ndarray:
    """Unity window with short equal-power-ish ramps only at the edges.

    A full Hann window would attenuate the kick attack because our analysis
    region starts only a few milliseconds before the event. Short edge ramps
    remove splice discontinuities while keeping the actual event at full
    correction strength.
    """
    if length <= 0:
        return np.zeros(0, dtype=np.float32)

    fade = int(max(0, min(fade_samples, length // 2)))
    window = np.ones(length, dtype=np.float32)

    if fade == 0:
        return window

    phase = np.linspace(
        0.0,
        np.pi / 2.0,
        fade,
        endpoint=True,
        dtype=np.float32,
    )
    fade_in = np.sin(phase) ** 2
    fade_out = fade_in[::-1]

    window[:fade] = fade_in
    window[-fade:] = np.minimum(window[-fade:], fade_out)

    return window


def build_kick_reference(
    drums: np.ndarray,
    events: list[Event],
    sr: int,
    cfg: KickRefinementConfig,
) -> np.ndarray | None:
    """Build a robust kick prototype from the strongest detected events."""
    pre = int(sr * cfg.reference_pre_ms / 1000.0)
    post = int(sr * cfg.reference_post_ms / 1000.0)

    strong = [e for e in events if e.confidence >= cfg.min_event_confidence]

    strong = sorted(
        strong,
        key=lambda e: e.confidence,
        reverse=True,
    )[: cfg.max_reference_events]

    if not strong:
        return None

    regions = []
    for e in strong:
        region, _, _ = _extract_region(
            drums,
            e.sample,
            pre=pre,
            post=post,
        )

        peak = np.max(np.abs(region)) + 1e-12
        regions.append(region / peak)

    # Median rejects overlapping snare/hat material better than a mean.
    reference = np.median(
        np.stack(regions, axis=0),
        axis=0,
    ).astype(np.float32)

    amplitudes = []
    for e in strong:
        region, _, _ = _extract_region(
            drums,
            e.sample,
            pre,
            post,
        )
        amplitudes.append(float(np.max(np.abs(region))))

    reference *= float(np.median(amplitudes))
    return reference


def refine_kick_bleed(
    drums: np.ndarray,
    target: np.ndarray,
    sr: int,
    cfg: KickRefinementConfig | None = None,
    events: list | None = None,
    reference: np.ndarray | None = None,
) -> tuple[np.ndarray, KickRefinementStats]:
    """Remove kick leakage without hard edits.

    Each accepted event creates only a *correction signal*:

        correction = cleaned_region - original_region

    Corrections are edge-windowed and accumulated into a separate buffer.
    The original target is never repeatedly reprocessed. Overlapping
    corrections are normalized before one final addition to the source stem.
    """
    cfg = cfg or KickRefinementConfig()

    # Detection and the reference prototype depend only on the drums, so a
    # caller refining several target stems can compute them once and pass
    # them in - otherwise every stem repeats a full-song zero-phase filter
    # and envelope convolution for byte-identical results.
    if events is None:
        events = detect_kick_events(drums, sr=sr)

    if reference is None:
        reference = build_kick_reference(
            drums,
            events,
            sr,
            cfg,
        )

    if reference is None:
        return target.copy(), KickRefinementStats(
            events_detected=len(events),
            cancellations_attempted=0,
            cancellations_applied=0,
            rejected_event_confidence=len(events),
            rejected_match_confidence=0,
            mean_confidence=0.0,
        )

    pre = int(sr * cfg.reference_pre_ms / 1000.0)
    post = int(sr * cfg.reference_post_ms / 1000.0)
    region_len = pre + post
    fade_samples = int(sr * cfg.edge_fade_ms / 1000.0)

    # Both depend only on the prototype and the region geometry, never on
    # the individual event, so they are computed once for all events.
    ref_peak = float(np.max(np.abs(reference))) + 1e-12
    edge_window = _edge_window(region_len, fade_samples=fade_samples)

    correction_sum = np.zeros_like(target, dtype=np.float32)
    window_sum = np.zeros(target.shape[-1], dtype=np.float32)

    confidences: list[float] = []
    applied = 0
    rejected_event = 0
    rejected_match = 0
    attempted = 0

    # Always match against the untouched original target. This prevents one
    # kick correction from changing the input used to estimate the next kick.
    original = target.astype(np.float32, copy=False)

    for event in events:
        if event.confidence < cfg.min_apply_event_confidence:
            rejected_event += 1
            continue

        attempted += 1

        target_region, start, end = _extract_region(
            original,
            event.sample,
            pre=pre,
            post=post,
        )

        drum_region, _, _ = _extract_region(
            drums,
            event.sample,
            pre=pre,
            post=post,
        )

        # Scale the prototype toward the current drum event before the
        # constrained spectral matcher gets to modify anything.
        event_peak = float(np.max(np.abs(drum_region)))
        scaled_ref = reference * (event_peak / ref_peak)

        result = adaptive_cancel(
            scaled_ref,
            target_region,
            sr=sr,
            cfg=cfg.cancel,
        )
        confidences.append(result.confidence)

        if result.confidence < cfg.cancel.confidence_threshold:
            rejected_match += 1
            continue

        real_len = end - start

        # Only the delta is blended into the real stem.
        correction = result.cleaned[:, :real_len] - target_region[:, :real_len]

        # Short fades ensure the correction reaches exactly zero at region
        # boundaries, eliminating hard-splice discontinuities/clicks.
        window = edge_window[:real_len]

        correction_sum[:, start:end] += correction * window[None, :]
        window_sum[start:end] += window
        applied += 1

    # For one active event, denominator stays 1 so the edge window remains a
    # true crossfade. Where correction windows overlap (>1), average them
    # instead of stacking multiple full-strength subtractions.
    overlap_normalizer = np.maximum(window_sum, 1.0)
    correction_sum /= overlap_normalizer[None, :]

    output = original + correction_sum

    stats = KickRefinementStats(
        events_detected=len(events),
        cancellations_attempted=attempted,
        cancellations_applied=applied,
        rejected_event_confidence=rejected_event,
        rejected_match_confidence=rejected_match,
        mean_confidence=float(np.mean(confidences)) if confidences else 0.0,
    )

    return output.astype(np.float32), stats
