"""Adaptive/recursive stem separation router.

The JUCE plugin treats the JSON manifest written here as the boundary between
Python separation backends and the C++ tree UI. Keep backend-specific naming and
model quirks in this module; the plugin should only need stable child metadata.
"""

from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable, Iterable

from .adaptive.analysis import analyse_audio, assess_children
from .adaptive.foreground import split_foreground
from .adaptive.policy import MAX_ADAPTIVE_DEPTH, should_offer_split
from .runtime import report_downloads

try:
    from audio_separator.separator import Separator
except ImportError as exc:  # pragma: no cover - runtime dependency check
    Separator = None  # type: ignore[assignment]
    _AUDIO_SEPARATOR_IMPORT_ERROR = exc
else:
    _AUDIO_SEPARATOR_IMPORT_ERROR = None


# Models proven/selected for StemLab's first recursive pass.
VOCAL_MODEL = "UVR-BVE-4B_SN-44100-2.pth"
DRUM_MODEL = "MDX23C-DrumSep-aufr33-jarredou.ckpt"
DEVERB_MODEL = "dereverb_mel_band_roformer_less_aggressive_anvuew_sdr_18.8050.ckpt"
FOREGROUND_MODEL = "StemLab Adaptive Foreground DSP v1"

# The analyzer is intentionally conservative. This is an upper bound, not a
# promise that every composite stem will be expanded to this many children.
MAX_DYNAMIC_CHILDREN = 5

ProgressCallback = Callable[[float, str], None]


@dataclass(frozen=True)
class RecursiveChild:
    """One child node returned to the plugin's adaptive stem tree."""

    id: str
    label: str
    path: Path
    category: str = "unknown"
    actions: tuple[str, ...] = ()
    confidence: float = 0.0
    estimated_source_count: int = 1


def default_model_dir() -> Path:
    """Return the per-user cache directory for recursive model files."""
    packaged = os.environ.get("STEMLAB_RECURSIVE_MODEL_DIR")
    if packaged:
        return Path(packaged)
    local = os.environ.get("LOCALAPPDATA")
    if local:
        return Path(local) / "StemLab" / "Models" / "Recursive"
    return Path.home() / ".stemlab" / "models" / "recursive"


def _load_model(
    separator: "Separator",
    model_filename: str,
    display: str,
    progress: ProgressCallback | None,
) -> None:
    """Load a recursive model, naming its first-use download as it happens.

    These models are fetched the first time they are used - the release
    bundles carry the Engine, not the weights. audio-separator downloads
    inside this call, so without this the status area would sit on "Loading
    ..." for the length of a multi-hundred-megabyte transfer with a frozen
    bar behind it.
    """

    def on_download(percent: float) -> None:
        if progress:
            bounded = max(0.0, min(100.0, percent))
            progress(
                4.0 + 7.0 * (bounded / 100.0),
                f"Downloading the {display} model ({bounded:.0f}%)",
            )

    with report_downloads(on_download if progress else None):
        separator.load_model(model_filename=model_filename)


def _require_separator() -> None:
    if Separator is None:
        raise RuntimeError(
            "Recursive Stem Splitting requires audio-separator. "
            "Run scripts/win/setup_dev.ps1, or install StemLab with the recursive extra."
        ) from _AUDIO_SEPARATOR_IMPORT_ERROR


def _separator(output_dir: Path, model_dir: Path) -> "Separator":
    _require_separator()
    output_dir.mkdir(parents=True, exist_ok=True)
    model_dir.mkdir(parents=True, exist_ok=True)
    return Separator(
        output_dir=str(output_dir),
        model_file_dir=str(model_dir),
        output_format="WAV",
        normalization_threshold=0.9,
        amplification_threshold=0.0,
        use_soundfile=True,
    )


def _resolved_outputs(output_dir: Path, result: Iterable[str]) -> list[Path]:
    paths: list[Path] = []
    for item in result:
        path = Path(item)
        if not path.is_absolute():
            path = output_dir / path
        paths.append(path.resolve())
    return paths


