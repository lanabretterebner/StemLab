"""What a finished separation leaves on disk.

A hybrid run with refinement writes four full sets of stems - one per model
under engines/, the fused baseline/, and the refined/ output - and used to
keep all four, quietly quadrupling the size of every job. Only the last one
is the answer.

The safety rule is the interesting half: the working folders are only removed
once the final folder is known to hold stems, so refinement producing nothing
cannot end with the only surviving copy being deleted.
"""

from __future__ import annotations

from pathlib import Path

from stemlab.audio import STEM_NAMES
from stemlab.pipeline import _discard_intermediates


def _stems(folder: Path) -> Path:
    folder.mkdir(parents=True, exist_ok=True)

    for stem in STEM_NAMES:
        (folder / f"{stem}.wav").write_bytes(b"RIFF")

    return folder


def _job(tmp_path: Path, *, refine: bool, hybrid: bool = True):
    output = tmp_path / "job_1"
    baseline = _stems(output / "baseline")
    final = _stems(output / "refined") if refine else baseline

    if hybrid:
        _stems(output / "engines" / "roformer")
        _stems(output / "engines" / "demucs")

    return output, baseline, final


class TestOnlyTheFinalOutputSurvives:
    def test_a_hybrid_refine_run_keeps_one_set(self, tmp_path):
        output, baseline, final = _job(tmp_path, refine=True)

        _discard_intermediates(output, baseline, final, print)

        assert final.is_dir()
        assert not baseline.exists()
        assert not (output / "engines").exists()

    def test_without_refinement_the_baseline_is_the_answer(self, tmp_path):
        output, baseline, final = _job(tmp_path, refine=False)

        _discard_intermediates(output, baseline, final, print)

        # baseline IS final here, so it has to survive; only the per-model
        # folders were working files.
        assert final.is_dir()
        assert sorted(p.name for p in final.glob("*.wav"))
        assert not (output / "engines").exists()

    def test_a_single_model_run_has_nothing_to_remove(self, tmp_path):
        output, baseline, final = _job(tmp_path, refine=False, hybrid=False)

        _discard_intermediates(output, baseline, final, print)

        assert final.is_dir()


class TestItNeverDeletesTheLastCopy:
    def test_an_empty_final_folder_stops_it(self, tmp_path):
        output, baseline, _ = _job(tmp_path, refine=True)
        empty = output / "refined"

        for path in empty.glob("*.wav"):
            path.unlink()

        messages: list[str] = []
        _discard_intermediates(output, baseline, empty, messages.append)

        # Refinement produced nothing. The baseline is now the only set of
        # stems in existence and removing it would lose the whole job.
        assert baseline.is_dir()
        assert (output / "engines").is_dir()
        assert any("no stems" in message for message in messages)

    def test_a_missing_final_folder_stops_it(self, tmp_path):
        output, baseline, _ = _job(tmp_path, refine=True)

        _discard_intermediates(output, baseline, output / "never-written", print)

        assert baseline.is_dir()
        assert (output / "engines").is_dir()

    def test_a_removal_that_fails_does_not_fail_the_job(self, tmp_path, monkeypatch):
        # Tidying up is never worth failing a finished separation over, so the
        # failure has to be forced: on the happy path nothing here can throw,
        # and a test that only walks the happy path proves nothing about the
        # handler it claims to cover.
        from stemlab import pipeline

        def unwritable(*_args, **_kwargs):
            raise PermissionError("read-only file system")

        monkeypatch.setattr(pipeline.shutil, "rmtree", unwritable)

        output, baseline, final = _job(tmp_path, refine=True)
        messages: list[str] = []

        _discard_intermediates(output, baseline, final, messages.append)

        assert final.is_dir()
        assert baseline.is_dir()
        assert any("still there" in message for message in messages)


class TestTheEscapeHatch:
    def test_keeping_them_is_one_variable(self, tmp_path, monkeypatch):
        monkeypatch.setenv("STEMLAB_KEEP_INTERMEDIATES", "1")

        output, baseline, final = _job(tmp_path, refine=True)

        _discard_intermediates(output, baseline, final, print)

        assert baseline.is_dir()
        assert (output / "engines").is_dir()

    def test_an_unset_variable_does_not_keep_them(self, tmp_path, monkeypatch):
        monkeypatch.delenv("STEMLAB_KEEP_INTERMEDIATES", raising=False)

        output, baseline, final = _job(tmp_path, refine=True)

        _discard_intermediates(output, baseline, final, print)

        assert not baseline.exists()
