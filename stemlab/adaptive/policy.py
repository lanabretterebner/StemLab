"""Rules that decide whether a child stem may be split recursively."""

from __future__ import annotations

from .analysis import ChildAssessment

MAX_ADAPTIVE_DEPTH = 4


def should_offer_split(
    assessment: ChildAssessment,
    *,
    depth: int,
    category: str,
) -> bool:
    """Return whether the UI should expose another recursive split.

    This is intentionally conservative.  A fixed neural separator is allowed
    to live underneath a variable tree, but FI-STEM does not keep recursing
    simply because a model can always manufacture another pair of files.
    """
    if depth >= MAX_ADAPTIVE_DEPTH:
        return False
    if assessment.confidence < 0.48:
        return False
    if assessment.energy_ratio < 0.025:
        return False
    if assessment.estimated_source_count < 2:
        return False

    # The first implementation has a real recursive path for vocal groups.
    # Other categories are kept metadata-ready for the upcoming lead backend.
    return category in {
        "vocal.group",
        "vocal.harmony_group",
        "vocal.lead_group",
        "instrument.bed",
    }
