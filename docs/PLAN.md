> This is the original design plan, written before development started. Some
> details changed along the way; the [README](../README.md) describes the game
> as it is.

# CalcFront — plan

A single-player remake of [OpenFront](https://github.com/openfrontio/openfrontio) for the
TI-84 Plus CE, written in C with the CE toolchain and the CE C libraries (`graphx`,
`keypadc`, `fileioc`), tested in CEmu. Instead of online opponents you play against
**nations** (smart AI) and **tribes** (the passive grey bots of the original).

## 1. What the calculator allows

| Constraint | Consequence |
|---|---|
| eZ80 @ 48 MHz, no FPU | Integer / fixed-point only. `pow(x, 0.6)` and `pow(x, 0.73)` from the original formulas become lookup tables. |
| ~60 KB of heap/BSS | Not enough for a tile map. The map lives in the unused back half of VRAM (76,800 bytes) — the game draws single-buffered with incremental redraws. |
| 320×240 screen, 8-bit palette | 1 palette entry per player + terrain shades + UI colours. |
| Program ≤ 64 KB | Code is zx0-compressed by the toolchain; maps ship as separate appvars. |
| Appvar ≤ 64 KB | Maps are stored 2 bits/tile and compressed (≈3–8 KB each). |

**Map size:** at most 256×256 tiles, so every coordinate is one byte and a tile index is
16 bits (fast on eZ80). The real World map is 2064×1032; ours will be about 256×128.

**One byte per tile in RAM:** `owner (5 bits, 0 = unowned, up to 31 players)` +
`terrain (2 bits: water / plains / highland / mountain)` + `fallout (1 bit)`.
Shoreline and borders are derived from neighbours.

## 2. Gameplay scope (taken from the OpenFront engine source)

Kept, with the original formulas rescaled to the smaller tile count:

- **Spawn phase** — pick your start tile with the cursor; AI players spawn too.
- **Troops** — cap `2·(tiles^0.6·1000 + 50000) + 250k per city level`, growth
  `(10 + troops^0.73/4)·(1 − troops/cap)`; tribes get ⅓ cap and ½ growth.
- **Gold** — income per tick, spent on structures and weapons.
- **Attacks** — send a % of troops (adjustable ratio) at a neighbour or at unowned land;
  the front advances tile by tile, slower and costlier on highland/mountain; retreat.
- **Structures** — City, Defense Post, Port, Missile Silo, SAM Launcher (with build
  time, min spacing, upgrades).
- **Naval** — transport boats for overseas attacks, trade ships between ports (gold),
  warships.
- **Nukes** — Atom Bomb, Hydrogen Bomb, MIRV, SAM interception, fallout.
- **Diplomacy** — alliance request / break (traitor penalty), target, embargo.
- **Win** — own 80 % of the land; lose when you own nothing.
- **Difficulties** — Easy / Medium / Hard / Impossible, using the original nation
  multipliers (troop cap ×0.5 / 0.75 / 1 / 1.25, growth ×0.9 / 0.95 / 1 / 1.05) plus
  AI reaction speed and aggression.

Cut or deferred: factories and trains (stretch goal), teams, emojis/chat, donations,
multiplayer.

## 3. Opponents

- **Tribes (grey bots)** — as in the original: random spawn, expand into free land,
  occasionally attack a neighbour, never build. All drawn in grey shades.
- **Nations (smart AI)** — spawn at the real positions and with the real names from each
  map's manifest (e.g. the World map has 70; we keep the ones that fit). Behaviour ported
  from `NationExecution` and friends: expand, pick weak/hostile targets, keep a troop
  reserve, build cities/ports/defences/silos, launch boats, nuke the leader, form and
  betray alliances. Each nation thinks on a staggered schedule so no tick is overloaded.
- Game setup lets you choose map, difficulty, number of nations and number of tribes
  (total capped at 31 players).

## 4. Controls

| Keys | Action |
|---|---|
| Arrows | Move the cursor (camera follows at the screen edge) |
| `8` `4` `6` `2` | Scroll the map up / left / right / down |
| `+` / `−` | Zoom in / out (1, 2, 4, 8 px per tile) |
| `Y=` `Window` `Zoom` `Trace` `Graph` | Five soft keys; their labels are drawn on the bottom bar above them and change with context |
| `Clear` | Back / pause menu |

Soft-key layout on the map (target = tile under the cursor):

1. **Attack** — attack the owner of the tile; becomes **Boat** if only reachable by sea
2. **Build** — opens City / Defense / Port / Silo / SAM (+ Warship on water)
3. **Nuke** — Atom / Hydrogen / MIRV at the cursor
4. **Diplo** — Ally / Break / Target / Embargo for the tile's owner
5. **More** — attack-ratio −/+, leaderboard, player info, game speed

## 5. Screen layout

- Top bar: troops / cap, gold, % of map owned, attack ratio.
- Map view: terrain + player colours, darker borders, structure icons, ships, nukes.
- Bottom bar: the five soft-key labels. Event messages appear just above it.
- Menus: title, new game (map, difficulty, bot counts), pause, win/lose, how-to-play.

## 6. Architecture

```
src/
  main.c        game loop, state machine
  sim/          portable game core — no calculator headers
    map.c  player.c  attack.c  units.c  nuke.c  diplo.c  rng.c  fixed.c
    ai_tribe.c  ai_nation.c
  ce/           calculator layer: render.c  input.c  hud.c  menu.c  mapfile.c
  host/         PC harness: runs the same sim natively, dumps PNG frames
tools/
  mapconv.py    OpenFront map.bin + manifest.json -> 2-bit appvar + nation list
maps/           generated .8xv appvars
```

The simulation core is plain C with no calculator dependency, so it also compiles on
Linux. That lets me run thousands of full bot-vs-bot games in seconds to catch crashes,
overflows and balance problems before anything touches the emulator.

Performance approach: fixed tick (10/s like the original, with a speed setting), a
per-tick work budget for attack fronts, per-attack border lists instead of the original
priority queue, and a dirty-tile list so only changed tiles are redrawn.

## 7. Maps

`tools/mapconv.py` downsamples the official OpenFront terrain to ≤ 256×256 and exports
one appvar per map. First set: **World, Europe, Africa, Asia, North America,
South America, Oceania, MENA, Britannia, Japan, Black Sea, Pangaea, Mars**. Adding more
later is one command each. The game lists whichever map appvars are on the calculator.

## 8. Milestones

Each one ends with a build that runs and is checked in the emulator.

1. **Setup** — install the CE toolchain and CEmu, hello-world program, automated
   screenshot test, git repo.
2. **Map** — converter, map appvars, renderer, cursor, scrolling, zoom.
3. **Core sim** — spawn, troop/gold growth, attacks on free land and on players,
   win/lose, HUD, PC harness.
4. **AI** — tribes, then nations, then the four difficulties.
5. **Structures** — build menu, city, defense post, port, upgrades.
6. **Naval** — transport boats, trade ships, warships.
7. **Nukes** — silo, SAM, atom, hydrogen, MIRV, fallout.
8. **Diplomacy** — alliances, betrayal, embargo, AI relations.
9. **Polish** — menus, settings, save/resume, speed and balance tuning, README with
   install instructions, release files (`CALCFRNT.8xp` + map appvars).

## 9. Testing

- PC harness: mass bot-vs-bot soak runs, determinism check, PNG dumps of game states.
- CEmu `autotester`: scripted key presses + screen checks for menus, spawn, attack,
  build, nuke, win screen.
- Manual playthroughs in CEmu on at least two maps per difficulty.
- Frame-time measurements late game on the World map with the maximum player count.

## 10. Things needed from you

1. **A TI-84 Plus CE ROM image.** CEmu cannot run without one, it is copyrighted by TI
   and must be dumped from your own calculator (CEmu has a built-in ROM dump wizard).
   Nothing on this machine is a calculator ROM. Without it I can still build everything
   and test the game core on PC, but not run it in the emulator.
2. Permission to download and install the CE toolchain (to `~/CEdev`) and CEmu.
3. Confirmation that the target is the colour **TI-84 Plus CE** (the C toolchain does not
   support the older monochrome TI-84 Plus).

## 11. Licence

OpenFront's code is AGPL-3.0 and its assets (maps) are CC BY-SA 4.0. CalcFront reuses the
map data and ports the game rules, so it will be released under AGPL-3.0 with map
attribution to OpenFront in the README and the in-game credits.
