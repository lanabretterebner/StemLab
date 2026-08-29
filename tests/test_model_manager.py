"""Tests for the model inventory the plugin's Model Manager is driven by.

Two things are worth guarding here. The first is that a status probe answers
at all on a broken install: it runs on every editor open, and a half-installed
environment is the exact case the Model Manager exists to repair, so it may
not depend on numpy, torch, or any other optional engine dependency.

The second is drift. Locating a model without importing the engine means
model_manager keeps its own copy of a handful of filenames and directory
rules. Those copies are asserted against the engine's own definitions below,
so a rename over there fails here rather than quietly making every model
report as missing.
"""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from stemlab import model_manager


def _engine_module(name: str):
    """Import an engine module, or skip when its dependencies are absent."""
    try:
        return importlib.import_module(f"stemlab.{name}")
    except Exception as exc:  # noqa: BLE001 - any import failure means "cannot check"
        pytest.skip(f"stemlab.{name} does not import here: {exc}")


class TestStatusNeedsNothingOptional:
    def test_status_runs_without_importing_numpy_or_torch(self):
        # A subprocess, because the guarantee is about what gets imported and
        # this test session may well have numpy loaded already for other
        # reasons. Only the modules the probe itself pulls in count.
        source = Path(__file__).resolve().parents[1] / "src"

        probe = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "import json,sys;"
                    "from stemlab import model_manager;"
                    "payload=model_manager.status();"
                    "print(json.dumps({"
                    "'models':len(payload['models']),"
                    "'numpy':'numpy' in sys.modules,"
                    "'torch':'torch' in sys.modules}))"
                ),
            ],
            capture_output=True,
            text=True,
            # Inherited, not hand-built. A constructed environment has to name
            # every variable the child needs, and Path.home() reads USERPROFILE
            # on Windows rather than HOME - so a POSIX-shaped env left the child
            # with no home at all and every model reporting as unlocatable.
            env={**os.environ, "PYTHONPATH": str(source)},
        )

        assert probe.returncode == 0, probe.stderr
        result = json.loads(probe.stdout.strip().splitlines()[-1])

        assert result["models"] == len(model_manager.MODELS)
        assert not result["numpy"], "the status probe must not pull in numpy"
        assert not result["torch"], "the status probe must not pull in torch"

    def test_every_model_locates_without_raising(self):
        for model in model_manager.MODELS:
            # None is a fine answer; an exception is not, because the probe
            # runs before anything is known to be installed.
            model_manager.locate(model.id)

    def test_it_survives_a_machine_with_no_nameable_home(self, monkeypatch):
        """Path.home() raises on Windows with no USERPROFILE or HOMEDRIVE.

        A release build hit exactly this: locate() reached Path.home() for the
        RoFormer cache and the whole probe died. Linux hides it, because
        Path.home() falls back to the passwd database there, so the guard has
        to be tested by making home unnameable rather than by unsetting a
        variable.
        """

        def no_home():
            raise RuntimeError("Could not determine home directory.")

        monkeypatch.setattr(Path, "home", staticmethod(no_home))

        for model in model_manager.MODELS:
            # Absent is the right answer. Raising is not.
            assert model_manager.locate(model.id) is None

        payload = model_manager.status()

        assert len(payload["models"]) == len(model_manager.MODELS)
        assert payload["anyModelMissing"] is True

        # A cache whose path cannot be resolved is left out rather than
        # offered with a size and a Clear that could not work.
        for entry in payload["caches"]:
            assert entry["path"]

    def test_status_reports_one_entry_per_model_and_cache(self):
        payload = model_manager.status()

        assert [entry["id"] for entry in payload["models"]] == [
            model.id for model in model_manager.MODELS
        ]
        assert [entry["id"] for entry in payload["caches"]] == [
            cache.id for cache in model_manager.caches()
        ]

    def test_missing_flag_agrees_with_the_per_model_entries(self):
        payload = model_manager.status()
        expected = any(not entry["present"] for entry in payload["models"])

        assert payload["anyModelMissing"] is expected


