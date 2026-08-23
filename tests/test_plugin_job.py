from pathlib import Path

from stemlab.plugin_job import (
    build_manifest,
    encode_path_for_udp,
    find_stem_file,
)


def test_manifest_selects_requested_stems(tmp_path):
    final_dir = tmp_path / "refined"
    final_dir.mkdir()

    for stem in ("vocals", "drums", "bass"):
        (final_dir / f"song_{stem}.wav").write_bytes(b"fake")

    source = tmp_path / "capture.wav"
    source.write_bytes(b"fake")

    data = build_manifest(
        output_dir=tmp_path,
        final_dir=final_dir,
        input_path=source,
        start_ppq=17.25,
        selected_stems=["vocals", "bass"],
        refined=True,
    )

    assert data["capture_start_ppq"] == 17.25
    assert data["engine"] == "roformer"
    assert [x["name"] for x in data["stems"]] == ["vocals", "bass"]
    assert all(Path(x["path"]).is_absolute() for x in data["stems"])


def test_udp_path_is_single_hex_atom(tmp_path):
    path = tmp_path / "folder with spaces" / "manifest.json"
    encoded = encode_path_for_udp(path)

    assert " " not in encoded
    assert len(encoded) % 2 == 0
    int(encoded, 16)
