"""The Remote Script's half of Set BPM.

StemLabRemote cannot be imported the way the rest of the package is: it
subclasses Live's own ControlSurface, which only exists inside Ableton. The
fixtures below stand in for the parts of the Live Object Model the tempo path
touches, so the contract can be tested without Live - the same way the script
itself already guards its ``import Live`` so the file stays readable outside
the host.

What is being pinned here is the behaviour that matters in a Live Set:

  * Warp goes off *before* the tempo moves, and only on the clips actually
    playing StemLab's source file.
  * A tempo Live would reject never reaches Live.
  * A dynamic analysis applies its first section and says plainly that Live
    took only one, because Live's API has no call that writes Arrangement
    tempo automation at all.
"""

from __future__ import annotations

import importlib
import json
import sys
import types
from pathlib import Path

import pytest

REMOTE_ROOT = Path(__file__).resolve().parents[1] / "src" / "integrations" / "ableton"


class _FakeControlSurface:
    """The two things StemLabRemote uses from its base class."""

    def __init__(self, c_instance=None):
        self._c_instance = c_instance

    def log_message(self, *_args, **_kwargs):
        return None

    def schedule_message(self, _delay, callback, *args):
        return callback(*args)


@pytest.fixture(scope="module")
def remote_module():
    """Import StemLabRemote with Live's modules stubbed out."""
    saved = {
        name: sys.modules.get(name)
        for name in ("Live", "_Framework", "_Framework.ControlSurface", "StemLabRemote")
    }

    live = types.ModuleType("Live")
    framework = types.ModuleType("_Framework")
    control_surface = types.ModuleType("_Framework.ControlSurface")
    control_surface.ControlSurface = _FakeControlSurface
    framework.ControlSurface = control_surface

    sys.modules["Live"] = live
    sys.modules["_Framework"] = framework
    sys.modules["_Framework.ControlSurface"] = control_surface
    sys.modules.pop("StemLabRemote", None)

    sys.path.insert(0, str(REMOTE_ROOT))

    try:
        yield importlib.import_module("StemLabRemote")
    finally:
        sys.path.remove(str(REMOTE_ROOT))

        for name, module in saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module


class FakeClip:
    def __init__(self, file_path, warping=True, is_audio_clip=True):
        self.file_path = file_path
        self.warping = warping
        self.is_audio_clip = is_audio_clip


class FakeSlot:
    def __init__(self, clip=None):
        self.clip = clip
        self.has_clip = clip is not None


class FakeTrack:
    def __init__(self, arrangement_clips=(), clip_slots=()):
        self.arrangement_clips = list(arrangement_clips)
        self.clip_slots = list(clip_slots)


class FakeSong:
    def __init__(self, tracks=(), tempo=120.0):
        self.tracks = list(tracks)
        self.tempo = tempo
        self.undo_steps = []

    def begin_undo_step(self):
        self.undo_steps.append("begin")

    def end_undo_step(self):
        self.undo_steps.append("end")


def make_remote(remote_module, song):
    """A StemLabRemote that never opens a socket or talks to Live."""
    remote = remote_module.StemLabRemote.__new__(remote_module.StemLabRemote)
    remote.log_message = lambda *_a, **_k: None
    remote.schedule_message = lambda _delay, callback, *args: callback(*args)
    remote.song = lambda: song
    return remote


def write_request(tmp_path, **overrides):
    request = {
        "protocol": "stemlab-ableton-tempo",
        "version": 1,
        "request_id": "req-1",
        "reply_path": str(tmp_path / "reply.json"),
        "bpm": 174.0,
        "source_path": str(tmp_path / "source.wav"),
        "segments": [],
    }
    request.update(overrides)

    path = tmp_path / "request.json"
    path.write_text(json.dumps(request), encoding="utf-8")
    return path, request


def read_reply(request):
    return json.loads(Path(request["reply_path"]).read_text(encoding="utf-8"))


def test_sets_tempo_and_unwarps_only_the_source_clip(remote_module, tmp_path):
    source = str(tmp_path / "source.wav")
    other = str(tmp_path / "other.wav")

    mine = FakeClip(source)
    theirs = FakeClip(other)
    session = FakeClip(source)

    song = FakeSong(
        tracks=[
            FakeTrack(arrangement_clips=[mine, theirs]),
            FakeTrack(clip_slots=[FakeSlot(session), FakeSlot(None)]),
        ]
    )

    remote = make_remote(remote_module, song)
    path, request = write_request(tmp_path, source_path=source)

    remote._set_tempo_on_live_thread("req-1", str(path))

    assert song.tempo == pytest.approx(174.0)
    assert mine.warping is False
    assert session.warping is False
    # A clip playing a different file is none of StemLab's business.
    assert theirs.warping is True

    reply = read_reply(request)
    assert reply["success"] is True
    assert reply["protocol"] == "stemlab-ableton-tempo-reply"
    assert reply["tempo"] == pytest.approx(174.0)
    assert "174.00" in reply["message"]


