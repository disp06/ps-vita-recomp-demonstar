# DemonStar + DemonStar Secret Missions 1 — PS Vita static recompilation

A native PS Vita port of the Windows versions of **DemonStar** (1998) and
**DemonStar: Secret Missions 1** (2002) by Mountain King Studios.

The original `ds.exe` files are statically recompiled from x86 to C, and a small
Win32 runtime (kernel32 / user32 / gdi32 / DirectDraw / DirectSound / DirectInput /
winmm MIDI / registry) is re-implemented on top of the Vita SDK. Both games ship in
one VPK with a launcher.

**No game code or data is in this repository.** You need your own copy of the games:
the recompiler reads your `ds.exe` files, and the games load their `.glb` data from
your copy at runtime.

## Supported game files

| Game | File | MD5 |
|---|---|---|
| DemonStar (1998, Win95) | `PROGRAM/ds.exe` | `a84f0408db9e46204226688f71e10b70` |
| Secret Missions 1 (2002) | `ds.exe` | `1fc3b043f0c881062ced47a64bf92737` |

Other versions will build but the generated code will not match the runtime checks.

## Building

Requirements: Python 3 with `pefile` and `capstone`, [VitaSDK](https://vitasdk.org),
git. (For the Linux test build: gcc.)

```sh
pip install pefile capstone
# put your game files in place
cp /path/to/DemonStar/PROGRAM/ds.exe /path/to/DemonStar/PROGRAM/*.glb games/ds1/
cp /path/to/DemonStarSM1/ds.exe /path/to/DemonStarSM1/*.glb games/sm1/
tools/fetch_soundfont.sh          # GeneralUser GS soundfont for MIDI music
./build.sh vita                   # -> vbuild/DemonStar.vpk
./build.sh linux                  # optional headless test build -> build/demonstar
```

## Installing on the Vita

1. Install `DemonStar.vpk` with VitaShell.
2. Copy your game files to the Vita:
   - `ux0:data/DemonStar/ds1/` — `ds.exe`, `Game.glb`, `Game2.glb`
   - `ux0:data/DemonStar/sm1/` — `ds.exe`, `Game.glb`, `game1.glb`, `GAME2.glb`, `Game3.glb`, `demon1.ifr`, `player1.bmp`, `player2.bmp`
3. Launch it and pick a game. The launcher also sets screen mode (4:3 or stretched)
   and DemonStar's difficulty, because DemonStar has no in-game options menu.

Saves, high scores and settings go to `ux0:data/DemonStar/save/`; log at
`ux0:data/DemonStar/log.txt`.

## Controls

| Vita | Game |
|---|---|
| D-pad / left stick | Move |
| Cross / Square | Fire |
| Circle | Mega bomb (also selects in SM1 menus) |
| START | Start player 1 (F2) / unpause |
| SELECT | Esc (SM1 menu) |
| Triangle | Enter (pause in game, select in menus) |
| L + Up/Down, Cross | Type a letter; R = Enter |
| Hold L + R + START + SELECT | Save and quit |

High-score names are entered with the d-pad and fire, like the original.

## How it works

- `tools/pe.py` — loads the PE and finds all functions (recursive descent from the
  entry point, code pointers in data, gap filling, MSVC switch tables).
- `tools/recomp.py` — turns every function into C. Registers are C locals, flags are
  computed lazily only where something reads them (compare+branch pairs become plain
  C comparisons), x87 is emulated with doubles, imports become direct calls into
  the runtime, indirect calls go through a dispatch table. Busy-wait loops get a
  poll inserted so Windows timer callbacks still run on a single thread.
- `rt/` — the Win32/DirectX runtime, a software mixer, a General MIDI synth
  (TinySoundFont + GeneralUser GS), and the host layers (`host_vita.c`,
  `host_linux.c` for headless testing with scripted input and frame dumps).

## Limitations

- Multiplayer (DirectPlay / IPX / modem) is not available.
- Music is played through a General MIDI soundfont, so it sounds like a GM synth,
  not exactly like any specific 1998 sound card.

## Third-party components

See `vita/LICENSES.txt`: TinySoundFont (MIT), GeneralUser GS soundfont (downloaded
at build time, free license), launcher font generated from DejaVu Sans Mono.

DemonStar is © Mountain King Studios. This project is not affiliated with them.
