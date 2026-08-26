"""Tests for surfacing model downloads in the plugin's status area.

The release bundles carry the Engine, not the weights, so the first use of
any model downloads it. A download that is not reported looks exactly like a
hang: the status text sits still and the bar sits behind it, for as long as a
multi-hundred-megabyte transfer takes.
"""

from __future__ import annotations

import io
import sys

from stemlab.runtime import report_downloads

# One frame of a real tqdm byte-transfer bar, carriage-returned like the
# genuine article rather than newline-terminated.
BAR = "\rmodel.ckpt: {percent:3d}%|##   | {got}/2.70G [00:05<00:45, 53.9MB/s]"


class TestReportDownloads:
    def test_byte_transfer_bars_become_percentages(self):
        seen: list[float] = []
        sink = io.StringIO()
        saved = sys.stdout
        sys.stdout = sink
        try:
            with report_downloads(seen.append):
                for percent, got in ((10, "270M"), (45, "1.21G"), (100, "2.70G")):
                    sys.stdout.write(BAR.format(percent=percent, got=got))
        finally:
            sys.stdout = saved

        assert seen == [10.0, 45.0, 100.0]

    def test_separation_bars_are_not_downloads(self):
        # The separation bar counts seconds, not bytes. Reporting it as a
        # download would put "Downloading" on screen for the whole job.
        seen: list[float] = []
        saved = sys.stdout
        sys.stdout = io.StringIO()
        try:
            with report_downloads(seen.append):
                sys.stdout.write("50%|####| 5.85/11.7 [00:10<00:10,  1.75s/seconds]\n")
        finally:
            sys.stdout = saved

        assert seen == []

    def test_the_output_still_reaches_the_log(self):
        sink = io.StringIO()
        saved = sys.stdout
        sys.stdout = sink
        try:
            with report_downloads(lambda _percent: None):
                sys.stdout.write(BAR.format(percent=100, got="2.70G"))
        finally:
            sys.stdout = saved

        assert "2.70G/2.70G" in sink.getvalue()

    def test_repeated_percentages_are_reported_once(self):
        # tqdm redraws far faster than the percentage changes, and every
        # report crosses a pipe into the plugin.
        seen: list[float] = []
        saved = sys.stdout
        sys.stdout = io.StringIO()
        try:
            with report_downloads(seen.append):
                for _ in range(20):
                    sys.stdout.write(BAR.format(percent=42, got="1.13G"))
        finally:
            sys.stdout = saved

        assert seen == [42.0]

    def test_streams_are_restored_even_when_the_body_raises(self):
        saved_out, saved_err = sys.stdout, sys.stderr
        try:
            with report_downloads(lambda _percent: None):
                raise RuntimeError("download failed")
        except RuntimeError:
            pass

        assert sys.stdout is saved_out
        assert sys.stderr is saved_err

    def test_without_a_callback_it_leaves_the_streams_alone(self):
        saved_out = sys.stdout
        with report_downloads(None):
            assert sys.stdout is saved_out


class TestRecursiveModelLoading:
    def test_loading_a_model_reports_its_download(self, monkeypatch):
        """The status area names the download while load_model blocks."""
        from stemlab import recursive

        reported: list[tuple[float, str]] = []

        class FakeSeparator:
            def load_model(self, model_filename: str) -> None:
                # audio-separator downloads here, writing a tqdm bar to this
                # process's own stdout - there is no pipe for the plugin.
                for percent in (0, 50, 100):
                    sys.stdout.write(
                        BAR.format(percent=percent, got=f"{percent * 9}M")
                    )

        saved = sys.stdout
        sys.stdout = io.StringIO()
        try:
            recursive._load_model(
                FakeSeparator(),
                "MDX23C-DrumSep-aufr33-jarredou.ckpt",
                "drum separation",
                lambda percent, stage: reported.append((percent, stage)),
            )
        finally:
            sys.stdout = saved

        assert [stage for _percent, stage in reported] == [
            "Downloading the drum separation model (0%)",
            "Downloading the drum separation model (50%)",
            "Downloading the drum separation model (100%)",
        ]

        # The bar creeps rather than jumping to the separation band, which
        # starts at 12.
        percents = [percent for percent, _stage in reported]
        assert percents == sorted(percents)
        assert 4.0 <= percents[0] and percents[-1] <= 11.0

    def test_a_cached_model_reports_no_download(self, monkeypatch):
        from stemlab import recursive

        reported: list[tuple[float, str]] = []

        class FakeSeparator:
            def load_model(self, model_filename: str) -> None:
                # Already on disk: audio-separator says so and downloads
                # nothing, so the status must not claim otherwise.
                sys.stdout.write("Loading model from cache\n")

        saved = sys.stdout
        sys.stdout = io.StringIO()
        try:
            recursive._load_model(
                FakeSeparator(),
                "MDX23C-DrumSep-aufr33-jarredou.ckpt",
                "drum separation",
                lambda percent, stage: reported.append((percent, stage)),
            )
        finally:
            sys.stdout = saved

        assert reported == []
