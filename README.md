# Pinball NX

Pinball NX is a Nintendo Switch port of PKMN Pinball based on [Pinball NS](https://github.com/ReiniShari/Pinball-NS). It keeps the upstream C11 Game Boy Color emulator and unified-table runtime, then adds a libnx frontend with native 16:9 presentation, optional portrait/TATE rotation, controller support, and simultaneous touch zones.

## Features

- 16:9 landscape presentation with a zoomed auto-scrolling camera that smoothly tracks the ball (or toggleable full-table overview).
- Full-table Portrait/TATE rotation at 90° CW and 270° CCW with matching rotated touch guide labels.
- Support for both **Pokémon Pinball (Original / Gen 1: Red & Blue)** and **Pokémon Pinball Generations 1.6 (Red, Blue, Gold, Silver, Ruby & Sapphire)**, including automatic IPS patching and in-menu switching with separate cartridge saves.
- Multi-touch flippers, plunger, table-nudge, and pause zones.
- Joy-Con and Pro Controller input through modern `PadState`/`padUpdate`, plus 48 kHz stereo `audout` audio.
- SD-card configuration (`config.ini`), startup diagnostics (`boot.log`), and a readable missing-ROM screen.
- Automated devkitA64 builds on every push, pull request, and version tag.

ROMs and patches are **not included**. Use a dump of your own supported US cartridge (and optionally the Pokémon Pinball Generations 1.6 ROM or `.ips` patch).

## Expected SD Card File Structure

```text
sdmc:/
└── switch/
    └── pinball-nx/
        ├── pinball-nx.nro                  # downloaded from the GitHub release
        ├── pokemon_pinball.gbc             # user-supplied US ROM (Gen 1: Red & Blue)
        ├── pokemon_pinball_generations.gbc # optional Generations 1.6 ROM (6 tables)
        ├── pokemon_pinball_generations.ips # optional Generations 1.6 IPS patch (or generations.ips)
        ├── pinball.sav                     # Original cartridge save (auto-managed)
        ├── gen_pinball.sav                 # Generations cartridge save (auto-managed)
        ├── config.ini                      # created automatically on first boot
        └── boot.log                        # startup diagnostic log
```

At least one supported ROM is required (`pokemon_pinball.gbc` for Original US, or `pokemon_pinball_generations.gbc`; if you provide `pokemon_pinball.gbc` along with `pokemon_pinball_generations.ips` or `generations.ips`, Pinball NX automatically applies the patch on startup and caches `pokemon_pinball_generations.gbc`). Pinball NX does not distribute ROMs. `config.ini` persists `orientation`, `touch_guide`, `landscape_camera` (`1` = follow ball, `0` = full table), and `generation` (`0` = Original, `1` = Generations 1.6). `boot.log` records step-by-step startup diagnostics.

## Controls

| Action | Controller | Landscape / portrait touch |
|---|---|---|
| Left flipper | L, ZL, D-pad Left | Bottom-left zone |
| Right flipper | R, ZR, A | Bottom-right zone |
| Plunger | D-pad Down | Center bottom / plunger lane |
| Nudge left/right/up | X / Y / D-pad Up | Upper-left / upper-right / upper-center |
| Pause/settings | + | Top-right Pause corner |
| Cycle orientation | - (or Pause menu) | — |
| Toggle touch guide | X (in Pause menu) | — |
| Toggle landscape camera | L / ZL (in Pause menu) | — |
| Switch Generation (Original / Gen 1.6) | Y (in Pause menu) | — |

Touch is multi-touch: flippers and nudge/plunger gestures can be held simultaneously. Touch coordinates and overlay labels rotate automatically for both portrait orientations (`90°` CW and `270°` CCW).

## Installation

1. Download `pinball-nx.nro` from the [v0.2.0 release](https://github.com/Niecks90/pinball-nx/releases/tag/v0.2.0).
2. Copy it to `sdmc:/switch/pinball-nx/`.
3. Dump your compatible US Pokémon Pinball ROM and copy it to `sdmc:/switch/pinball-nx/pokemon_pinball.gbc` (and optionally add `pokemon_pinball_generations.gbc` or `pokemon_pinball_generations.ips` / `generations.ips` for Generations 1.6).
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
