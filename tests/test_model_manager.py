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
import types
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
        assert payload["essentialModelMissing"] is True

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
        expected = any(
            not entry["present"]
            for entry in payload["models"]
            if entry["id"] in model_manager.ESSENTIAL_MODEL_IDS
        )

        assert payload["essentialModelMissing"] is expected


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

    def test_compiling_switched_off_is_reported_as_off(self, monkeypatch):
        # The Model Manager no longer invites itself open over compiling at
        # all, but it still has to say whether compiling is on: an unset
        # opt-in and a missing compiler need opposite advice.
        monkeypatch.delenv("STEMLAB_TORCH_COMPILE", raising=False)

        payload = model_manager.status()

        assert payload["compileRequested"] is False
        assert "STEMLAB_TORCH_COMPILE" in payload["compileReason"]

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

    def test_the_analysis_cache_no_longer_warns(self):
        analysis = next(
            cache for cache in model_manager.caches() if cache.id == "analysis"
        )

        # It used to warn that clearing it lost manual BPM/key corrections,
        # which nothing could recover. Corrections are gone, and everything
        # left in the file is a cached result the next analysis recomputes -
        # so a warning here would now be telling the user to fear a re-run.
        assert analysis.warning == ""


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

        assert seen["command"] == [
            sys.executable,
            "-m",
            "stemlab.bs_roformer_download_cli",
            "--model",
            model_manager.ROFORMER_MODEL_ID,
        ]


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


class TestTheDownloaderIsGivenArgumentsItAccepts:
    """The first version of this shipped the model as a bare positional.

    Upstream's parser takes ``--model`` (repeatable, dest="models") and
    rejects a positional with ``error: unrecognized arguments`` and exit code
    2, so every download failed - before and after the launcher fix, which is
    why fixing the launcher alone did not make downloading work.

    The test below is deliberately not written against
    bs_roformer_download_command: an expectation derived from the code under
    test agrees with it whether or not either is right, which is exactly how
    the wrong argv survived a green suite.
    """

    def _download_arguments(self):
        command = model_manager.bs_roformer_download_command(
            "--model", model_manager.ROFORMER_MODEL_ID
        )
        # Everything the entry point itself sees: past the interpreter, -m,
        # and the module name.
        return command[3:]

    def test_the_model_is_passed_as_an_option_not_a_positional(self):
        assert self._download_arguments() == ["--model", model_manager.ROFORMER_MODEL_ID]

    def test_upstreams_own_parser_accepts_it(self, monkeypatch):
        # The only check that cannot be wrong about what upstream takes,
        # because it asks upstream. Needs bs_roformer importable, which costs
        # torch - true in an Engine and in a dev install, not in plain CI.
        download = pytest.importorskip(
            "bs_roformer.download", reason="bs-roformer-infer is not installed here"
        )

        monkeypatch.setattr(
            sys, "argv", ["bs-roformer-download", *self._download_arguments()]
        )

        parsed = download.parse_args()
        selected = download._resolve_models(parsed)

        # Not just "it parsed": an unknown identifier prints a complaint,
        # resolves to nothing, and still exits 0, which would leave the
        # failure to the on-disk check afterwards.
        assert [model.slug for model in selected] == [model_manager.ROFORMER_MODEL_ID]

    def test_listing_models_is_the_flag_upstream_documents(self):
        command = model_manager.bs_roformer_download_command("--list-models")

        assert command[3:] == ["--list-models"]


