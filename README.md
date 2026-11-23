# Pinball NX

Pinball NX is a Nintendo Switch/Horizon OS port of [Pinball NS](https://github.com/ReiniShari/Pinball-NS). It keeps the upstream C11 Game Boy Color emulator and unified-table runtime, adds a modern libnx frontend, native 16:9 presentation, optional portrait/TATE rotation, controller support, and simultaneous touch zones.

The original Pokémon Pinball ROM is **not included**. Dump your own supported US ROM and copy it to `sdmc:/switch/pinball-nx/pokemon_pinball.gbc`.

## Controls

| Action | Controller | Landscape / portrait touch |
|---|---|---|
| Left flipper | L, ZL, D-pad Left | Bottom-left zone |
| Right flipper | R, ZR, A | Bottom-right zone |
| Plunger | D-pad Down | Center bottom / plunger lane |
| Nudge left/right/up | X/Y, D-pad directions | Upper-left / upper-right / upper-center |
| Pause/settings | + | Touch the pause corner |
| Cycle orientation | - | Pause menu |
| Toggle touch guide | X | Settings |

Touch is multi-touch: flippers and nudge/plunger gestures can be held simultaneously. Orientation is persisted in `sdmc:/switch/pinball-nx/config.ini`.

## Installation

Copy `pinball-nx.nro` to `sdmc:/switch/pinball-nx/` and launch it from the Homebrew Menu. Place the ROM at the path above. Saves/configuration are stored beside it on SD.

## Building

Install devkitPro/devkitA64 and run:

```sh
make
```

The GitHub Actions workflow builds inside `devkitpro/devkita64:latest` and uploads `pinball-nx.nro` as an artifact.

## Credits and licensing

The emulator/game runtime and original design are by Reinhard Sharapov in `ReiniShari/Pinball-NS`; upstream attribution and source files are retained. Pokémon Pinball is copyrighted by Nintendo, Creatures Inc., GAME FREAK inc., HAL Laboratory, and Jupiter Corp. This project does not distribute ROMs.
