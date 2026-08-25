"""Adaptive source analysis used by FI-STEM recursive separation.

Keep this package free of UI/DAW code.  It is deliberately small so the
separation policy can evolve without touching the JUCE plugin.
"""

from .analysis import AudioProfile, ChildAssessment, analyse_audio, assess_children
from .foreground import ForegroundSplit, split_foreground
from .policy import MAX_ADAPTIVE_DEPTH, should_offer_split

__all__ = [
    "AudioProfile",
    "ChildAssessment",
    "MAX_ADAPTIVE_DEPTH",
    "ForegroundSplit",
    "analyse_audio",
    "assess_children",
    "should_offer_split",
    "split_foreground",
]