class TestDemucsIsFoundWhereItActuallyLands:
    """`get_model` prefers the HuggingFace hub over the legacy .th repo.

    demucs.pretrained.get_model tries adefossez/HTDemucs-6s first, and that
    repository exists, so on a machine with a reachable hub the weights arrive
    as safetensors in the HF cache and 5c90dfd2-34c22ccb.th is never written.
    Looking only for the .th turned a successful download into "Demucs still
    is not on disk after downloading it", and showed a working Demucs as
    missing in the inventory.
    """

    def _hf_snapshot(self, root: Path) -> Path:
        snapshot = (
            root / "hub" / model_manager.DEMUCS_HF_DIRECTORY / "snapshots" / "abc123"
        )
        snapshot.mkdir(parents=True)
        return snapshot

    def test_the_huggingface_copy_counts_as_installed(self, tmp_path, monkeypatch):
        snapshot = self._hf_snapshot(tmp_path)
        (snapshot / "5c90dfd2.safetensors").write_bytes(b"x" * 2048)

        monkeypatch.setenv("HF_HOME", str(tmp_path))
        monkeypatch.setenv("TORCH_HOME", str(tmp_path / "empty-torch"))
        monkeypatch.delenv("STEMLAB_DEMUCS_MODEL_REPO", raising=False)

        found = model_manager.locate("demucs")

        assert found is not None
        assert found.name == "5c90dfd2.safetensors"

    def test_the_legacy_checkpoint_still_wins_when_it_is_there(self, tmp_path, monkeypatch):
        # A packaged release ships the .th and sets HF_HUB_OFFLINE; that path
        # must not start depending on a hub cache it deliberately avoids.
        snapshot = self._hf_snapshot(tmp_path)
        (snapshot / "5c90dfd2.safetensors").write_bytes(b"x" * 2048)

        torch_home = tmp_path / "torch"
        checkpoints = torch_home / "hub" / "checkpoints"
        checkpoints.mkdir(parents=True)
        (checkpoints / model_manager.DEMUCS_CHECKPOINT).write_bytes(b"y" * 4096)

        monkeypatch.setenv("HF_HOME", str(tmp_path))
        monkeypatch.setenv("TORCH_HOME", str(torch_home))
        monkeypatch.delenv("STEMLAB_DEMUCS_MODEL_REPO", raising=False)

        assert model_manager.locate("demucs").name == model_manager.DEMUCS_CHECKPOINT

    def test_neither_present_is_still_missing(self, tmp_path, monkeypatch):
        monkeypatch.setenv("HF_HOME", str(tmp_path / "hf"))
        monkeypatch.setenv("TORCH_HOME", str(tmp_path / "torch"))
        monkeypatch.delenv("STEMLAB_DEMUCS_MODEL_REPO", raising=False)

        assert model_manager.locate("demucs") is None

    def test_hf_hub_cache_wins_over_hf_home(self, tmp_path, monkeypatch):
        # huggingface_hub's own precedence, and the two are not the same
        # directory: HF_HUB_CACHE names the hub folder, HF_HOME its parent.
        cache = tmp_path / "explicit"
        snapshot = cache / model_manager.DEMUCS_HF_DIRECTORY / "snapshots" / "abc123"
        snapshot.mkdir(parents=True)
        (snapshot / "5c90dfd2.safetensors").write_bytes(b"x" * 2048)

        monkeypatch.setenv("HF_HUB_CACHE", str(cache))
        monkeypatch.setenv("HF_HOME", str(tmp_path / "unused"))
        monkeypatch.setenv("TORCH_HOME", str(tmp_path / "torch"))
        monkeypatch.delenv("STEMLAB_DEMUCS_MODEL_REPO", raising=False)

        assert model_manager.locate("demucs") is not None

    def test_the_mirrored_cache_name_matches_demucs(self):
        # The drift guard: demucs owns this naming, we only copy it.
        hf = pytest.importorskip("demucs.hf", reason="demucs is not installed here")

        expected = f"{hf.DEFAULT_NAMESPACE}/{hf.hf_repo_name(model_manager.DEMUCS_MODEL_NAME)}"

        assert model_manager.DEMUCS_HF_DIRECTORY == "models--" + expected.replace("/", "--")


class TestWhatOpensTheModelManagerUnasked:
    """The panel used to appear on every single launch.

    Two separate reasons, both fixed by the flag below. It opened when *any*
    of the seven models was absent - including the optional ones that fetch
    themselves on first use - and it opened whenever compiling was switched
    on and a present model had no warm-up marker. Nothing writes that marker
    except the Model Manager's own Compile action, so with compiling on the
    second condition was true forever, no matter what was installed.
    """

    def _payload(self, monkeypatch, present):
        monkeypatch.setattr(model_manager, "locate", lambda model_id: (
            Path("/tmp/pretend.ckpt") if model_id in present else None
        ))
        monkeypatch.setattr(Path, "stat", lambda self: os.stat_result((0,) * 10))
        return model_manager.status()

    def test_every_essential_model_present_does_not_open_it(self, monkeypatch):
        payload = self._payload(monkeypatch, set(model_manager.ESSENTIAL_MODEL_IDS))

        assert payload["essentialModelMissing"] is False

    def test_an_optional_model_missing_does_not_open_it(self, monkeypatch):
        # The regression: beat tracking and the adaptive-split models are
        # fetched on first use, and their absence is not a problem to solve.
        present = set(model_manager.ESSENTIAL_MODEL_IDS)
        optional = [m.id for m in model_manager.MODELS if m.id not in present]

        assert optional, "this test is pointless if everything is essential"

        payload = self._payload(monkeypatch, present)

        assert payload["essentialModelMissing"] is False

    def test_a_missing_essential_model_does_open_it(self, monkeypatch):
        for essential in model_manager.ESSENTIAL_MODEL_IDS:
            present = {m.id for m in model_manager.MODELS} - {essential}

            assert self._payload(monkeypatch, present)["essentialModelMissing"] is True

    def test_compiling_being_available_is_not_a_reason_to_open_it(self, monkeypatch):
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE", "1")

        payload = self._payload(monkeypatch, {m.id for m in model_manager.MODELS})

        # The condition that fired on every launch is gone entirely, not
        # merely false today.
        assert "anyCompilePending" not in payload
        assert payload["essentialModelMissing"] is False


