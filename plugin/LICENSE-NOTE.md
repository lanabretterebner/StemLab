# Plug-in licensing note

The StemLab engine source in the repository is MIT licensed.

This plug-in prototype uses JUCE for VST3/GUI/audio-host plumbing. The VST3 SDK
bundled in current JUCE releases is MIT licensed, but **JUCE itself has its own
dual licensing model** (open-source AGPLv3 or a commercial JUCE licence).

If you distribute a StemLab binary built using JUCE's open-source licence, make
sure the resulting distribution complies with the applicable JUCE/AGPL terms.
Alternatively, use a commercial JUCE licence or replace the JUCE layer with a
different VST3 framework/direct VST3 implementation.

This file is informational, not legal advice.
