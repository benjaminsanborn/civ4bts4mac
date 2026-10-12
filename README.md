# Civ4 BtS Native

**Civilization IV: Beyond the Sword, running natively on Apple Silicon Macs.**

Aspyr's Mac version of Civilization IV: Beyond the Sword is a 32-bit Intel program, and current versions of macOS can't run those at all. This project is a recompiled version of that program that runs as a normal app on M1 and later Macs, with sharp graphics on Retina screens.

The app contains **no game data**. It reads everything (art, sound, maps, rules) from your own copy of the game, which you need to own and install from Steam.

> Not affiliated with or endorsed by Firaxis, 2K or Aspyr.

This is a **test release**. It plays well, but expect rough edges, and please [report what you find](#reporting-a-problem).

---

## What you need

- A Mac with **Apple Silicon** (M1, M2, M3, M4 or later) running **macOS 13 Ventura or later**.
- **Civilization IV: Beyond the Sword from Steam**, the Mac version ("Sid Meier's Civilization IV: Beyond the Sword", made by Aspyr, version 3.19).
  - Other versions won't work: not the Windows version, not retail discs, not other patches. The app checks for this exact version.
- About **1 GB** of free space, on top of the game itself (most of it for the converted movies).

## Setting it up

### 1. Install the game in Steam

1. Open Steam and install *Sid Meier's Civilization IV: Beyond the Sword* as usual.
2. Steam may warn that the game can't run on your Mac. **Install it anyway.** This app uses the files Steam downloads, not Steam's copy of the game program.

### 2. Download and install the app

1. Go to the [**Releases**](https://github.com/benjaminsanborn/civ4bts4mac/releases/latest) page and download the `.dmg` file under *Assets* (for example `Civ4-BtS-Native-0.2.0.dmg`).
2. Open the downloaded file and drag **Civ4 BtS Native** into the **Applications** folder.
3. Open **Civ4 BtS Native** from Applications.

### 3. First launch: the setup window

The first time, a setup window appears:

- **Game installation:** the app looks for your Steam copy and checks it.
  - All the checks should show a green tick.
  - If it can't find the game, click **Choose Folder…** and pick the folder that contains *Beyond the Sword*. In Steam, right-click the game and choose **Manage ▸ Browse local files** to find it.
- **BUG Mod (optional):** the popular *BTS Unaltered Gameplay* mod gives you a richer interface (a better scoreboard, city and unit details, a clock, reminders) without changing the rules.
  - Tick **Install BUG Mod 4.4** to have it downloaded (2.8 MB) and set up for you.
- Click **Play**.

From then on, the app starts the game directly.

**To get back to the setup window later** (for example to add or remove BUG Mod), hold the **⌥ Option** key while you open the app.

### Movies

The Beyond the Sword movies play straight away. The original game's wonder, era and victory movies use an old video format that macOS can no longer play. The first time you play, the app converts them in the background, which takes a minute or two. Until then, those movies are skipped.

## Playing

- **Interface size:** the game always draws at your screen's full resolution. The **Resolution** setting in *Options ▸ Graphics* sets how big the interface is:
  - a smaller resolution gives bigger buttons and text;
  - the map and text stay sharp at any size;
  - the default matches your Mac's normal scaling.
- **Zoom the map** with the scroll wheel, or with **⌥ Option + / ⌥ Option −**.
- **⌘ Command works like Ctrl** for the game's keyboard shortcuts (for example ⌘S saves).
- **⌘H** hides the game and **⌘Q** quits it. You can switch to other apps with ⌘Tab as usual.

**Where your files are**

| What | Where |
| --- | --- |
| Saves, settings, BUG Mod | `~/Library/Application Support/Civ4 BtS Native/Civilization IV Beyond the Sword` |
| Log files | `~/Library/Logs/Civ4 BtS Native` |

(`~` is your home folder. In Finder, use **Go ▸ Go to Folder…** and paste the path.)

## What needs testing

Every game played helps. These areas especially need people to try them:

- [ ] **Long games** on different map sizes and speeds, all the way to a victory: AI turns, autosaves, the victory movie and screens.
- [ ] **Loading old saves**, including ones made with Aspyr's version or on Windows.
- [ ] **Every screen:** advisors (F1–F10), the Civilopedia, diplomacy, the city screen, espionage, the tech tree, World Builder, Hall of Fame, replays.
- [ ] **Scenarios and mods** in *Advanced ▸ Load a Mod* and *Play a Scenario*, including community mods.
- [ ] **BUG Mod**, in both setups (always on, and as a mod).
- [ ] **Movies:** wonders, eras, victories, the intro.
- [ ] **Sound and music**, including after hiding and reopening the game.
- [ ] **Different Macs and screens:** MacBook Air and Pro, iMac, Mac mini or Studio with external monitors, several monitors at once.
- [ ] **Interface sizes:** try each resolution setting and look for anything misplaced, blurry or cut off.
- [ ] **Multiplayer** (LAN, hot seat, PBEM) has not been tested at all.
- [ ] **Speed:** tell us if anything feels slow, and how long late-game turns take on big maps.

If something works fine, that's useful to know too. A short note in an issue ("finished a Huge map game on an M2 Air, no problems") is welcome.

## Reporting a problem

1. **In the game, press ⌘ Command ⌥ Option B.** This does three things:
   - saves a screenshot;
   - shows the screenshot and the log file in Finder;
   - opens a new issue on GitHub, already filled in with your Mac's details and the end of the log.
2. Describe what you did and what went wrong, then drag the screenshot and `game.log` into the issue.
3. Attach a save game if the problem happens in a particular game.

- **If the game quits unexpectedly,** the app offers to do the same the next time you open it.
- **If the game won't start at all,** open the setup window (hold ⌥ Option while opening the app) and click **Report a Bug…**.
- **If neither works,** open an issue by hand on the [**Issues**](https://github.com/benjaminsanborn/civ4bts4mac/issues) page and attach `~/Library/Logs/Civ4 BtS Native/game.log`.

Reporting needs a free GitHub account.

## Contributing

Bug reports and test notes are the most useful help right now. If you'd like to work on the code, read on.

### How it works

- **Recompiler:** `tools/recomp` turns the game's 32-bit Intel machine code into C (*lifting*), which is compiled for Apple Silicon.
- **Runtime:** `runtime/` stands in for the old macOS libraries the game expects: Carbon windows and events, QuickDraw, AGL/OpenGL, QuickTime movies, files and threads.
- **Guest libraries:** `guest/` holds Python 2.6 and libstdc++, which the game links against and which are built for the game's own CPU and recompiled with it.
- **Setup app and packaging:** `tools/setup` (the setup window and launcher) and `tools/make_app.sh` (packaging).

### Building from source

You need an Apple Silicon Mac, **Xcode's command line tools** (`xcode-select --install`), about 12 GB of free disk space, and the Steam game installed. Then:

```sh
git clone https://github.com/benjaminsanborn/civ4bts4mac.git
cd civ4bts4mac
tools/build.sh        # setup window, then download third-party sources, recompile and build (a long first run)
tools/link_civ4.sh    # link build/civ4
tools/run.sh          # run it
```

`tools/build.sh` opens the same setup window first. It checks your build tools and lets you pick the game installation and BUG Mod.

For iterating on the runtime:
- `tools/try.sh` relinks and runs for a while in a separate test profile (`build/home`) without touching your own saves.
- `CIV4_FRAMES=dir` saves frames as images.
- `CIV4_INPUT` scripts clicks and keys.
- `tools/statehash.sh` checks that the recompiled game stays deterministic.

To package an app: `tools/make_app.sh` (add `--sign` and `--notarize` for a release).

### Sending changes

1. Open an issue first for anything big, so we can agree on an approach.
2. Keep pull requests focused, and describe how you tested them.
3. Match the style of the surrounding code: short C, comments that explain *why*.
4. Never commit game files (art, data, the game program): this project only ever reads them from the user's own copy.
