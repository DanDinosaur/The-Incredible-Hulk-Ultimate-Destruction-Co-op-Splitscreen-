# The Incredible Hulk: Ultimate Destruction — Co-op Split Screen (PC)

Play the whole story campaign of **The Incredible Hulk: Ultimate Destruction** with a friend, two Hulks on one screen, on Windows.

This is a co-op mod built on **[Hulk Recompiled](https://github.com/lookingcoding/hulk-ultimate-destruction-recomp-xbox-ver-)**, the native Windows port and static recompilation of the original Xbox game. It adds a second player, split screen, co-op respawns, an in-game co-op menu and a Red Hulk skin for either player.

> [!IMPORTANT]
> **No game files are included.** You need your own legal copy of the original Xbox game, **North American release (USA, Asia)**, as a disc image (`.iso`) or extracted files. The download contains only the mod's program and the compiler that builds the game's code from *your* disc the first time you start it.

> [!NOTE]
> This is a **beta**. The campaign is playable start to finish in co-op, but there are rough edges — see [Known issues](#known-issues).

---

## Features

- **Second player (P2):** a second Hulk on the second controller, with full controls, his own camera (right stick) and the same health and Critical Mass upgrades as P1.
- **Split screen:** side by side (P1 left, P2 right) or top and bottom, chosen in the co-op menu. Each half has its own HUD, health bar and radar. On the radar, P2's arrow is blue so you can always find each other.
- **Co-op respawns** (like Turok or Force Unleashed co-op): when a player goes down, his half of the screen goes dark and his partner carries on. After a few seconds (9 by default) he comes back beside his partner. If **both** players go down, it's the game's normal death and you restart from the checkpoint.
- **P2 can start missions too:** P2 can walk into a mission star or jump-out circle, not only P1.
- **Red Hulk:** P1, P2 or neither can be drawn as Red Hulk (see below).
- **Per-player camera distance** in side-by-side split (D-pad up/down).
- **Co-op menu** in game for both players (see below). Settings are saved to `coop.ini`.
- Everything from Hulk Recompiled still works: Display Options (resolution, frame rate, FOV, render distance), Cheats, keyboard and mouse, mods. See the [Hulk Recompiled README](https://github.com/lookingcoding/hulk-ultimate-destruction-recomp-xbox-ver-#readme).

---

## What you need

- Windows 10 or 11, 64-bit.
- **Two controllers** (Xbox 360 / Xbox One / Xbox Series or any XInput pad). Keyboard and mouse can drive one player.
- Your own copy of **The Incredible Hulk: Ultimate Destruction (Xbox, USA/Asia release)** as an `.iso` file, or the extracted game files with `default.xbe`. The European/PAL release is **not** supported by the co-op build.
- About 1 GB of free disk space for the first-launch build, plus the size of your ISO (about 2.3 GB).

## Install

1. Download `HulkCoop-<version>-windows-x64.zip` from **[Releases](../../releases)** and extract it to a folder of your choice, for example `D:\Games\Hulk Coop`. Keep the folder structure.
2. Put your game's `.iso` (any file name) into the `assets` folder. Or put the extracted game files there, with `default.xbe` directly in `assets`.
3. Plug in both controllers. The first controller Windows connected is P1, the second is P2 (you can change this in `coop.ini`).
4. Double-click **`HulkRecompiledUSA.exe`**.
   - **First launch only:** the game builds its code from your disc using the included `compiler` folder. This takes a few minutes; let it finish. Later launches start straight away.
5. Start or load a story game. P2 joins about three seconds after the level starts and split screen turns on (top and bottom at first; switch to side by side in the co-op menu, see below).

Saves, settings and logs are kept next to the exe (`hdd\`, `settings.toml`, `coop.ini`, `logs\`).

**Updating:** extract the new version over the old folder. Your `assets`, saves and `coop.ini` are kept. If the game says its code needs rebuilding, let it do that once.

### If something goes wrong

- *"The game in assets is not supported" or similar:* check that your ISO is the **USA/Asia** Xbox release.
- *The first-launch build fails:* make sure the `compiler` folder was extracted completely and that you have about 1 GB free. Antivirus software sometimes blocks `clang.exe` / `lld-link.exe`; allow them.
- *P2 doesn't move:* check the pad numbers in `coop.ini` (`p1_pad`, `p2_pad`). Pads count 1–4 in the order Windows connected them.
- Logs are in `logs\`. Please attach the newest `hulk_NNN.log` when you report a problem.

---

## Controls

| Action | How |
|---|---|
| Play | Both players use the game's normal controls on their own controller. |
| Each player's camera | Right stick on that player's controller. |
| Open / close the **co-op menu** | **RB + D-pad Up** (each player has his own menu, drawn in his half) — or **F1** on the keyboard for the player the keyboard drives |
| Camera closer / farther (side-by-side split only) | **D-pad Up / Down** on that player's controller (hold) |
| Swap which player the keyboard drives | **F8** |
| Pause, menus | Either controller, as normal. |

## Co-op menu

Open it with **RB + D-pad Up**. Move with the **D-pad or left stick**, change a value with **left/right**, press **A** to choose, **B** to close. While a menu is open the game ignores that player's controller. Changes are saved to `coop.ini` right away.

| Option | What it does |
|---|---|
| **Teleport P2 to P1** | Puts P2 next to P1 (handy if you get separated or stuck). |
| **Teleport P1 to P2** | Puts P1 next to P2. |
| **Split screen** | *Side by side* (P1 left, P2 right) or *Top / bottom* (P1 top). Side by side looks best on a big widescreen TV. |
| **P1 god mode / P2 god mode** | That player can't lose health. Separate for each player. |
| **Red Hulk** | *P1*, *P2* or *Off*: which player is drawn as Red Hulk. |
| **Test - draw P2's half first** | A developer test switch. Leave it **Off**. |
| **Respawn wait** | Seconds a downed player waits before coming back beside his partner (1–60, default 9). |
| **Fall rescue** | *On*: a player who falls far below his partner (out of the map) is put back beside him. Default *Off*. |
| **P2 joins P1 after cutscenes** | *On*: when a cutscene or mission start moves P1 far away, P2 is brought along. Default *Off* (use Teleport instead). |
| **Menus not stretched (4:3)** | *On*: the game's menus and pop-ups are shown in the 4:3 middle of a widescreen picture instead of stretched. |
| **Keyboard drives** | *P1* or *P2*: which player the keyboard and mouse control (same as F8). |
| **Close** | Closes the menu. |

## Playing as Red Hulk

Open the co-op menu (**RB + D-pad Up**), go to **Red Hulk** and press left/right to pick:

- **P1** — player 1 is Red Hulk.
- **P2** — player 2 is Red Hulk (default, so the two Hulks are easy to tell apart).
- **Off** — both players are the classic green Hulk.

It takes effect immediately and is saved. You can also set it in `coop.ini` with `red_hulk_player = 1`, `2` or `0`. Red Hulk is a colour change of the Hulk model (same moves and abilities), and P2's radar arrow is shown in blue.

## Camera distance (side-by-side split)

In side-by-side split each player can move his own camera closer or farther with **D-pad Up / Down** (hold). Both cameras start at `0.7` of the game's distance, which fits the narrower halves better. P2's camera follows P1's setting until P2 changes his own. P1's setting is remembered in `coop.ini` as `zoom_side_by_side`. Top/bottom split always uses the game's normal camera distance.

---

## coop.ini

`coop.ini` is created next to the exe the first time you play. The co-op menu changes most of these settings for you. Edit it with Notepad while the game is closed. Example (the text after `#` is only explanation here):

```ini
p1_pad = 1              # controller for P1 (1-4, in the order Windows connected them)
p2_pad = 2              # controller for P2
split = 1               # 0 = one full screen on P1 (no split screen)
split_vertical = 1      # 1 = side by side, 0 = top and bottom
respawn_seconds = 9     # how long a downed player waits
fall_rescue = 0         # 1 = a player who falls far below his partner is put beside him
keyboard_player = 1     # which player the keyboard drives (F8 swaps it)
god_p1 = 0              # god mode for each player
god_p2 = 0
menus_4_3 = 0           # 1 = menus in the 4:3 middle of a widescreen picture
follow_warps = 0        # 1 = P2 joins P1 after cutscenes / mission starts
red_hulk_player = 2     # 1 = P1, 2 = P2, 0 = neither
zoom_side_by_side = 0.700  # camera distance in side-by-side split (0.4 - 2.5; 1 = the game's)
prop_sort = 1           # 1 = trees and props near either player are kept loaded first
```

---

## Known issues

- **North American (USA/Asia) disc only.** The PAL release is not supported by the co-op build.
- Cutscenes and scripted camera shots are shown full screen from P1's view; split screen comes back right after.
- Missions are the single-player missions: the objectives and timers are the game's, and some count only P1's actions.
- Trees and props: the original game only keeps a limited number of objects loaded around the camera. The mod loads many more and keeps those near *either* player first, but in very dense areas, with the players far apart, distant scenery can still pop in on one half.
- P2 can occasionally look less smooth than P1 at a distance.
- If a level change or quitting to the menu ever crashes, please send the newest log from `logs\`.

## Reporting problems

Open an issue on this repository with:
- what happened and what you were doing (a photo or short video helps a lot),
- the newest `logs\hulk_NNN.log`,
- your `coop.ini`.

---

## Building from source

The full source of this fork is in [`source/`](source/). It is the Hulk Recompiled project with the co-op mod added (`source/src/game_usa/coop.cpp`) and a few runtime changes for split screen in `source/sdk/runtime/` (the SDK is included as plain files, not a submodule). Build it the same way as [Hulk Recompiled](https://github.com/lookingcoding/hulk-ultimate-destruction-recomp-xbox-ver-) (Windows, clang/LLVM, CMake, Ninja), with your USA `default.xbe` in `source/assets/usa/` and `-DHULK_USA_ONLY=ON`. The build recompiles the game from your own `default.xbe`; no game code is stored in this repository.

## Credits and licence

- **Hulk Recompiled** by lookingcoding and contributors — the Windows port this mod is built on.
- **Xbox XBE Recomp SDK** by TGP482.
- Co-op split screen mod by **DanDinosaur**.

The source code is released under the MIT licence (see `source/LICENSE` and `source/sdk/LICENSE`); third-party licences are in `source/sdk/THIRD_PARTY.txt` and `compiler/LLVM-LICENSE.txt` in the release.

*The Incredible Hulk: Ultimate Destruction* is © its respective owners (Marvel, Vivendi Universal Games / Radical Entertainment). This project is not affiliated with or endorsed by them. It contains no game assets or game code; you must own the game.
