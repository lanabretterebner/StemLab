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
    to live underneath a variable tree, but StemLab does not keep recursing
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

    # Only the categories a recursive split can actually be routed to:
    # run_recursive sends "vocal." to split_vocals and "instrument." to
    # split_lead_group, and offering a split that the router would refuse
    # would put a button in the tree that fails when it is pressed.
    return category in {
        "vocal.group",
        "vocal.harmony_group",
        "instrument.bed",
    }
