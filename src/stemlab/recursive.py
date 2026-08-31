"""Adaptive/recursive stem separation router.

The JUCE plugin treats the JSON manifest written here as the boundary between
Python separation backends and the C++ tree UI. Keep backend-specific naming and
model quirks in this module; the plugin should only need stable child metadata.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, replace
from pathlib import Path
from typing import TYPE_CHECKING, Callable, Iterable

from .adaptive.analysis import analyse_audio, assess_children
from .adaptive.foreground import split_foreground
from .adaptive.policy import MAX_ADAPTIVE_DEPTH, should_offer_split
from .paths import recursive_models_dir
from .runtime import report_downloads

if TYPE_CHECKING:
    from audio_separator.separator import Separator


# Models proven/selected for StemLab's first recursive pass.
VOCAL_MODEL = "UVR-BVE-4B_SN-44100-2.pth"
# Only when the input's own rate cannot be read; audio-separator's own
# default, so this changes nothing that was not already broken.
RECURSIVE_FALLBACK_SAMPLE_RATE = 44100

# audio-separator's VR backend - the one that runs .pth models, which here
# means VOCAL_MODEL - always emits 44.1 kHz no matter what it is asked for.
# vr_separator converts its spectrogram back to a waveform and resamples it to
# a hardcoded ``target_sr=44100``; it never reads the Separator's
# ``sample_rate``. common_separator then uses that ``sample_rate`` purely as
# the header value it writes the file with. So asking the VR backend for
# 48 kHz resamples nothing - it stamps 44.1 kHz audio as 48 kHz, and the child
# plays back 8.8% fast and about 1.5 semitones sharp against its own parent.
# The MDXC backend that runs the .ckpt models (DRUM_MODEL, DEVERB_MODEL) does
# honour the requested rate end to end, which is why only vocals drifted.
VR_BACKEND_SAMPLE_RATE = 44100

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
    return recursive_models_dir()


# audio-separator keeps its model registry beside the weights, and fetches it
# the same unsafe way, so it can be truncated by the same interrupted transfer.
_DOWNLOAD_REGISTRY = "download_checks.json"


def _model_cache_files(model_dir: Path, model_filename: str) -> list[Path]:
    """Every cached file belonging to one model.

    audio-separator stores a checkpoint under its own name and any config it
    needs beside it under the same stem, flat in the model directory. Matching
    on the stem rather than the exact name is what also catches the ``.yaml``
    a roformer model is useless without.
    """
    if not model_dir.is_dir():
        return []

    stem = Path(model_filename).stem
    return sorted(
        path for path in model_dir.iterdir() if path.is_file() and path.name.startswith(stem)
    )


def _discard_unusable_downloads(model_dir: Path, model_filename: str) -> list[str]:
    """Delete the cached files for a model so the next load re-fetches them.

    The registry goes too, but only when it no longer parses: it is shared by
    every model, and throwing it away on an unrelated failure would cost a
    round trip for nothing.
    """
    discarded: list[str] = []

    for path in _model_cache_files(model_dir, model_filename):
        try:
            path.unlink()
        except OSError:
            continue
        discarded.append(path.name)

    registry = model_dir / _DOWNLOAD_REGISTRY
    if registry.is_file():
        try:
            json.loads(registry.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            try:
                registry.unlink()
                discarded.append(registry.name)
            except OSError:
                pass

    return discarded


def _load_model(
    separator: "Separator",
    model_filename: str,
    display: str,
    progress: ProgressCallback | None,
    model_dir: Path | None = None,
) -> None:
    """Load a recursive model, naming its first-use download as it happens.

    These models are fetched the first time they are used - the release
    bundles carry the Engine, not the weights. audio-separator downloads
    inside this call, so without this the status area would sit on "Loading
    ..." for the length of a multi-hundred-megabyte transfer with a frozen
    bar behind it.

    A load that fails once is retried once, against a cleared cache.
    audio-separator writes a download straight to its final path with no
    temporary file, and then skips downloading whenever that path exists
    (``download_file_if_not_exists``). A transfer interrupted by a cancel or a
    dropped connection therefore leaves a truncated file that every later run
    reuses and rejects - the same failure, for ever, until somebody deletes it
    by hand. Nothing upstream clears it, so this does.
    """

    def on_download(percent: float) -> None:
        if progress:
            bounded = max(0.0, min(100.0, percent))
            progress(
                4.0 + 7.0 * (bounded / 100.0),
                f"Downloading the {display} model ({bounded:.0f}%)",
            )

    def attempt() -> None:
        with report_downloads(on_download if progress else None):
            separator.load_model(model_filename=model_filename)

    try:
        attempt()
        return
    except Exception:
        if model_dir is None:
            raise

        discarded = _discard_unusable_downloads(model_dir, model_filename)

        if not discarded:
            # Nothing cached to blame, so the failure is real: a missing
            # dependency, no network, an unsupported device. Re-downloading
            # would only fail the same way, more slowly.
            raise

        if progress:
            progress(4.0, f"Re-downloading the {display} model")

    attempt()


def _require_separator() -> "type[Separator]":
    # Imported here, not at module scope: audio-separator brings torch and
    # onnxruntime with it, and the pure-DSP operations in this module (lead /
    # adaptive instrument splits) must run without paying for that stack.
    try:
        from audio_separator.separator import Separator
    except ImportError as exc:
        raise RuntimeError(
            "Recursive Stem Splitting requires audio-separator. "
            "Run scripts/win/setup_dev.ps1, or install StemLab with the recursive extra."
        ) from exc
    return Separator


def __getattr__(name: str) -> object:
    # `recursive.Separator` is the optional-dependency probe the build
    # scripts and tests check: the class when audio-separator is installed,
    # None when it is not. Resolving it on attribute access keeps a plain
    # import of this module free of the ML stack.
    if name != "Separator":
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    try:
        return _require_separator()
    except RuntimeError:
        return None


def _backend_sample_rate(model_filename: str, source_rate: int) -> int:
    """The rate the backend behind ``model_filename`` will really produce.

    Asking for a rate a backend ignores is worse than asking for the rate it
    honours: the request is silently dropped but still becomes the file's
    header. Ask each backend for what it can actually deliver and put the
    result back on the session's rate afterwards.
    """
    if Path(model_filename).suffix.lower() == ".pth":
        return VR_BACKEND_SAMPLE_RATE
    return source_rate


def _conform_sample_rate(
    paths: Iterable[Path],
    target_rate: int,
    target_frames: int | None,
    progress: ProgressCallback | None = None,
) -> None:
    """Put children on the parent's rate, and on its exact length.

    A no-op whenever the backend already wrote the rate that was asked of it,
    so it is safe to run after every split. The length is pinned to the
    parent's because a child that is one sample longer or shorter drifts
    against the stem it was split out of, and the plugin's Solo and Mute
    subtract those children from that same parent. ``target_frames`` of None
    means the parent's length could not be read, which is not the same as it
    being zero - pinning to a failed probe would truncate every child to
    silence, so the resampler's own length is kept instead.
    """
    import soundfile as sf

    from .resample import resample_file

    for path in sorted(paths):
        try:
            rate = int(sf.info(str(path)).samplerate)
        except Exception:
            # A file whose rate cannot be read is also a file this cannot
            # mis-rate, and output_dir holds whatever the backend left behind
            # as well as the children. Stepping over it leaves it exactly as
            # the backend wrote it, which is the status quo for anything this
            # function was never going to touch.
            continue

        if rate == target_rate:
            continue

        if progress:
            progress(90.0, f"Returning {path.name} to {target_rate} Hz")

        restored = path.with_name(f"{path.stem}_stemlab_rate{path.suffix}")
        try:
            resample_file(path, restored, target_rate, out_frames=target_frames)
            # A file cannot be rewritten underneath its own reader, so the
            # resample lands beside the child and then takes its place.
            restored.replace(path)
        except Exception as exc:
            restored.unlink(missing_ok=True)
            # Fail the split rather than publish this child. Left alone it
            # would play, which is worse than an error: nothing downstream
            # inspects a child's rate, so the only report would be the user
            # hearing it drift against the stem it came out of.
            raise RuntimeError(
                f"Could not return {path.name} to the session's rate of "
                f"{target_rate} Hz. It holds {rate} Hz audio and would play "
                "back at the wrong pitch and speed against its own parent."
            ) from exc


def _source_rate_and_frames(input_path: Path) -> tuple[int, int | None]:
    """The parent stem's rate and length, which its children have to match.

    A length of None says the probe failed. soundfile is not the only reader
    in this pipeline - audio-separator reaches audio through librosa, which
    can open formats libsndfile will not - so a stem that cannot be probed
    here can still separate perfectly well, and must not be truncated for it.
    """
    import soundfile as sf

    try:
        info = sf.info(str(input_path))
    except Exception:
        return RECURSIVE_FALLBACK_SAMPLE_RATE, None

    rate = int(info.samplerate)
    frames = int(info.frames)
    return (
        rate if rate > 0 else RECURSIVE_FALLBACK_SAMPLE_RATE,
        frames if frames > 0 else None,
    )


def _separator(output_dir: Path, model_dir: Path, sample_rate: int) -> "Separator":
    separator_cls = _require_separator()
    output_dir.mkdir(parents=True, exist_ok=True)
    model_dir.mkdir(parents=True, exist_ok=True)
    return separator_cls(
        output_dir=str(output_dir),
        model_file_dir=str(model_dir),
        output_format="WAV",
        # audio-separator defaults this to 44100 and both loads and writes at
        # it, so on any session that is not 44.1 kHz the children came back at
        # a different rate from the stem they were split out of. The plugin's
        # monitor mix takes its rate from the first lane it reads and skips
        # every lane that disagrees, so those children were dropped from the
        # mix entirely - silently, which is what made a split look like it had
        # broken the parent stem's Solo and Mute.
        sample_rate=sample_rate,
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

    source_rate, source_frames = _source_rate_and_frames(input_path)
    separator = _separator(
        output_dir, model_dir, _backend_sample_rate(DRUM_MODEL, source_rate)
    )
    _load_model(separator, DRUM_MODEL, "drum separation", progress, model_dir)

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
    _conform_sample_rate(paths, source_rate, source_frames, progress)

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

    source_rate, source_frames = _source_rate_and_frames(input_path)
    separator = _separator(
        output_dir, model_dir, _backend_sample_rate(VOCAL_MODEL, source_rate)
    )
    _load_model(separator, VOCAL_MODEL, "lead/backing vocal", progress, model_dir)

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
    _conform_sample_rate(paths, source_rate, source_frames, progress)

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

    source_rate, source_frames = _source_rate_and_frames(input_path)
    separator = _separator(
        output_dir, model_dir, _backend_sample_rate(DEVERB_MODEL, source_rate)
    )
    _load_model(separator, DEVERB_MODEL, "vocal de-reverb", progress, model_dir)

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
    _conform_sample_rate(paths, source_rate, source_frames, progress)

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