class TestMirroredConstantsHaveNotDrifted:
    def test_beat_this_filenames_match_the_specs(self):
        beat_tracking = _engine_module("beat_tracking")

        for model_id, mode in (
            ("beat-this-fast", "fast"),
            ("beat-this-accurate", "accurate"),
        ):
            expected = f"{beat_tracking.MODEL_SPECS[mode].name}.ckpt"
            assert model_manager.BEAT_THIS_FILENAMES[model_id] == expected

    def test_beat_this_search_order_matches(self):
        beat_tracking = _engine_module("beat_tracking")

        assert (
            model_manager._beat_this_directories()
            == beat_tracking._candidate_model_directories()
        )

    def test_beat_this_approx_sizes_match_the_specs(self):
        beat_tracking = _engine_module("beat_tracking")

        for model_id, mode in (
            ("beat-this-fast", "fast"),
            ("beat-this-accurate", "accurate"),
        ):
            assert (
                model_manager.MODELS_BY_ID[model_id].approx_bytes
                == beat_tracking.MODEL_SPECS[mode].size
            )

    def test_roformer_model_id_matches(self):
        pretrained = _engine_module("pretrained")

        assert model_manager.ROFORMER_MODEL_ID == pretrained.DEFAULT_MODEL

    def test_demucs_constants_match(self):
        demucs_backend = _engine_module("demucs_backend")

        assert model_manager.DEMUCS_CHECKPOINT == demucs_backend.PACKAGED_DEMUCS_FILENAME
        assert model_manager.DEMUCS_MODEL_NAME == demucs_backend.DEFAULT_DEMUCS_MODEL

    def test_recursive_filenames_and_directory_match(self):
        recursive = _engine_module("recursive")

        assert model_manager.RECURSIVE_FILENAMES == {
            "recursive-vocals": recursive.VOCAL_MODEL,
            "recursive-drums": recursive.DRUM_MODEL,
            "recursive-deverb": recursive.DEVERB_MODEL,
        }
        assert model_manager._recursive_model_dir() == recursive.default_model_dir()


class TestCompileSeam:
    def test_models_no_backend_patches_say_why_rather_than_failing_silently(self):
        # compile_support patches the RoFormer separators and nothing else, so
        # everything else must explain itself rather than offer a dead action.
        for model_id in ("demucs", "beat-this-fast", "recursive-drums"):
            model = model_manager.MODELS_BY_ID[model_id]

            assert model.compile_support == model_manager.COMPILE_UNWIRED
            assert model.compile_note, "an unwired model must explain itself"

            with pytest.raises(model_manager.CompileUnavailable):
                model_manager.compile_model(model_id)

    def test_roformer_is_offered_because_compile_support_patches_it(self):
        model = model_manager.MODELS_BY_ID["roformer"]

        assert model.compile_support == model_manager.COMPILE_SUPPORTED

    def test_the_cache_is_the_one_compile_support_actually_reads(self):
        # A second opinion about this path would warm a directory no
        # separation consults, which looks exactly like compiling doing
        # nothing. Worth a test precisely because it would fail silently.
        compile_support = _engine_module("compile_support")

        assert model_manager.inductor_cache_dir() == compile_support.inductor_cache_dir()

    def test_status_carries_the_reason_a_model_cannot_be_compiled(self):
        entries = {entry["id"]: entry for entry in model_manager.status()["models"]}

        assert entries["demucs"]["compiled"] is False
        assert entries["demucs"]["reason"]

    def test_an_unprobed_status_says_nothing_rather_than_guessing(self, monkeypatch):
        # Answering "can this machine compile" means importing torch, which
        # status refuses to do. Reporting that as a finding would put "not
        # been probed" in front of everyone who turned compiling on, so the
        # unprobed answer is silence.
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
        monkeypatch.delitem(sys.modules, "torch", raising=False)

        payload = model_manager.status()

        assert payload["compileRequested"] is True
        assert payload["compileReason"] == ""

    def test_probing_asks_compile_support_for_the_real_answer(self, monkeypatch):
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")

        payload = model_manager.status(probe_compile=True)

        # Whatever this machine says, it has to be compile_support's answer
        # for the device a job would use - not a placeholder.
        assert payload["compileRequested"] is True
        assert isinstance(payload["compileSupported"], bool)

    def test_the_probe_asks_about_the_device_a_job_would_use(self, monkeypatch):
        # Asking about "auto" would test a string that has no inductor backend
        # and always answer no; the probe has to resolve it first.
        seen = {}
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")
        monkeypatch.setattr(model_manager, "_best_device", lambda: "cuda")

        import stemlab.compile_support as compile_support

        monkeypatch.setattr(
            compile_support,
            "compile_support_status",
            lambda device: (seen.setdefault("device", device), (True, "fine"))[1],
        )

        model_manager.status(probe_compile=True)

        assert seen["device"] == "cuda"

    def test_status_explains_an_unset_opt_in_rather_than_just_saying_no(
        self, monkeypatch
    ):
        monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)
        payload = model_manager.status()

        assert payload["compileRequested"] is False
        assert "STEMLAB_TORCH_COMPILE" in payload["compileReason"]

    def test_nothing_is_pending_while_compiling_is_switched_off(self, monkeypatch):
        # Otherwise the Model Manager would invite itself open to offer a
        # warm-up that the separation would not use anyway.
        monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)

        assert model_manager.status()["anyCompilePending"] is False

    def test_a_downloaded_model_with_no_backend_reports_unavailable(self, tmp_path, monkeypatch):
        # The seam, not the warm-up: with the weights in place and no
        # stemlab.model_compile installed, asking to compile must say so
        # rather than claim success or raise something unrecognisable.
        weights = tmp_path / "model.ckpt"
        weights.write_bytes(b"not really a checkpoint")

        monkeypatch.setattr(model_manager, "locate", lambda _id: weights)
        monkeypatch.setitem(sys.modules, "stemlab.model_compile", None)

        with pytest.raises(model_manager.CompileUnavailable):
            model_manager.compile_model("roformer")


