"""Tells BUG Mod where the game's user folder is.

BUG looks for CivilizationIV.ini, its settings and saves under ~/Documents on a
Mac, but this build keeps them in ~/Library/Application Support (so the game
never needs access to Documents). BUG imports this module when it starts, as
its Info/CvAltRoot.py describes; a CvAltRoot.py in the user's own CustomAssets
or mod takes precedence over this one.
"""

import os

rootDir = os.path.join(os.environ.get("HOME", ""), "Library", "Application Support",
                       "Civ4 BtS Native", "Civilization IV Beyond the Sword")

# BUG then takes ~/Documents as the user folder and looks for its settings
# there first, which would ask for Documents access. Point it at the folder
# above the root instead.
try:
    import BugPath
    _setUserDir = BugPath.setUserDir
    BugPath.setUserDir = lambda dir: _setUserDir(os.path.dirname(rootDir))
except (ImportError, AttributeError):
    pass
