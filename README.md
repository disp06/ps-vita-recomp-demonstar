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
3. Launch it and pick a game. The launcher also sets screen mode (Original 1:1 pixels like the PC window, 4:3, or stretched)
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

## Two Vitas (ad-hoc or same Wi-Fi)

Player 1 plays on one Vita, player 2 on another. Both Vitas need this VPK
(v1.04 or later) and **the same game files** in `ux0:data/DemonStar` (the ds.exe files
are compared; settings come from player 1's Vita).

1. In the launcher set **Play** to *Two Vitas, ad-hoc: HOST* on player 1's Vita
   and *Two Vitas, ad-hoc: JOIN* on player 2's.
2. Player 1 picks DemonStar or Secret Missions; player 2 picks *JOIN player 1's game*.
3. Each Vita opens the system ad-hoc dialog (group `DEMONSTR`) and connects.
4. When the lobby says the game is starting, both Vitas run it together.
   DemonStar: player 2 presses START to join in. Secret Missions 1: player 1
   chooses Start Game -> 2 Player Game.

*Same Wi-Fi* works the same way, through a router, on UDP port 7777 (both Vitas
on one network in Settings > Network). Use it if ad-hoc doesn't connect.

How it works: delay-based lockstep. Both Vitas run the whole game. Only the
buttons are sent, 3 frames (50 ms) ahead, several times per packet in case some
are lost. The game's clock, the date, the registry and the sound buffer
positions are made to depend only on the game's own progress, so both Vitas
stay in step frame by frame. A state checksum is compared twice a second; a
mismatch shows "Out of sync" and is logged. If the link stalls,
"Waiting for the other Vita..." is shown. After 20 s without a link, or when
either player quits (L+R+START+SELECT), the session ends on both Vitas.

- Rollback is not used: it would need the game state saved and replayed every
  frame, which a recompiled 1998 game can't do cheaply. On ad-hoc the 50 ms
  input delay is hardly noticeable.
- Two-Vita games keep their own settings and high scores
  (`ux0:data/DemonStar/save/<game>/net`), so the normal ones are untouched.
- The Bluetooth player 2 mode was removed in v1.04: it called the 8BitVita-Pro3
  plugin, and without that plugin the app crashed at the launcher (C2-12828-1).

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

- The games' own network modes (DirectPlay / IPX / modem) are not available;
  two-Vita play is done by the port instead (see above).
- Two-Vita play was tested with two instances of the Linux build (lockstep,
  packet loss, 8-minute run with checksums every frame), not yet on two real Vitas.
- Music is played through a General MIDI soundfont, so it sounds like a GM synth,
  not exactly like any specific 1998 sound card.

## Third-party components

See `vita/LICENSES.txt`: TinySoundFont (MIT), GeneralUser GS soundfont (downloaded
at build time, free license), launcher font generated from DejaVu Sans Mono.

DemonStar is © Mountain King Studios. This project is not affiliated with them.