class TestRemoval:
    def test_deleting_an_absent_model_reclaims_nothing_and_does_not_raise(self, monkeypatch):
        monkeypatch.setattr(model_manager, "locate", lambda _id: None)

        assert model_manager.delete_model("beat-this-fast") == 0

    def test_deleting_a_model_takes_its_verification_sidecar_with_it(
        self, tmp_path, monkeypatch
    ):
        weights = tmp_path / "small0.ckpt"
        weights.write_bytes(b"0" * 32)

        # beat_tracking records this beside the checkpoint so a re-hash can be
        # skipped; left behind it would describe a file that no longer exists.
        sidecar = tmp_path / "small0.ckpt.verified.json"
        sidecar.write_text("{}", encoding="utf-8")

        monkeypatch.setattr(model_manager, "locate", lambda _id: weights)

        freed = model_manager.delete_model("beat-this-fast")

        assert not weights.exists()
        assert not sidecar.exists()
        assert freed == 34

    def test_unknown_cache_id_is_rejected_rather_than_ignored(self):
        with pytest.raises(KeyError):
            model_manager.delete_cache("not-a-cache")

    def test_the_analysis_cache_warns_that_it_holds_corrections(self):
        analysis = next(
            cache for cache in model_manager.caches() if cache.id == "analysis"
        )

        # Deleting it loses user-entered BPM, key and meter corrections, which
        # nothing can re-download. The UI needs to be able to say so.
        assert "correction" in analysis.warning.lower()