def test_undo_step_wraps_both_writes(remote_module, tmp_path):
    """One Ctrl-Z should put the Set back, tempo and Warp together."""
    source = str(tmp_path / "source.wav")
    song = FakeSong(tracks=[FakeTrack(arrangement_clips=[FakeClip(source)])])

    remote = make_remote(remote_module, song)
    path, _ = write_request(tmp_path, source_path=source)

    remote._set_tempo_on_live_thread("req-1", str(path))

    assert song.undo_steps == ["begin", "end"]


def test_already_unwarped_clip_is_left_alone(remote_module, tmp_path):
    """Assigning warping re-analyses the clip, so do not assign it twice."""
    source = str(tmp_path / "source.wav")
    clip = FakeClip(source, warping=False)
    song = FakeSong(tracks=[FakeTrack(arrangement_clips=[clip])])

    remote = make_remote(remote_module, song)
    path, request = write_request(tmp_path, source_path=source)

    remote._set_tempo_on_live_thread("req-1", str(path))

    assert clip.warping is False
    # Nothing was changed, so the message must not claim a clip was.
    assert "Warp off" not in read_reply(request)["message"]


def test_dynamic_applies_the_first_section_and_says_what_live_cannot_do(remote_module, tmp_path):
    source = str(tmp_path / "source.wav")
    song = FakeSong(tracks=[FakeTrack(arrangement_clips=[FakeClip(source)])])

    remote = make_remote(remote_module, song)
    path, request = write_request(
        tmp_path,
        source_path=source,
        bpm=174.0,
        segments=[
            {"start": 0.0, "bpm": 168.0},
            {"start": 40.0, "bpm": 174.0},
            {"start": 90.0, "bpm": 172.0},
        ],
    )

    remote._set_tempo_on_live_thread("req-1", str(path))

    # The first section, not the overall tempo: the start is what has to line
    # up for anything after it to line up.
    assert song.tempo == pytest.approx(168.0)

    reply = read_reply(request)
    assert reply["success"] is True
    assert reply["segments_total"] == 3
    assert "3 tempo sections" in reply["message"]
    assert "Song Tempo" in reply["message"]


def test_out_of_range_tempo_never_reaches_live(remote_module, tmp_path):
    song = FakeSong(tracks=[], tempo=120.0)

    remote = make_remote(remote_module, song)
    path, request = write_request(tmp_path, bpm=1200.0)

    remote._set_tempo_on_live_thread("req-1", str(path))

    assert song.tempo == pytest.approx(120.0)

    reply = read_reply(request)
    assert reply["success"] is False
    assert "range" in reply["message"]


def test_foreign_protocol_is_refused(remote_module, tmp_path):
    song = FakeSong(tracks=[], tempo=120.0)

    remote = make_remote(remote_module, song)
    path, request = write_request(tmp_path, protocol="something-else")

    remote._set_tempo_on_live_thread("req-1", str(path))

    assert song.tempo == pytest.approx(120.0)
    assert read_reply(request)["success"] is False


def test_segments_are_sorted_and_filtered(remote_module):
    normalise = remote_module.StemLabRemote._normalise_tempo_segments

    assert normalise(None) == []
    assert normalise(
        [
            {"start": 30.0, "bpm": 174.0},
            {"start": 0.0, "bpm": 168.0},
            {"start": 10.0, "bpm": 5000.0},
            {"start": 20.0},
            {"bpm": 90.0},
        ]
    ) == [
        {"start": 0.0, "bpm": 168.0},
        {"start": 0.0, "bpm": 90.0},
        {"start": 30.0, "bpm": 174.0},
    ]


def test_udp_command_decodes_the_request_path(remote_module, tmp_path):
    source = str(tmp_path / "source.wav")
    song = FakeSong(tracks=[FakeTrack(arrangement_clips=[FakeClip(source)])])

    remote = make_remote(remote_module, song)
    path, request = write_request(tmp_path, source_path=source)

    encoded = str(path).encode("utf-8").hex()
    remote._handle_udp_message("stemlab_set_tempo req-1 %s" % encoded)

    assert song.tempo == pytest.approx(174.0)
    assert read_reply(request)["success"] is True


@pytest.mark.parametrize(
    "message",
    [
        "stemlab_set_tempo",
        "stemlab_set_tempo req-1",
        "stemlab_set_tempo req-1 ",
        "stemlab_set_tempo req-1 nothexatall",
        "stemlab_set_tempo  ",
    ],
)
def test_malformed_udp_commands_do_nothing(remote_module, message):
    song = FakeSong(tracks=[], tempo=120.0)
    remote = make_remote(remote_module, song)

    remote._handle_udp_message(message)

    assert song.tempo == pytest.approx(120.0)
