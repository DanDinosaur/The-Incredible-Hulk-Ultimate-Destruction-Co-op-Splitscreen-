# Co-op fork notes

This folder is Hulk Recompiled (https://github.com/lookingcoding/hulk-ultimate-destruction-recomp-xbox-ver-, commit bcb9817)
with the split-screen co-op mod added:

- `src/game_usa/coop.cpp` — the whole co-op mod (second player, split screen, respawns, co-op menu, Red Hulk, radar,
  camera distance, prop streaming for two cameras). It is compiled into the executable; the recompiled game code
  (the DLL) is built from the player's own disc.
- `src/game/hooks.cpp`, `src/common/game_info.cpp`, `CMakeLists.txt` (`HULK_USA_ONLY`) — small hooks the mod needs.
- `sdk/` — Xbox XBE Recomp SDK (https://github.com/TGP482/XboxXBE-Recomp, commit 0fd0044) as plain files, with
  split-screen support in `runtime/xdk/d3d8.cpp` and `runtime/gpu/*` (per-half viewports, Red Hulk / blue radar tint
  in `shaders.cpp`, a Direct3D 11 path in `d3d11.cpp`).

Build: put the USA/Asia `default.xbe` in `assets/usa/`, configure with CMake (Ninja, clang) and `-DHULK_USA_ONLY=ON`,
build the `HulkRecompiledUSA` and `HulkRecompiledUSA_code` targets. No game data or recompiled game code is stored here.
