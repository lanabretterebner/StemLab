"""BS-RoFormer runs at 44.1 kHz, whatever rate the session is at.

The model is trained at 44.1 kHz and its band splits are fixed in bins, so
feeding it 48 kHz puts every learned band edge 8.8% high. StemLab resamples
on the way in and restores the source rate on the way out.

Neither the model nor torch is installed here, so a stand-in separator
records what it was handed and writes six stems back at that rate. That is
exactly the contract these tests are about: what reaches the model, and
what comes back sample-aligned with the session it came from.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from stemlab.audio import STEM_NAMES
from stemlab.hybrid import fuse_stem_pair
from stemlab.pretrained import ROFORMER_SAMPLE_RATE, RoFormerBackend, _resample_file

# 22050 and 88200 are exact ratios, 48000 and 96000 are not; 96000 also
# exercises the case where the round trip discards real content above
# 22.05 kHz. 44100 is the path that must not change at all.
RATES = [44100, 48000, 88200, 96000, 22050]

# 7, 4099, 100003 and 999983 are prime, so nothing about them divides
# evenly into any resampling ratio. 262144 is a power of two for contrast,
# and 96000 -> 44100 -> 96000 measurably lost a sample at that length.
LENGTHS = [1, 7, 4099, 100003, 262144]


def write_source(
    path: Path,
    rate: int,
    frames: int,
    channels: int = 2,
    subtype: str = "PCM_24",
    frequencies: tuple[float, ...] = (220.0,),
) -> Path:
    """Write a band-limited test tone every rate here can represent."""
    path.parent.mkdir(parents=True, exist_ok=True)
    t = np.arange(frames, dtype=np.float64) / rate
    wave = np.zeros(frames, dtype=np.float64)

    for frequency in frequencies:
        wave += 0.2 * np.sin(2.0 * np.pi * frequency * t)

    data = np.stack([wave * (1.0 - 0.05 * ch) for ch in range(channels)], axis=1)

    # float64 all the way to libsndfile. Casting to float32 here would make a
    # DOUBLE fixture carry no more information than a FLOAT one, so a test
    # parametrised over both could not fail on a width the code narrowed.
    sf.write(str(path), data, rate, subtype=subtype)
    return path


class FakeSeparator:
    """Stand in for ``bs-roformer-infer``: read the input, write six stems.

    It writes each stem at the rate, length, channel count and width it was
    given, which is the closest a torch-free environment can get to the real
    CLI's observable behaviour.
    """

    def __init__(self, after_write=None) -> None:
        # Runs once the six stems are on disk, which is the only moment a
        # test can plant something in the output directory that the restore
        # pass will then have to survive.
        self.after_write = after_write
        self.rates: list[int] = []
        self.frames: list[int] = []
        self.input_files: list[int] = []
        self.input_digest: str | None = None
        self.input_subtype: str | None = None
        self.stem_subtype: str | None = None
        self.stem_digests: dict[str, str] = {}

    def __call__(self, command, _log, _progress, **_kwargs) -> int:
        command = list(command)
        folder = Path(command[command.index("--input_folder") + 1])
        store = Path(command[command.index("--store_dir") + 1])

        # The upstream CLI discovers its input with a case-sensitive
        # glob("*.wav"); more than one file here would separate both.
        wavs = sorted(folder.glob("*.wav"))
        self.input_files.append(len(wavs))
        source = wavs[0]
        self.input_digest = hashlib.sha256(source.read_bytes()).hexdigest()

        info = sf.info(str(source))
        self.rates.append(info.samplerate)
        self.frames.append(info.frames)
        self.input_subtype = info.subtype
        self.stem_subtype = info.subtype

        data, rate = sf.read(str(source), always_2d=True, dtype="float32")
        store.mkdir(parents=True, exist_ok=True)

        for stem in STEM_NAMES:
            path = store / f"{source.stem}_{stem}.wav"
            sf.write(str(path), data, rate, subtype=info.subtype)
            self.stem_digests[stem] = hashlib.sha256(path.read_bytes()).hexdigest()

        if self.after_write is not None:
            self.after_write(store)

        return 0


def run_separation(
    monkeypatch, source: Path, output: Path, before_restore=None
) -> tuple[FakeSeparator, list[str]]:
    """Run the real backend against the stand-in separator."""
    separator = FakeSeparator(after_write=before_restore)
    monkeypatch.setattr("stemlab.pretrained.run_progress_process", separator)

    messages: list[str] = []
    backend = RoFormerBackend(device="cpu", log_callback=messages.append)
    backend.separate(source, output)
    return separator, messages


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_44100_input_takes_the_same_path_as_before(tmp_path, monkeypatch):
    """A 44.1 kHz session must not be resampled, rewritten, or re-encoded.

    PCM_16 on purpose: it is the one width the input leg is allowed to
    widen, so a staged file that came back PCM_24 would prove the guard
    had been lost even if every sample still matched.
    """
    source = write_source(tmp_path / "song.wav", 44100, 100003, subtype="PCM_16")
    output = tmp_path / "stems"

    calls: list[tuple] = []
    real = _resample_file

    def counted(*args, **kwargs):
        calls.append(args)
        return real(*args, **kwargs)

    monkeypatch.setattr("stemlab.pretrained._resample_file", counted)

    separator, messages = run_separation(monkeypatch, source, output)

    # Bytes in, bytes out, and no resampler touched either end of it.
    assert separator.input_digest == digest(source)
    assert separator.input_subtype == "PCM_16"
    assert calls == []
    assert {stem: digest(output / f"{stem}.wav") for stem in STEM_NAMES} == separator.stem_digests
    assert not any("Resampling" in message for message in messages)


@pytest.mark.parametrize("rate", RATES)
def test_the_model_only_ever_sees_44100(tmp_path, monkeypatch, rate):
    source = write_source(tmp_path / "song.wav", rate, 100003)

    separator, messages = run_separation(monkeypatch, source, tmp_path / "stems")

    assert separator.rates == [ROFORMER_SAMPLE_RATE]
    # One file, or the CLI's glob would hand the model both rates.
    assert separator.input_files == [1]
    assert any("Resampling" in message for message in messages) == (rate != ROFORMER_SAMPLE_RATE)


@pytest.mark.parametrize("rate", RATES)
@pytest.mark.parametrize("frames", LENGTHS)
def test_stems_come_back_at_the_source_rate_and_length(tmp_path, monkeypatch, rate, frames):
    """Exact, not approximate: a sample of drift desyncs every stem."""
    source = write_source(tmp_path / "song.wav", rate, frames)
    output = tmp_path / "stems"

    run_separation(monkeypatch, source, output)

    for stem in STEM_NAMES:
        info = sf.info(str(output / f"{stem}.wav"))
        assert (info.samplerate, info.frames) == (rate, frames), stem


def test_a_source_shorter_than_one_44100_frame_still_reaches_the_model(tmp_path, monkeypatch):
    """One frame at 96 kHz resamples away to nothing; the CLI needs audio."""
    source = write_source(tmp_path / "song.wav", 96000, 1)
    output = tmp_path / "stems"

    separator, _ = run_separation(monkeypatch, source, output)

    assert separator.frames == [1]
    assert sf.info(str(output / "vocals.wav")).frames == 1


def test_length_is_exact_where_soxr_alone_drifts(tmp_path, monkeypatch):
    """88200 -> 44100 -> 88200 came back a sample long at this length."""
    source = write_source(tmp_path / "song.wav", 88200, 999983)
    output = tmp_path / "stems"

    run_separation(monkeypatch, source, output)

    assert sf.info(str(output / "vocals.wav")).frames == 999983


@pytest.mark.parametrize("channels", [1, 2, 6])
@pytest.mark.parametrize("subtype", ["PCM_16", "PCM_24", "FLOAT", "DOUBLE"])
def test_channel_layout_and_sample_width_survive(tmp_path, monkeypatch, channels, subtype):
    source = write_source(
        tmp_path / "song.wav",
        48000,
        65537,
        channels=channels,
        subtype=subtype,
    )
    output = tmp_path / "stems"

    separator, _ = run_separation(monkeypatch, source, output)

    # The stems come back exactly as the separator wrote them: the return
    # trip may not quietly move a float stem to a narrower integer width,
    # or drop a channel of a six-channel layout.
    for stem in STEM_NAMES:
        info = sf.info(str(output / f"{stem}.wav"))
        expected = (channels, separator.stem_subtype, 65537)
        assert (info.channels, info.subtype, info.frames) == expected, stem


@pytest.mark.parametrize(
    ("subtype", "expected"),
    [("PCM_16", "PCM_24"), ("PCM_24", "PCM_24"), ("FLOAT", "FLOAT"), ("DOUBLE", "DOUBLE")],
)
def test_the_model_input_is_never_narrowed(tmp_path, monkeypatch, subtype, expected):
    """The extra trip through 44.1 kHz must not re-quantise the input.

    The temporary file the model reads is the one place a width is allowed
    to change, and only upward: a 16-bit source would otherwise be rounded
    to 16 bits a second time on the way in.
    """
    source = write_source(tmp_path / "song.wav", 48000, 65537, subtype=subtype)

    separator, _ = run_separation(monkeypatch, source, tmp_path / "stems")

    assert separator.input_subtype == expected


def test_the_round_trip_is_inaudible(tmp_path, monkeypatch):
    """Two resamples of in-band content must not audibly alter the stems."""
    frames = 200000
    source = write_source(
        tmp_path / "song.wav",
        48000,
        frames,
        subtype="FLOAT",
        frequencies=(110.0, 440.0, 3150.0),
    )
    output = tmp_path / "stems"

    run_separation(monkeypatch, source, output)

    original, _ = sf.read(str(source), always_2d=True, dtype="float32")
    stem, _ = sf.read(str(output / "vocals.wav"), always_2d=True, dtype="float32")

    # The first and last few thousand samples are the resampler's own
    # filter transient at each end, not a fidelity claim about the middle.
    edge = 3000
    error = np.max(np.abs(original[edge:-edge] - stem[edge:-edge]))
    assert error < 1e-4, error


def test_a_failed_return_trip_fails_the_separation(tmp_path, monkeypatch):
    """Stems stranded at 44.1 kHz must not be handed back as a success.

    Fusion reads its rate off the RoFormer stem, so a stem left at 44.1 kHz
    in a 48 kHz session makes every fused output 44.1 kHz - the exact state
    the return trip exists to prevent, and one that is inaudible until it is
    lined up against the session. Reporting it in the log and returning
    normally would let that ship; the whole separation fails instead.
    """
    source = write_source(tmp_path / "song.wav", 48000, 65537)
    output = tmp_path / "stems"
    real = _resample_file

    def fail_on_the_way_back(*args, **kwargs):
        if kwargs.get("out_frames") is not None:
            raise OSError("no space left on device")
        return real(*args, **kwargs)

    # The restore lives in stemlab.resample now and is shared with the Demucs
    # backend, so it resolves resample_file from that module rather than from
    # pretrained's alias. Patching the alias would sail past it.
    monkeypatch.setattr("stemlab.resample.resample_file", fail_on_the_way_back)

    with pytest.raises(RuntimeError, match="48000 Hz"):
        run_separation(monkeypatch, source, output)

    # The underlying cause survives into the message rather than being
    # replaced by the summary.
    # The half-written replacement is cleaned up rather than left behind
    # where the stem lookup would find it.
    assert not list(output.glob("*_stemlab_rate*"))


def test_a_stem_the_probe_cannot_open_does_not_strand_the_rest(tmp_path, monkeypatch):
    """rglob is sorted, so an unreadable name sorting first must not cost the stems.

    The rate probe used to sit outside the try, which turned one unopenable
    file in the output directory - a partial write, something the pre-clean
    could not unlink - into zero restored stems.
    """
    source = write_source(tmp_path / "song.wav", 48000, 65537)
    output = tmp_path / "stems"

    def seed_a_broken_file(out_dir):
        # "aaa" sorts before every stem name.
        (out_dir / "aaa_broken.wav").write_bytes(b"RIFF....WAVEfmt not-really")

    _, messages = run_separation(monkeypatch, source, output, before_restore=seed_a_broken_file)

    for stem in STEM_NAMES:
        assert sf.info(str(output / f"{stem}.wav")).samplerate == 48000, stem

    # Reported, but never mistaken for a stem going out at the wrong rate:
    # a file that cannot be read is not one this pass can mis-rate.
    assert any("aaa_broken.wav" in message for message in messages)


def test_multichannel_audio_is_not_read_channel_major(tmp_path):
    """soxr silently misreads a transposed array instead of raising.

    A (channels, frames) buffer is taken as a handful of frames with
    thousands of channels and handed straight back, so a future refactor
    that transposes on the way in would corrupt audio with no error at all.
    """
    source = tmp_path / "six.wav"
    levels = np.array([0.1, 0.2, 0.3, 0.4, 0.5, 0.6], dtype=np.float32)
    sf.write(str(source), np.tile(levels, (48000, 1)), 48000, subtype="FLOAT")

    destination = tmp_path / "six_44100.wav"
    written = _resample_file(source, destination, 44100, out_frames=44100)

    data, rate = sf.read(str(destination), always_2d=True, dtype="float32")
    assert (written, rate, data.shape) == (44100, 44100, (44100, 6))
    # Each channel still holds its own level away from the filter edges.
    assert np.allclose(data[1000:-1000], levels, atol=1e-4)


def test_fused_stems_keep_the_session_rate(tmp_path):
    """Fusion takes its rate from the RoFormer stem, so that stem sets it.

    This is why the stems are restored to the source rate rather than left
    at the model's: hybrid.fuse_stem_pair loads the RoFormer side with no
    target_sr and resamples only the Demucs side to match it.
    """
    roformer = write_source(tmp_path / "roformer_vocals.wav", 48000, 96000)
    demucs = write_source(tmp_path / "demucs_vocals.wav", 44100, 88200)
    output = tmp_path / "fused_vocals.wav"

    _, sample_rate = fuse_stem_pair(roformer, demucs, output, "vocals")

    assert sample_rate == 48000
    assert sf.info(str(output)).samplerate == 48000
    assert sf.info(str(output)).frames == 96000