def _find_output(paths: Iterable[Path], *needles: str) -> Path | None:
    normalised_needles = [x.lower().replace("-", "_").replace(" ", "_") for x in needles]
    for path in paths:
        normalised = path.stem.lower().replace("-", "_").replace(" ", "_")
        if any(needle in normalised for needle in normalised_needles):
            return path
    return None


def _json_safe_float(value: float) -> float:
    """Round for the manifest, refusing NaN/inf.

    json.dumps writes those as the bare tokens NaN/Infinity, which no strict
    JSON parser accepts - the plugin's would reject the whole manifest and
    discard a split whose audio is sitting on disk.
    """
    number = float(value)

    if not math.isfinite(number):
        return 0.0

    return round(number, 5)


def _safe_id_part(text: str) -> str:
    out = []
    previous_underscore = False
    for ch in text.lower():
        if ch.isalnum():
            out.append(ch)
            previous_underscore = False
        elif not previous_underscore:
            out.append("_")
            previous_underscore = True
    return "".join(out).strip("_") or "stem"


def _child_id(parent_id: str, label: str) -> str:
    return f"{parent_id}/{_safe_id_part(label)}"


def _children_from_outputs(
    paths: list[Path],
    parent_id: str,
    preferred: tuple[tuple[str, tuple[str, ...], str, tuple[str, ...]], ...],
    leftover_category: str,
) -> list[RecursiveChild]:
    """Map separator outputs onto labelled children, then keep unmatched files."""
    children: list[RecursiveChild] = []
    used: set[Path] = set()
    for label, needles, category, actions in preferred:
        # Search only what is still unclaimed. Needles overlap by design
        # ("reverb" is inside "no_reverb"), so matching against the whole
        # list could return a file an earlier entry already took - and the
        # entry that really wanted it then silently lost its label.
        found = _find_output([path for path in paths if path not in used], *needles)
        if found is not None and found not in used:
            children.append(
                RecursiveChild(
                    _child_id(parent_id, label),
                    label,
                    found,
                    category=category,
                    actions=actions,
                )
            )
            used.add(found)
    for path in paths:
        if path not in used:
            label = path.stem.replace("_", " ").title()
            children.append(
                RecursiveChild(
                    _child_id(parent_id, label),
                    label,
                    path,
                    category=leftover_category,
                )
            )
    return children


def _with_adaptive_metadata(
    parent_path: Path,
    children: list[RecursiveChild],
    *,
    depth: int,
) -> list[RecursiveChild]:
    """Score children and expose another split only when evidence supports it."""
    if not children:
        return children

    assessments = assess_children(parent_path, [child.path for child in children])
    updated: list[RecursiveChild] = []

    for child in children:
        assessment = assessments.get(child.path)
        if assessment is None:
            updated.append(child)
            continue

        actions = list(child.actions)
        if (
            should_offer_split(
                assessment,
                depth=depth,
                category=child.category,
            )
            and "split" not in actions
        ):
            actions.append("split")

        updated.append(
            replace(
                child,
                actions=tuple(actions),
                confidence=assessment.confidence,
                estimated_source_count=assessment.estimated_source_count,
            )
        )

    return updated


def _write_manifest(
    output_dir: Path,
    *,
    operation: str,
    source: Path,
    parent_id: str,
    root_stem: str,
    model: str,
    children: list[RecursiveChild],
    source_category: str = "unknown",
    depth: int = 1,
) -> Path:
    # Schema 2 is consumed directly by PluginProcessor::finishRecursiveJob().
    # Additive fields are fine, but renaming these keys requires a matching C++
    # parser update and a migration plan for any saved job metadata.
    data = {
        "schema": 2,
        "feature": "adaptive_stem_tree",
        "operation": operation,
        "source": str(source.resolve()),
        "source_category": source_category,
        "parent_id": parent_id,
        "root_stem": root_stem,
        "depth": depth,
        "model": model,
        "children": [
            {
                "id": child.id,
                "label": child.label,
                "path": str(child.path.resolve()),
                "category": child.category,
                "actions": list(child.actions),
                "confidence": _json_safe_float(child.confidence),
                "estimated_source_count": int(child.estimated_source_count),
            }
            for child in children
        ],
    }
    manifest = output_dir / "recursive_manifest.json"
    manifest.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return manifest