class TestClearingTheKernelCacheClearsItsMarkers:
    def test_a_warmed_model_stops_claiming_to_be_compiled(self, tmp_path, monkeypatch):
        # The markers live beside the cache directory, so an rmtree of the
        # cache leaves them behind and every warmed model goes on reporting
        # "Compiled" against kernels that are gone.
        cache = tmp_path / "inductor"
        cache.mkdir()
        (cache / "kernel.so").write_bytes(b"x" * 4096)

        marker = tmp_path / "stemlab_warm_roformer.json"
        marker.write_text('{"torch": "2.0.0"}', encoding="utf-8")

        # Through the documented override rather than by patching an
        # internal: the marker path is compile_support's to decide, and a
        # test that patches one of the two functions resolving it stops
        # testing the path the app actually uses.
        monkeypatch.setenv("STEMLAB_TORCH_COMPILE_CACHE", str(cache))
        monkeypatch.setattr(model_manager, "_torch_version", lambda: "2.0.0")

        assert model_manager._compile_state("roformer")["compiled"] is True

        freed = model_manager.delete_cache(model_manager.COMPILE_CACHE_ID)

        assert not marker.exists()
        assert freed >= 4096
        assert model_manager._compile_state("roformer")["compiled"] is False


class TestTheWarmUpMarkerSurvivesBeingReadElsewhere:
    """The marker is written by one process and read by another.

    Compiling imports torch; the status probe refuses to, because it runs on
    every editor open. If the recorded version came from `torch.__version__`
    in one and from package metadata in the other, the two could disagree
    over a build's local suffix - and `_compile_state` retires a marker whose
    version does not match, so a model that really was compiled reports
    itself as not compiled and the Compile button never changes.
    """

    @staticmethod
    def _installed_torch(monkeypatch, metadata, imported):
        """An installed torch whose metadata and module disagree."""
        import importlib.metadata

        monkeypatch.setattr(
            importlib.metadata, "version", lambda name: metadata if name == "torch" else ""
        )

        if imported is None:
            monkeypatch.delitem(sys.modules, "torch", raising=False)
        else:
            module = types.ModuleType("torch")
            module.__version__ = imported
            monkeypatch.setitem(sys.modules, "torch", module)

    def test_the_version_does_not_depend_on_torch_being_imported(self, monkeypatch):
        # The two spellings a build can have. Which one gets recorded must not
        # depend on whether the process that asked happened to import torch.
        self._installed_torch(monkeypatch, "2.11.0+cpu", "2.11.0a0+gitdeadbee")
        while_compiling = model_manager._torch_version()

        self._installed_torch(monkeypatch, "2.11.0+cpu", None)
        while_probing = model_manager._torch_version()

        assert while_compiling == while_probing == "2.11.0+cpu"

    def test_a_marker_written_while_compiling_is_accepted_by_a_status_probe(
        self, tmp_path, monkeypatch
    ):
        cache = tmp_path / "inductor"
        cache.mkdir()
        monkeypatch.setattr(model_manager, "_locatable_inductor_cache_dir", lambda: cache)

        # Written by the compiling process, which has torch loaded.
        self._installed_torch(monkeypatch, "2.11.0+cpu", "2.11.0a0+gitdeadbee")

        marker = model_manager._compile_marker("roformer")
        marker.write_text(
            json.dumps({"torch": model_manager._torch_version()}), encoding="utf-8"
        )

        # Read back by the status probe, which refuses to import it.
        self._installed_torch(monkeypatch, "2.11.0+cpu", None)

        assert model_manager._compile_state("roformer")["compiled"] is True

    def test_a_marker_from_a_different_torch_is_still_retired(self, tmp_path, monkeypatch):
        # The guard has to keep working: inductor artifacts do not survive a
        # torch upgrade, so a marker from an older one is not evidence.
        cache = tmp_path / "inductor"
        cache.mkdir()
        monkeypatch.setattr(model_manager, "_locatable_inductor_cache_dir", lambda: cache)

        marker = model_manager._compile_marker("roformer")
        marker.write_text(json.dumps({"torch": "2.10.0+cpu"}), encoding="utf-8")

        self._installed_torch(monkeypatch, "2.11.0+cpu", None)

        state = model_manager._compile_state("roformer")

        assert state["compiled"] is False
        assert "different torch" in state["reason"]
