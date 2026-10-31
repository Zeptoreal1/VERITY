# Alt Spam - Geode mod for Geometry Dash (Android / mobile)

Automatic alternating-input spam for practising wave zigzags. Inputs are injected inside the game's
physics step (`GJBaseGameLayer::processCommands`) using game time, so timing is frame-accurate.

## Build (no setup needed)
1. Upload this folder's contents to a new GitHub repo and push.
2. Actions -> **Build Geode Mod**. When it finishes, download the **AltSpam-geode** artifact.
3. Put `yourname.alt-spam.geode` into your Geode `mods` folder (Android:
   `Android/media/com.geode.launcher/game/geode/mods/`) or install it from the Geode menu.

Optional: change `"id"` / `"developer"` in `mod.json` (format `developer.modname`).
The workflow auto-syncs `"geode"` in `mod.json` with the latest Geode SDK release.

## Using it
In a level, the HUD (top-left) has: `<  5  >` pattern selector (5 / 7 clicks per burst),
ON/OFF, and `P` (presets: 5 slots with Load / Save / Clear). Live CPS, FPS and timing error show underneath.
Everything else (CPS, intensity, speed, burst frequency, delay, buffer, jitter, ramp, countdown,
auto-stop, start side, hold length, split P1/P2 in dual mode, auto-arm) is in the mod's settings.
Desktop builds also get keybinds (settings -> Keybinds).

## How the timing works
`src/AltEngine.hpp` plans clicks (press/release pairs, alternating Left/Right) on a game-time clock
and fires them in the step they become due (buffer = early-fire tolerance). A release never shares a
step with its press, and nothing is left held when you stop/die/pause. `tests/engine_test.cpp`
verifies this and runs in CI.

## Notes
- Left/Right both drive the jump button (Player 1). Enable *Split inputs* to send Right to Player 2 in dual mode.
- This is an input bot: practice use only; don't submit runs using it.