def split_drums(
    input_path: Path,
    output_dir: Path,
    *,
    parent_id: str = "drums",
    root_stem: str = "drums",
    model_dir: Path | None = None,
    progress: ProgressCallback | None = None,
    depth: int = 1,
) -> Path:
    """Split a drum stem into component children and return its manifest."""
    model_dir = model_dir or default_model_dir()
    if progress:
        progress(4.0, "Loading recursive drum model")

    separator = _separator(output_dir, model_dir)
    _load_model(separator, DRUM_MODEL, "drum separation", progress)

    if progress:
        progress(12.0, "Splitting drum components")

    custom_names = {
        "Kick": "kick",
        "Snare": "snare",
        "Toms": "toms",
        "Tom": "toms",
        "Hi-Hat": "hihat",
        "Hi Hat": "hihat",
        "Hihat": "hihat",
        "HH": "hihat",
        "Ride": "ride",
        "Crash": "crash",
        "Cymbals": "cymbals",
        "Cymbal": "cymbals",
    }
    result = separator.separate(str(input_path), custom_output_names=custom_names)
    paths = _resolved_outputs(output_dir, result)

    children = _children_from_outputs(
        paths,
        parent_id,
        (
            ("Kick", ("kick",), "drum.kick", ()),
            ("Snare", ("snare",), "drum.snare", ()),
            ("Hi-Hat", ("hihat", "hi_hat", "hh"), "drum.hihat", ()),
            ("Toms", ("toms", "tom"), "drum.toms", ()),
            ("Ride", ("ride",), "drum.ride", ()),
            ("Crash", ("crash",), "drum.crash", ()),
            ("Cymbals", ("cymbals", "cymbal"), "drum.cymbals", ()),
        ),
        leftover_category="drum.other",
    )

    if not children:
        raise RuntimeError("Drum recursion finished without output files")

    children = _with_adaptive_metadata(input_path, children, depth=depth)

    if progress:
        progress(96.0, "Writing recursive drum results")

    return _write_manifest(
        output_dir,
        operation="drums",
        source=input_path,
        parent_id=parent_id,
        root_stem=root_stem,
        model=DRUM_MODEL,
        children=children,
        source_category="drum.group",
        depth=depth,
    )


def split_vocals(
    input_path: Path,
    output_dir: Path,
    *,
    parent_id: str = "vocals",
    root_stem: str = "vocals",
    model_dir: Path | None = None,
    progress: ProgressCallback | None = None,
    depth: int = 1,
) -> Path:
    """Split vocals into lead/backing children and return its manifest."""
    model_dir = model_dir or default_model_dir()
    if progress:
        progress(4.0, "Loading lead/backing vocal model")

    separator = _separator(output_dir, model_dir)
    _load_model(separator, VOCAL_MODEL, "lead/backing vocal", progress)

    if progress:
        progress(12.0, "Splitting lead and backing vocals")

    custom_names = {
        "Lead Vocals": "lead_vocals",
        "Backing Vocals": "backing_vocals",
        "lead_only": "lead_vocals",
        "backing_only": "backing_vocals",
        # Some UVR BVE metadata presents the pair as Instrumental/Vocals.
        "Instrumental": "lead_vocals",
        "Vocals": "backing_vocals",
    }
    result = separator.separate(str(input_path), custom_output_names=custom_names)
    paths = _resolved_outputs(output_dir, result)

    children = _children_from_outputs(
        paths,
        parent_id,
        (
            ("Lead Vocal", ("lead_vocals", "lead_only", "lead"), "vocal.lead", ("deverb",)),
            (
                "Backing / Harmonies",
                ("backing_vocals", "backing_only", "backing"),
                "vocal.harmony_group",
                (),
            ),
        ),
        leftover_category="vocal.group",
    )

    if not children:
        raise RuntimeError("Vocal recursion finished without output files")

    children = _with_adaptive_metadata(input_path, children, depth=depth)

    if progress:
        progress(96.0, "Writing recursive vocal results")

    return _write_manifest(
        output_dir,
        operation="vocals",
        source=input_path,
        parent_id=parent_id,
        root_stem=root_stem,
        model=VOCAL_MODEL,
        children=children,
        source_category="vocal.group",
        depth=depth,
    )


