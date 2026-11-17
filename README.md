# Hybrid Co-op for Total War: ROME II

Simultaneous turns for ROME II multiplayer co-op campaigns.

- **Both players take their turn at the same time.**
- **End Turn means "I'm ready".** The button shows whether your friend is still playing or already ready.
- **The AI factions move once every human player is ready.**
- **Diplomacy works between the two humans:** offers, payments and treaties. While one player is negotiating, the other player can't open a second negotiation, because the game only supports one at a time.

> Status: **testing**. Unofficial fan mod; not affiliated with Creative Assembly or SEGA.

## For players

You need two things on **both** PCs:

1. **The mod**: subscribe on the Steam Workshop *(link coming)* and enable **hybrid_coop** in the ROME II launcher.
2. **The loader**: download `dinput8.dll` from the [latest release](../../releases/latest) and put it next to
   `Rome2.exe`. To find that folder: Steam → right-click ROME II → Manage → Browse local files.
   You can also run `install_loader.bat` from the release, which finds the game folder for you.

Then start or load a multiplayer co-op campaign as usual.

- **Updates:** the co-op code updates through the Workshop. The loader never needs replacing.
- **Disabling:** disable the mod in the launcher and the loader does nothing. Delete `dinput8.dll` to remove it
  completely.
- **Game updates:** after a ROME II update the mod switches itself off until it is updated, instead of crashing.

Logs, which are useful for bug reports, are written to `%LOCALAPPDATA%\RomeHybridCoop` (`loader.log` and `logs\hybrid_*.log`).

## Why a DLL, and is it safe?

Workshop mods for ROME II can only change game data and Lua scripts. Simultaneous turns need changes to how the
game engine decides whose turn it is, so part of the mod is native code. Everything is in this repository:

| File | What it does |
|---|---|
| `src/loader/loader.c` → `dinput8.dll` | Passes DirectInput through to Windows' real `dinput8.dll`. If the mod is enabled in the launcher, it takes `hybrid_coop.dll` out of the mod's pack and loads it, **but only if its Ed25519 signature is valid** (`src/loader/pubkey.h`). |
| `src/core/` → `hybrid_coop.dll` | The co-op logic: hooks in `empire.retail.dll` (shared turn, ready logic, acting faction for commands, diplomacy rules). It first checks the exact game version and the bytes at every hook site. |
| `lua/hybrid_coop.lua` | The in-game End Turn text. Also part of the mod pack. |

- **Built by GitHub, not by a person.** Release files are built and signed by
  [GitHub Actions](.github/workflows/build.yml) straight from this source. `SHA256SUMS.txt` lets you check your
  download.
- **Only signed DLLs run.** The signing key exists only as a repository secret, so the loader runs no DLL except
  one built here.
- **No network access.** The DLLs don't connect to anything. They only read the mod pack and write logs and a
  small status file.

## Building

On any Linux machine or WSL, with `clang`, `lld`, `llvm` and `python3`. No Windows SDK is needed:

```sh
tools/build.sh                        # -> out/dinput8.dll, out/hybrid_coop.dll
```

The mod pack is built with `tools/build_pack.py`. It needs the game's own `lua_scripts/events.lua`, which is
Creative Assembly's file and so is not included here.

## How it works (short)

- **Acting faction.** Every command a player sends is preceded by a small tag naming the sending faction. While
  the command runs (in lockstep, on both PCs) the game's "current faction" is set to the sender, so the game's
  own turn checks pass.
- **UI swap.** Between simulation ticks the UI sees the local player's faction as current, so the camera,
  diplomacy, the End Turn button and order cursors behave as on your own turn.
- **One shared phase per round.** The first human's turn is ended automatically once it has really started.
  The last human's turn is the shared phase, which ends when all humans are ready.
- **Diplomacy.** There is one negotiation at a time, and the partner's diplomacy screen is closed by the game's
  own close path.

Third-party code: [Monocypher](third_party/monocypher) 4.0.2 (BSD-2-Clause / CC0), used only to check the signature.
