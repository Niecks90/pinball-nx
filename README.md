# Pinball NX

Pinball NX is a Nintendo Switch/Horizon OS port of [Pinball NS](https://github.com/ReiniShari/Pinball-NS). It keeps the upstream C11 Game Boy Color emulator and unified-table runtime, then adds a libnx frontend with native 16:9 presentation, optional portrait/TATE rotation, controller support, and simultaneous touch zones.

## Features

- 16:9 landscape presentation for handheld and docked output.
- Landscape mode plus persisted Portrait/TATE rotation at 90° and 270°.
- Multi-touch flippers, plunger, and table-nudge zones.
- Joy-Con and Pro Controller input through modern `PadState`/`padUpdate`.
- SD-card configuration and a readable missing-ROM screen.
- Automated devkitA64 builds on every push, pull request, and version tag.

The original Pokémon Pinball ROM is **not included**. Use a dump of your own supported US cartridge.

## Expected SD Card File Structure

```text
sdmc:/
└── switch/
    └── pinball-nx/
        ├── pinball-nx.nro              # downloaded from the GitHub release
        ├── pokemon_pinball.gbc         # user-supplied compatible US ROM
        └── config.ini                  # generated and updated automatically
```

`pokemon_pinball.gbc` is the only required user-supplied data file. Pinball NX does not distribute ROMs. `config.ini` stores the orientation and touch-guide preference and is created when settings are changed. The current Switch frontend reads the ROM and configuration from the exact paths shown above; no additional `romfs:/` assets or external folders are required.

## Controls

| Action | Controller | Landscape / portrait touch |
|---|---|---|
| Left flipper | L, ZL, D-pad Left | Bottom-left zone |
| Right flipper | R, ZR, A | Bottom-right zone |
| Plunger | D-pad Down | Center bottom / plunger lane |
| Nudge left/right/up | X/Y and directional input | Upper-left / upper-right / upper-center |
| Pause/settings | + | Pause corner |
| Cycle orientation | - | Pause/settings menu |
| Toggle touch guide | X | Settings |

Touch is multi-touch: flippers and nudge/plunger gestures can be held simultaneously. Touch coordinates are transformed for both portrait directions.

## Installation

1. Download `pinball-nx.nro` from the [v0.1.0 release](https://github.com/Niecks90/pinball-nx/releases/tag/v0.1.0).
2. Copy it to `sdmc:/switch/pinball-nx/`.
3. Dump your compatible US Pokémon Pinball ROM and copy it to `sdmc:/switch/pinball-nx/pokemon_pinball.gbc`.
4. Launch Pinball NX from the Homebrew Menu.

## Building from source

Install devkitPro with devkitA64 and the libnx tools, then run:

```sh
make
```

The same build runs in the official Docker image:

```sh
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest make
```

GitHub Actions uses that container for push, pull-request, and `v*` tag builds. Release builds attach `pinball-nx.nro` automatically.

## Credits and licensing

The emulator/game runtime and original design are by Reinhard Sharapov in [ReiniShari/Pinball-NS](https://github.com/ReiniShari/Pinball-NS); upstream source attribution is retained in this repository. Pokémon Pinball is copyrighted by Nintendo, Creatures Inc., GAME FREAK inc., HAL Laboratory, and Jupiter Corp. This project does not distribute ROMs.
