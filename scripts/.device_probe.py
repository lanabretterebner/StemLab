"""Print the torch device StemLab itself would pick.

Kept as a file rather than an inline -c string so the comparison scripts
cannot get its indentation wrong, which fails as an IndentationError and is
easily swallowed into a plausible-looking "cpu".
"""

from stemlab.device import pick_best_device

print(pick_best_device())