def split_lead_group(
    input_path: Path,
    output_dir: Path,
    *,
    parent_id: str,
    root_stem: str,
    source_category: str = "instrument.group",
    progress: ProgressCallback | None = None,
    depth: int = 1,
) -> Path:
    """Adaptive variable-count lead/foreground decomposition.

    The first backend is DSP-based and intentionally labelled experimental.
    It repeatedly peels the strongest foreground candidate from the remaining
    bed. The analyzer decides the target count and validation stops recursion
    early when a useful independent child is no longer supported.
    """
    profile = analyse_audio(input_path)
    target_count = max(2, min(MAX_DYNAMIC_CHILDREN, profile.estimated_source_count))

    if progress:
        progress(4.0, f"Adaptive analysis estimates about {target_count} sources")

    current = input_path
    peeled: list[tuple[Path, float]] = []
    pass_count = max(1, target_count - 1)
    first_split = None

    for pass_index in range(pass_count):
        pass_dir = output_dir / f"foreground_pass_{pass_index + 1}"

        def pass_progress(
            percent: float,
            stage: str,
            pass_number: int = pass_index,
        ) -> None:
            if not progress:
                return
            base = 8.0 + (pass_number / pass_count) * 78.0
            span = 78.0 / pass_count
            progress(base + span * (percent / 100.0), stage)

        split = split_foreground(current, pass_dir, progress=pass_progress)
        if pass_index == 0:
            first_split = split
        assessments = assess_children(current, [split.foreground, split.backing])
        fg_assessment = assessments[split.foreground]
        bed_assessment = assessments[split.backing]

        # Stop before manufacturing a tiny/duplicate foreground layer.
        if (
            fg_assessment.confidence < 0.46
            or fg_assessment.energy_ratio < 0.045
            or bed_assessment.energy_ratio < 0.08
        ):
            break

        peeled.append((split.foreground, split.confidence))
        current = split.backing

        if bed_assessment.estimated_source_count <= 1:
            break

    if not peeled:
        # Always return a useful two-way attempt when the user explicitly asks
        # for lead separation, even when the adaptive loop is conservative.
        assert first_split is not None
        peeled = [(first_split.foreground, first_split.confidence)]
        current = first_split.backing

    children: list[RecursiveChild] = []
    for index, (path, confidence) in enumerate(peeled, start=1):
        label = "Lead / Foreground" if index == 1 else f"Foreground Layer {index}"
        children.append(
            RecursiveChild(
                _child_id(parent_id, label),
                label,
                path,
                category="instrument.lead",
                confidence=confidence,
            )
        )

    children.append(
        RecursiveChild(
            _child_id(parent_id, "Backing / Bed"),
            "Backing / Bed",
            current,
            category="instrument.bed",
        )
    )

    children = _with_adaptive_metadata(input_path, children, depth=depth)

    if progress:
        progress(96.0, f"Writing {len(children)} adaptive instrument layers")

    return _write_manifest(
        output_dir,
        operation="lead",
        source=input_path,
        parent_id=parent_id,
        root_stem=root_stem,
        model=FOREGROUND_MODEL,
        children=children,
        source_category=source_category,
        depth=depth,
    )


