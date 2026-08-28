"""The prune step at the end of scripts/linux/install_backend.sh.

A test cannot build a real Engine - the installer downloads a relocatable
CPython and then pip-installs a gigabyte of torch - so these drive the
installer's --prune-only hook against a synthetic tree carrying exactly the
cases the selection has to get right: numpy/testing sitting beside
numpy/_core/tests, a *file* called "tests", a symlink called "tests", the real
Tcl/Tk layout of the pinned interpreter, a real ELF shared object, and one that
only has the name.

The half this cannot cover is whether a pruned Engine still works, which is why
install_backend.sh runs the prune immediately before its torch/torchaudio
import check: every real install re-proves the strip pass. That check was run
once by hand on a built cpu Engine - torch, torchaudio, demucs, bs_roformer and
audio-separator all imported afterwards, and htdemucs separated a synthetic
6-second mix into four stems.
"""

import ctypes
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "scripts" / "linux" / "install_backend.sh"
SCRIPT = INSTALLER.read_text(encoding="utf-8")

pytestmark = pytest.mark.skipif(
    sys.platform != "linux" or shutil.which("bash") is None,
    reason="install_backend.sh is a Linux bash script",
)

# A "tests" directory nested inside a package. All four must go.
REMOVED_TESTS = [
    "numpy/_core/tests",
    "scipy/sparse/linalg/_eigen/tests",
    "sklearn/utils/tests",
    "realpkg/tests",
]

# Everything the selection has to leave alone. numpy/testing is the one that
# matters most - numpy.ma.testutils, scipy.special._testutils and
# sklearn.utils._testing all import it at run time.
KEPT_PATHS = [
    "numpy/testing/__init__.py",
    "numpy/testing/_private/utils.py",
    "numpy/testing/tests/test_utils.py",
    "numpy/__pycache__/__init__.cpython-311.pyc",
    "pkg/testing/helpers.py",
    "pkg_with_testing_file/testing",
    "pkg_with_tests_file/tests",
    "tests/conftest.py",
    "realpkg/__init__.py",
]

# Every Tcl/Tk path the pinned interpreter actually ships, read out of
# cpython-3.11.13+20250818-x86_64-unknown-linux-gnu-install_only.tar.gz.
TCLTK_PATHS = [
    "tcl8/8.4/platform-1.0.19.tm",
    "tcl8.6/init.tcl",
    "tk8.6/pkgIndex.tcl",
    "itcl4.2.4/pkgIndex.tcl",
    "thread2.8.9/pkgIndex.tcl",
    "libtcl8.6.so",
    "libtk8.6.so",
    "python3.11/tkinter/__init__.py",
    "python3.11/lib-dynload/_tkinter.cpython-311-x86_64-linux-gnu.so",
]

# Their neighbours under lib/, from the same tarball.
LIB_SURVIVORS = [
    "libpython3.11.so",
    "libpython3.11.so.1.0",
    "libpython3.so",
    "pkgconfig/python-3.11.pc",
    "python3.11/json/__init__.py",
]

NOT_AN_ELF = "a file with a .so name and nothing else\n"

PROBE_SOURCE = "int stemlab_answer(void) { return 42; }\n"


