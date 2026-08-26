from pathlib import Path


def test_source_analysis_clears_stale_cancel_sentinel_before_launch() -> None:
    source = Path('src/plugin/Source/PluginProcessor.cpp').read_text(encoding='utf-8')
    marker = 'const auto cancelFile = output.withFileExtension("cancel");'
    start = source.index(marker)
    launch = source.index('command.add("--input");', start)
    block = source[start:launch]
    assert 'cancelFile.existsAsFile()' in block
    assert 'cancelFile.deleteFile()' in block


def test_utility_thread_removes_cancel_sentinel_when_finishing() -> None:
    source = Path('src/plugin/Source/PluginProcessor.cpp').read_text(encoding='utf-8')
    start = source.index('void finish(int exitCode)', source.index('class StemLabUtilityThread'))
    block = source[start:source.index('StemLabAudioProcessor& owner;', start)]
    assert 'cancelFile.existsAsFile()' in block
    assert 'cancelFile.deleteFile()' in block