def deverb_vocal(
    input_path: Path,
    output_dir: Path,
    *,
    parent_id: str,
    root_stem: str = "vocals",
    model_dir: Path | None = None,
    progress: ProgressCallback | None = None,
    depth: int = 1,
) -> Path:
    """Split a vocal into dry signal and removed reverb components."""
    model_dir = model_dir or default_model_dir()
    if progress:
        progress(4.0, "Loading vocal de-reverb model")

    separator = _separator(output_dir, model_dir)
    _load_model(separator, DEVERB_MODEL, "vocal de-reverb", progress)

    if progress:
        progress(12.0, "Removing vocal reverb")

    custom_names = {
        "noreverb": "deverbed_lead",
        "No Reverb": "deverbed_lead",
        "No-Reverb": "deverbed_lead",
        "Dry": "deverbed_lead",
        "reverb": "removed_reverb",
        "Reverb": "removed_reverb",
    }
    result = separator.separate(str(input_path), custom_output_names=custom_names)
    paths = _resolved_outputs(output_dir, result)

    children = _children_from_outputs(
        paths,
        parent_id,
        (
            (
                "De-Reverbed Lead",
                ("deverbed_lead", "noreverb", "no_reverb", "dry"),
                "vocal.lead.dry",
                (),
            ),
            (
                "Removed Reverb",
                ("removed_reverb", "reverb"),
                "effect.reverb",
                (),
            ),
        ),
        leftover_category="vocal.effect",
    )

    if not children:
        raise RuntimeError("De-reverb finished without output files")

    children = _with_adaptive_metadata(input_path, children, depth=depth)

    if progress:
        progress(96.0, "Writing de-reverb results")

    return _write_manifest(
        output_dir,
        operation="deverb",
        source=input_path,
        parent_id=parent_id,
        root_stem=root_stem,
        model=DEVERB_MODEL,
        children=children,
        source_category="vocal.lead",
        depth=depth,
    )


def run_recursive(
    *,
    operation: str,
    input_path: Path,
    output_dir: Path,
    parent_id: str,
    root_stem: str,
    category: str = "unknown",
    depth: int = 1,
    model_dir: Path | None = None,
    progress: ProgressCallback | None = None,
) -> Path:
    """Route one recursive operation and return the generated manifest path."""
    input_path = input_path.expanduser().resolve()
    output_dir = output_dir.expanduser().resolve()
    depth = max(1, min(MAX_ADAPTIVE_DEPTH, int(depth)))

    if not input_path.is_file():
        raise FileNotFoundError(f"Recursive source does not exist: {input_path}")

    if operation == "drums":
        return split_drums(
            input_path,
            output_dir,
            parent_id=parent_id,
            root_stem=root_stem,
            model_dir=model_dir,
            progress=progress,
            depth=depth,
        )
    if operation == "vocals":
        return split_vocals(
            input_path,
            output_dir,
            parent_id=parent_id,
            root_stem=root_stem,
            model_dir=model_dir,
            progress=progress,
            depth=depth,
        )
    if operation == "lead":
        return split_lead_group(
            input_path,
            output_dir,
            parent_id=parent_id,
            root_stem=root_stem,
            source_category=category if category != "unknown" else "instrument.group",
            progress=progress,
            depth=depth,
        )
    if operation == "deverb":
        return deverb_vocal(
            input_path,
            output_dir,
            parent_id=parent_id,
            root_stem=root_stem,
            model_dir=model_dir,
            progress=progress,
            depth=depth,
        )
    if operation == "adaptive":
        if category.startswith("vocal."):
            return split_vocals(
                input_path,
                output_dir,
                parent_id=parent_id,
                root_stem=root_stem,
                model_dir=model_dir,
                progress=progress,
                depth=depth,
            )
        if category.startswith("instrument.") or root_stem in {"guitar", "piano", "other"}:
            return split_lead_group(
                input_path,
                output_dir,
                parent_id=parent_id,
                root_stem=root_stem,
                source_category=category,
                progress=progress,
                depth=depth,
            )
        raise ValueError(f"No adaptive backend is registered for category: {category}")

    raise ValueError(f"Unknown recursive operation: {operation}")