def _write(path: Path, text: str = "x\n") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def _compile_probe(dest: Path) -> bool:
    """Build a real shared library exporting stemlab_answer() -> 42."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    source = dest.parent / "_stemlab_probe.c"
    source.write_text(PROBE_SOURCE, encoding="utf-8")

    # -g so there is debug information for --strip-unneeded to actually take.
    built = subprocess.run(
        ["cc", "-shared", "-fPIC", "-g", "-o", str(dest), str(source)],
        capture_output=True,
        text=True,
    )
    source.unlink()
    return built.returncode == 0


def _build_engine(root: Path) -> Path:
    """Lay out a synthetic Engine and return its site-packages."""
    lib = root / "lib"
    site_packages = lib / "python3.11" / "site-packages"

    root.mkdir(parents=True, exist_ok=True)
    (root / ".stemlab-engine").touch()

    for relative in REMOVED_TESTS:
        _write(site_packages / relative / "test_thing.py")
    for relative in KEPT_PATHS:
        _write(site_packages / relative)

    # A symlink named "tests". find is given neither -L nor a symlink-following
    # -type, so this is neither followed into numpy/testing nor deleted.
    (site_packages / "linkpkg").mkdir(parents=True, exist_ok=True)
    os.symlink("../numpy/testing/tests", site_packages / "linkpkg" / "tests")

    # Tcl/Tk exactly as the pinned python-build-standalone tarball lays it
    # out (cpython-3.11.13+20250818, x86_64). itcl4.2.4 and thread2.8.9 match
    # none of the tcl*/tk* globs and are reachable only through pkgIndex.tcl.
    for relative in TCLTK_PATHS:
        _write(lib / relative)

    # Neighbours of those globs, also taken from the real tarball. None of
    # them carries a pkgIndex.tcl, so the marker sweep cannot reach them.
    for relative in LIB_SURVIVORS:
        _write(lib / relative)

    return site_packages


def _run_prune(root: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(INSTALLER), "--prune-only", str(root)],
        capture_output=True,
        text=True,
        cwd=str(ROOT),
    )


@pytest.fixture(scope="module")
def pruned(tmp_path_factory):
    """Build the synthetic Engine once and run the real prune over it."""
    root = tmp_path_factory.mktemp("engine-prune") / "Engine"
    site_packages = _build_engine(root)

    probe = site_packages / "realpkg" / "_probe.so"
    versioned = site_packages / "realpkg" / "libprobe.so.1.2.3"
    compiled = _compile_probe(probe) and _compile_probe(versioned)
    if compiled:
        os.symlink("libprobe.so.1.2.3", site_packages / "realpkg" / "libprobe.so")
    _write(site_packages / "realpkg" / "fake.so", NOT_AN_ELF)

    before = {path.name: path.stat().st_size for path in (probe, versioned) if path.exists()}

    return {
        "root": root,
        "site_packages": site_packages,
        "process": _run_prune(root),
        "before": before,
        "compiled": compiled,
    }


def test_prune_completes(pruned):
    assert pruned["process"].returncode == 0, pruned["process"].stderr


def test_bundled_test_suites_are_removed(pruned):
    site_packages = pruned["site_packages"]
    for relative in REMOVED_TESTS:
        assert not (site_packages / relative).exists(), f"{relative} survived"

    # The installer announces every directory it deletes, so the selection is
    # observable from the real destructive code path rather than a dry run
    # that could drift away from it.
    announced = {
        line.split("removed ", 1)[1]
        for line in pruned["process"].stdout.splitlines()
        if "removed " in line
    }
    assert announced == {f"./{relative}" for relative in REMOVED_TESTS}


def test_numpy_testing_survives(pruned):
    # numpy.testing is public API imported at run time by numpy.ma.testutils,
    # scipy.special._testutils and sklearn.utils._testing. A "test*" glob
    # deletes it and every one of those breaks.
    site_packages = pruned["site_packages"]
    assert (site_packages / "numpy" / "testing" / "__init__.py").is_file()
    assert (site_packages / "numpy" / "testing" / "_private" / "utils.py").is_file()

    # numpy/testing/tests is the only "tests" directory in a real
    # site-packages that lives under a "testing" segment, and it goes with it.
    assert (site_packages / "numpy" / "testing" / "tests" / "test_utils.py").is_file()


def test_everything_outside_the_selection_survives(pruned):
    site_packages = pruned["site_packages"]
    for relative in KEPT_PATHS:
        assert (site_packages / relative).exists(), f"{relative} was deleted"


def test_a_file_called_tests_is_not_a_test_suite(pruned):
    # -type d. A package shipping a module or data file named "tests" or
    # "testing" keeps it.
    site_packages = pruned["site_packages"]
    assert (site_packages / "pkg_with_tests_file" / "tests").is_file()
    assert (site_packages / "pkg_with_testing_file" / "testing").is_file()


def test_a_tests_symlink_is_neither_followed_nor_removed(pruned):
    link = pruned["site_packages"] / "linkpkg" / "tests"
    assert link.is_symlink()
    assert (link / "test_utils.py").is_file()


def test_a_top_level_tests_directory_survives(pruned):
    # -mindepth 2. site-packages/tests is importable as "tests"; only suites
    # nested inside a package are bundled test suites.
    assert (pruned["site_packages"] / "tests" / "conftest.py").is_file()


def test_an_ancestor_directory_named_testing_does_not_disable_the_prune(tmp_path):
    # "-not -path '*/testing/*'" matches ancestors as happily as it matches
    # the segment it is aimed at. Run against an absolute root, an Engine
    # installed under ~/testing/ selects nothing and prunes nothing - which
    # looks exactly like a successful prune.
    root = tmp_path / "testing" / "Engine"
    site_packages = _build_engine(root)

    process = _run_prune(root)
    assert process.returncode == 0, process.stderr

    for relative in REMOVED_TESTS:
        assert not (site_packages / relative).exists(), f"{relative} survived under testing/"
    assert (site_packages / "numpy" / "testing" / "__init__.py").is_file()


@pytest.mark.skipif(shutil.which("cc") is None, reason="needs a C compiler")
def test_a_stripped_shared_object_still_loads_and_works(pruned):
    if not pruned["compiled"]:
        pytest.skip("cc could not build the probe shared object")

    probe = pruned["site_packages"] / "realpkg" / "_probe.so"
    assert probe.stat().st_size < pruned["before"]["_probe.so"]

    # The point of --strip-unneeded rather than plain strip: the library is
    # still loadable and its exported symbol still resolves.
    loaded = ctypes.CDLL(str(probe))
    assert loaded.stemlab_answer() == 42


@pytest.mark.skipif(
    shutil.which("cc") is None or shutil.which("readelf") is None,
    reason="needs a C compiler and readelf",
)
def test_strip_keeps_the_dynamic_symbol_table(pruned):
    if not pruned["compiled"]:
        pytest.skip("cc could not build the probe shared object")

    probe = pruned["site_packages"] / "realpkg" / "_probe.so"
    sections = subprocess.run(
        ["readelf", "-S", str(probe)], capture_output=True, text=True
    ).stdout
    assert ".dynsym" in sections
    assert ".symtab" not in sections


@pytest.mark.skipif(shutil.which("cc") is None, reason="needs a C compiler")
def test_versioned_shared_objects_are_stripped_through_the_real_file(pruned):
    if not pruned["compiled"]:
        pytest.skip("cc could not build the probe shared object")

    realpkg = pruned["site_packages"] / "realpkg"

    # *.so* catches libfoo.so.1.2.3, which is how most vendored libraries in
    # site-packages are named.
    assert realpkg.joinpath("libprobe.so.1.2.3").stat().st_size < pruned["before"][
        "libprobe.so.1.2.3"
    ]

    # -type f: the .so symlink beside it is not visited a second time, and is
    # still a symlink afterwards.
    assert realpkg.joinpath("libprobe.so").is_symlink()


def test_a_so_that_is_not_an_elf_object_is_left_alone(pruned):
    # site-packages contains .so names that are not ELF at all. strip fails on
    # them, that failure is swallowed per file, and the install continues.
    fake = pruned["site_packages"] / "realpkg" / "fake.so"
    assert fake.read_text(encoding="utf-8") == NOT_AN_ELF
    assert pruned["process"].returncode == 0


def test_tcl_tk_and_tkinter_are_removed(pruned):
    lib = pruned["root"] / "lib"
    for relative in TCLTK_PATHS:
        assert not (lib / relative).exists(), f"{relative} survived"

    # itcl4.2.4 and thread2.8.9 match no tcl*/tk* glob; only the pkgIndex.tcl
    # sweep reaches them, and the whole directory has to go, not just the file.
    assert not (lib / "itcl4.2.4").exists()
    assert not (lib / "thread2.8.9").exists()


def test_the_tcl_globs_do_not_take_the_interpreter_with_them(pruned):
    lib = pruned["root"] / "lib"
    for relative in LIB_SURVIVORS:
        assert (lib / relative).is_file(), f"{relative} was deleted"


def test_bytecode_is_deliberately_kept(pruned):
    """__pycache__ survives, and this test exists so it keeps surviving.

    A built cpu Engine carries 189 MB of bytecode out of 1431 MB (13.2%).
    Removing it costs a median 3049 ms per fresh process against 1362 ms
    with it - measured on numpy + scipy + sklearn + joblib, nine runs each,
    importing numpy, scipy.signal, scipy.interpolate, sklearn.decomposition
    and sklearn.cluster, with PYTHONDONTWRITEBYTECODE=1 standing in for an
    install directory the Engine cannot write to. That is +1687 ms on every
    job for four packages, before torch and demucs are in the picture, and the
    plugin starts a fresh Engine process per job. The megabytes are not worth
    the latency.
    """
    bytecode = pruned["site_packages"] / "numpy" / "__pycache__" / "__init__.cpython-311.pyc"
    assert bytecode.is_file()


def test_prune_refuses_a_tree_this_installer_does_not_own(tmp_path):
    root = tmp_path / "NotAnEngine"
    site_packages = _build_engine(root)
    (root / ".stemlab-engine").unlink()

    process = _run_prune(root)
    assert process.returncode != 0
    assert ".stemlab-engine" in process.stderr

    for relative in REMOVED_TESTS:
        assert (site_packages / relative / "test_thing.py").is_file()
    assert (root / "lib" / "tcl8.6" / "init.tcl").is_file()


def test_prune_refuses_a_directory_that_is_not_there(tmp_path):
    process = _run_prune(tmp_path / "absent")
    assert process.returncode != 0
    assert "Refusing to prune" in process.stderr


def test_shared_objects_are_never_plain_stripped():
    # Plain strip on a shared object bets the Engine on what a particular
    # strip build happens to leave behind; --strip-unneeded is defined to keep
    # everything relocation processing needs.
    assert "strip --strip-unneeded" in SCRIPT
    assert "strip --strip-all" not in SCRIPT


def test_the_selection_never_globs_test_star():
    assert "-name tests" in SCRIPT
    for glob in ("-name 'test*'", '-name "test*"', "-name test*"):
        assert glob not in SCRIPT


def test_prune_runs_before_the_engine_import_check():
    # The torch/torchaudio import check below the prune is the only
    # end-to-end proof that a pruned Engine still loads its shared objects.
    # Pruning after it would ship a broken strip in silence.
    assert SCRIPT.index('prune_engine "$DEST"') < SCRIPT.index("import torchaudio")


def test_prune_only_with_an_empty_value_refuses_instead_of_installing():
    """The flag was gated on its VALUE, so `--prune-only ''` skipped the hook.

    Falling through meant a full ~1.4 GB network install that also rewrites
    the engine pointer. It is not hypothetical: it fired once during
    development and built a complete Engine nobody asked for.
    """
    result = subprocess.run(
        ["bash", str(INSTALLER), "--prune-only", ""],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 2, result.stdout + result.stderr
    assert "needs a directory" in (result.stdout + result.stderr)


def test_the_tcl_sweep_only_removes_names_it_recognises(tmp_path):
    """The pkgIndex.tcl sweep rm -rf's a directory it finds by marker file.

    Bounding it by name costs one `case` and removes the possibility that a
    future interpreter shipping a pkgIndex.tcl somewhere unexpected takes part
    of the stdlib with it.
    """
    root = tmp_path / "Engine"
    lib = root / "lib"
    (root / ".stemlab-engine").parent.mkdir(parents=True, exist_ok=True)
    (root / ".stemlab-engine").write_text("marker\n")
    (root / "lib" / "python3.11" / "site-packages").mkdir(parents=True)

    # Recognised Tcl package directories: must go.
    for name in ("itcl4.2.4", "thread2.8.9", "tk8.6"):
        (lib / name).mkdir(parents=True)
        (lib / name / "pkgIndex.tcl").write_text("# tcl\n")

    # Not a Tcl package name, but carries the same marker: must survive.
    (lib / "site-tools").mkdir(parents=True)
    (lib / "site-tools" / "pkgIndex.tcl").write_text("# not tcl\n")
    (lib / "site-tools" / "keepme.py").write_text("x = 1\n")

    _run_prune(root)

    for name in ("itcl4.2.4", "thread2.8.9", "tk8.6"):
        assert not (lib / name).exists(), name
    assert (lib / "site-tools" / "keepme.py").is_file()


def test_a_failing_removal_does_not_abort_the_install(tmp_path):
    """Under set -e an unremovable path used to kill a finished install.

    The prune runs after every pip install and immediately before the engine
    pointer is written, so aborting there throws away all of the expensive
    work and leaves the Engine undiscoverable.

    The failure is injected by putting an ``rm`` that always fails ahead of
    the real one on PATH, rather than by making a directory read-only. Root
    ignores directory permissions, so the permissions version of this test
    passes without ever reaching the tolerance it is meant to check - and
    both CI and this container run as root.
    """
    root = tmp_path / "Engine"
    sp = root / "lib" / "python3.11" / "site-packages"
    (sp / "pkg" / "tests").mkdir(parents=True)
    (sp / "pkg" / "tests" / "test_x.py").write_text("x = 1\n")
    (root / ".stemlab-engine").write_text("marker\n")
    (root / "lib" / "tcl8.6").mkdir(parents=True)

    stub = tmp_path / "stub-bin"
    stub.mkdir()
    (stub / "rm").write_text("#!/bin/sh\nexit 1\n")
    (stub / "rm").chmod(0o755)

    env = dict(os.environ, PATH=f"{stub}{os.pathsep}{os.environ['PATH']}")
    result = subprocess.run(
        ["bash", str(INSTALLER), "--prune-only", str(root)],
        capture_output=True,
        text=True,
        env=env,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    # Proof the stub was actually reached: nothing was removed, and the
    # install still reported success.
    assert (sp / "pkg" / "tests" / "test_x.py").is_file()
    assert (root / "lib" / "tcl8.6").is_dir()
