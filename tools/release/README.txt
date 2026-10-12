Civ4 BtS Native 0.2.0 (test release)
=====================================

Civilization IV: Beyond the Sword, running natively on Apple Silicon Macs.

Aspyr's Mac version of the game is 32-bit Intel code, which current macOS
can't run. This app is a recompiled version of that program that runs as a
normal Apple Silicon app. It contains no game data: it reads everything
(art, sound, maps, rules) from your own installed copy.

Not affiliated with or endorsed by Firaxis, 2K or Aspyr.


What you need
-------------
- An Apple Silicon Mac (M1 or later) with macOS 13 Ventura or later.
- Your own copy of Civilization IV: Beyond the Sword for Mac from Steam:
  "Sid Meier's Civilization IV: Beyond the Sword", the Aspyr Mac version,
  version 3.19. Install it in Steam as usual. Steam may say it can't run it,
  but the files it downloads are what this app uses. Other versions (Windows,
  retail discs, other patches) won't work: the app checks for this exact build.


Installing
----------
1. Open the disk image and drag "Civ4 BtS Native" to Applications.
2. Open it. The first time, a setup window appears:
   - It finds your Steam installation and checks it (all checks should be
     green). If it isn't found, click "Choose Folder…" and pick the folder
     that contains "Beyond the Sword" (in Steam: right-click the game >
     Manage > Browse local files).
   - Optionally tick "Install BUG Mod 4.4", a popular interface mod that
     doesn't change the rules. It's downloaded from SourceForge (2.8 MB).
   - Click Play.
3. After that the game starts directly. To get back to the setup window
   (for example to add or remove BUG Mod), hold the Option key while opening
   the app.
4. The first time you play, the original game's movies (wonders, eras,
   victories) are converted in the background, which takes a minute or two;
   until then those movies are skipped. The Beyond the Sword movies play
   straight away.


Screen and interface size
-------------------------
The game always draws at your screen's full resolution. The resolution you
pick in Options > Graphics sets the size of the interface: a smaller
resolution gives bigger buttons and text, with the map and text just as
sharp. The default matches macOS's own scaling (on a Retina screen, half the
screen's pixel size).


Where things are
----------------
- Saves, settings, BUG Mod:
  ~/Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the Sword
  (a folder left in ~/Documents by version 0.1.0 is moved there)
- Log files: ~/Library/Logs/Civ4 BtS Native (game.log, and game-previous.log
  from the time before)
- Converted movies: ~/Library/Application Support/Civ4 BtS Native/movies
- Your choices from the setup window:
  ~/Library/Application Support/Civ4 BtS Native/config.json


Known limitations in this version
---------------------------------
- Multiplayer hasn't been tested.
- Late-game AI turns on Huge maps with many civilizations can take 30-40
  seconds; the window stays responsive meanwhile.


Reporting problems
------------------
In the game, press Command-Option-B. It saves a screenshot, shows it and the
log file in Finder, and opens a new issue on GitHub (a free GitHub account is
needed) already filled in with your Mac's details and the end of the log:
describe what happened and drag in the screenshot and log. If the game quits
unexpectedly, the app offers the same the next time it opens, and the setup
window has a "Report a Bug…" button.

Issues: https://github.com/benjaminsanborn/civ4bts4mac/issues
