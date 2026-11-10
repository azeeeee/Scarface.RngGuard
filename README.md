# Scarface RngGuard

An ASI plugin for **Scarface: The World Is Yours 1.0.0.2** that prevents a crash
on mission loading screens — typically when the player dies and the game
reloads the mission/level. It was reported on the **Gaspar Gomez** mission.

The game's ambient population spawner can be left with an empty or corrupted
list of spawn candidates while a level is being reloaded. Picking from that list
returns no candidate and the caller dereferences it, which kills the game on the
loading screen.

The plugin validates that list and skips the spawn instead. It does **not**
modify `scarface.exe`; everything happens at runtime.

## Install

1. Copy `dist/Scarface.RngGuard.asi` next to `scarface.exe` (or into `scripts\`).
2. Start the game.

An ASI loader is required. SilentPatch and Scarface Fusion Fix already ship one.

## Settings

`Scarface.RngGuard.ini` is created on first launch:

```ini
[General]
Logging=1     ; 0 = do not write a log file
```

Advanced options (log size cap, individual log categories, guard toggle) are in
the generated file under `[Advanced]` and normally do not need changing.

## Build

```
build.bat
```

Requires the 32-bit MSVC toolset. The plugin is written in C++ and builds to
`dist/Scarface.RngGuard.asi`.

See [docs/technical.md](docs/technical.md) for the crash analysis and the
runtime patches.