class TestCommandLine:
    def _run(self, *args: str):
        source = Path(__file__).resolve().parents[1] / "src"

        return subprocess.run(
            [sys.executable, "-m", "stemlab.model_manager", *args],
            capture_output=True,
            text=True,
            # Inherited, not hand-built. A constructed environment has to name
            # every variable the child needs, and Path.home() reads USERPROFILE
            # on Windows rather than HOME - so a POSIX-shaped env left the child
            # with no home at all and every model reporting as unlocatable.
            env={**os.environ, "PYTHONPATH": str(source)},
        )

    def test_status_writes_json_and_announces_its_path(self, tmp_path):
        destination = tmp_path / "inventory.json"

        result = self._run("--status", "--output", str(destination))

        assert result.returncode == 0, result.stderr
        assert f"STEMLAB_MODEL_INVENTORY {destination}" in result.stdout

        payload = json.loads(destination.read_text(encoding="utf-8"))
        assert len(payload["models"]) == len(model_manager.MODELS)

    def test_an_unknown_model_id_is_refused(self):
        result = self._run("--download", "no-such-model")

        assert result.returncode != 0
        assert "no-such-model" in (result.stderr + result.stdout)

    def test_doing_nothing_is_an_error_rather_than_a_silent_success(self):
        result = self._run()

        assert result.returncode != 0

    def test_failures_are_reported_on_the_protocol(self, tmp_path):
        # delete_cache raises KeyError for an unknown id; main() is the
        # boundary that turns that into a line the plugin can show.
        result = self._run("--delete-cache", "not-a-cache")

        assert result.returncode == 1
        assert "STEMLAB_ERROR" in result.stdout


class TestTheDownloaderIsNotRunThroughPipsLauncher:
    """A shipped Engine is built in one directory and run from another.

    Pip writes an absolute interpreter path into every console script it
    generates, so in a shipped Engine that path names a machine the user does
    not have. Exec'ing such a launcher reports ENOENT against the *launcher*,
    which is why the first report of this read as a missing file that was
    plainly present:

        FileNotFoundError: [Errno 2] No such file or directory:
            '~/.local/share/StemLab/Engine/bin/bs-roformer-download'

    Checking the file exists first - which this module did - cannot catch it.
    So the property worth pinning is not "we handle the error"; it is that the
    launcher beside the interpreter is never the thing we run.
    """

    def test_it_runs_the_module_under_the_running_interpreter(self):
        command = model_manager.bs_roformer_download_command("some-model")

        assert command[0] == sys.executable
        assert command[1:] == ["-m", "stemlab.bs_roformer_download_cli", "some-model"]

    def test_a_launcher_beside_the_interpreter_is_still_not_used(self, tmp_path, monkeypatch):
        # The regression itself: a launcher sitting right there, existing,
        # executable, and unrunnable. Nothing about it may change the command.
        engine_bin = tmp_path / "bin"
        engine_bin.mkdir()
        interpreter = engine_bin / "python3"
        interpreter.write_text("")

        for name in ("bs-roformer-download", "bs-roformer-download.exe"):
            launcher = engine_bin / name
            launcher.write_text("#!/nonexistent/build/machine/python\n")
            launcher.chmod(0o755)

        monkeypatch.setattr(sys, "executable", str(interpreter))

        command = model_manager.bs_roformer_download_command()

        assert command == [str(interpreter), "-m", "stemlab.bs_roformer_download_cli"]
        assert not any(part.endswith("bs-roformer-download") for part in command)

    def test_the_module_it_names_is_importable(self):
        # -m fails at the point of use, in a child, on a machine that is
        # already having a bad day. Cheaper to find out here.
        import importlib.util

        assert importlib.util.find_spec("stemlab.bs_roformer_download_cli") is not None

    def test_downloading_the_roformer_goes_through_it(self, tmp_path, monkeypatch):
        seen = {}
        fetched = tmp_path / "BS-Rofo-SW-Fixed.ckpt"
        fetched.write_bytes(b"weights")

        def fake_child(command, label, progress, cancellation, span):
            seen["command"] = list(command)

        monkeypatch.setattr(model_manager, "_run_child", fake_child)
        monkeypatch.setattr(model_manager, "locate", lambda _id: fetched)

        model_manager.download("roformer")

        assert seen["command"] == model_manager.bs_roformer_download_command(
            model_manager.ROFORMER_MODEL_ID
        )


class TestAFailedDownloadSaysWhatTheChildSaid:
    def test_the_error_carries_the_last_of_the_output(self, monkeypatch):
        # An exit code alone has sent more than one person reading source.
        script = "import sys; print('could not reach huggingface.co'); sys.exit(1)"

        with pytest.raises(RuntimeError) as caught:
            model_manager._run_child(
                [sys.executable, "-c", script], "Downloading", None, None, (0.0, 1.0)
            )

        assert "could not reach huggingface.co" in str(caught.value)
        assert "1" in str(caught.value)
