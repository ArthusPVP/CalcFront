# Architecture

CalcFront is split into a **portable game core** (`src/sim`, plain C, no
calculator headers) and a **front end** that draws and reads keys. The same core
runs on the calculator and in a PC test harness.

```
                 ┌────────────── src/main.c ──────────────┐
 keys ─────────▶ │ menus, setup, input, game loop (10 t/s) │
                 └───────┬───────────────────────┬────────┘
                         │ game_tick()           │ r_frame(), hud_*()
                 ┌───────▼───────┐       ┌───────▼────────┐
                 │   src/sim/    │──────▶│    src/ce/     │──▶ screen
                 │  game core    │ hooks │ renderer + HUD │
                 └───────▲───────┘       └────────────────┘
                         │ plat_map_open / plat_map_tiles
                 maps/calc/*.8xv (AppVars)     host/main.c (PC harness)
```

The core never draws. It tells the front end what changed through four hooks:
`ui_dirty(tile)` (a tile changed owner), `ui_dirty_unit(tile)` (a building
appeared, changed or vanished), `ui_event(type, a, b)` (messages for the
player) and `plat_map_*` (load map files).

## Memory layout on the calculator

| What | Where | Size |
|---|---|---|
| Tile map, 1 byte per tile | unused back half of video RAM (`0xD52C00`) | ≤ 64,000 B |
| Building bitmap, 1 bit per tile | right after the map (`0xD62C00`) | 8 KB |
| Railway bitmap + route-search scratch (`sim_mem_t`) | temporary `CFTMP` AppVar in user RAM | 20 KB |
| Everything else (players, attacks, units, caches) | static RAM (BSS) | ~40 KB |
| Program code | user RAM, decompressed at launch | ~98 KB |

**Tile byte:** bits 5–6 terrain (0 water, 1 plains, 2 highland, 3 mountain).
Land: bits 0–4 owner (0 = nobody), bit 7 fallout. Water: bits 0–2 water body
(0 = lake or map frame, never sailed), bits 3–4 and 7 are scratch for the route
search. The map edge is always water, so the 4 neighbours of a land tile always
exist. Tiles are indexed `y * width + x` (`tile_t`, 16-bit; `0xFFFF` = none).

## `src/sim/`: the game core

| File | Role |
|---|---|
| `sim.h` | All shared types and declarations: tiles, players, attacks, units, events, the scratch block, and shift-free inline helpers (`tx`, `ty`, `bitget`, `bitset`). |
| `tile_tab.h`, `fx_tab.h` | Generated lookup tables (bitmap indices, log2 and exp2 mantissas). |
| `fx.c` | Fixed-point math (`fx_log2`, `fx_exp2`, `fx_pow` for OpenFront's `^0.6` and `^0.73`), random numbers (an 8-bit Jenkins generator), distances. Written with byte operations, because multi-bit shifts are slow loops on the eZ80. |
| `map.c` | Map file loading (header, size variants, nation list), ownership changes (`conquer`, `relinquish`), coastline queries, player bounding boxes and neighbours, straight lines, and ship routes: greedy best-first search over one water body with a shared cache of routes. |
| `game.c` | Game setup and spawning, the per-tick economy (troop cap, troop growth, gold, multipliers), player death, the win check, and `game_tick()`, which runs every system in order. |
| `attack.c` | Land attacks: finds the front between two players, keeps a ring of 128 front tiles, and takes tiles with OpenFront's cost and loss formulas (terrain, defense posts, fallout, big-empire and traitor modifiers), cached per terrain type each tick. |
| `units.c` | The unit table (200 slots) shared by every unit type, building rules and prices, upgrades, capture on conquest, per-player counts cached each tick, nukes and SAM interception, and `units_tick()`, which dispatches each unit to its update function. |
| `ships.c` | Transport boats (land and start an attack), ports and trade ships (OpenFront's spawn odds and weighted destinations, gold by distance), warships (targets, shells, health, repair at port, veterancy, capturing trade ships, player orders). |
| `rail.c` | Factories turn nearby cities, ports and factories into stations, links them with straight railways over land, sends trains to random reachable stops, and pays gold at each stop. Only changed track is repainted. |
| `diplo.c` | Relations, alliance requests and expiry, betrayal and traitor status. |
| `ai.c` | Tribes (expand, retaliate, pick random neighbours) and nations (build economy and navy, nuke leaders, boat to weak overseas rivals, ask for help, betray on Hard and Impossible). Each AI thinks on its own staggered schedule. |

**One tick** (`game_tick`): economy for every player → attacks → units (boats,
trade, warships, shells, trains, nukes, construction) → railways rebuilt if
needed → AI → diplomacy every 16 ticks → win check every 32 ticks.

## `src/ce/` and `src/main.c`: the calculator front end

| File | Role |
|---|---|
| `main.c` | Key scanning with real-time auto-repeat, the title, help, setup, leaderboard and pause screens, the five action keys and their menus, player actions (attack, boat, build, nuke, diplomacy, warship orders), event messages, map discovery (`ti_Detect` on the `CFM2` signature), the scratch AppVar, and the game loop: run the ticks that are due, then draw. |
| `ce/render.c` | Draws the map straight into video memory. A full redraw happens only when the camera moves; otherwise each frame repaints only tiles that changed, the area under sprites that moved, and sprites or buildings that a 16×16 dirty-cell grid says were painted over. Sprites are pre-built masks. Also the cursor, nuke rings, explosion rings and the setup preview. |
| `ce/hud.c` | Number formatting (troops shown /10 like OpenFront), the troop bar, the info line and the action-key bar. |
| `ce/ui.h` | Palette indices, screen layout, renderer and HUD declarations. |

## Other folders

| Path | Role |
|---|---|
| `host/` | PC harness: runs the core with AIs on every side, checks the whole game state every 128 ticks under AddressSanitizer and UBSan, and can dump frames as images. |
| `tools/mapconv.py` | Converts OpenFront maps into CalcFront files: downscales, classifies terrain, labels water bodies, snaps nation positions, builds Normal and Large variants, compresses with ZX0, writes `maps/calc`, `maps/pc` and `maps/preview`. |
| `tools/fetch_maps.sh` | Downloads the OpenFront map sources into `vendor/`. |
| `tools/cemu_run.py`, `tools/cemu-headless-launch.patch` | Plays key scripts in a headless CEmu and saves screenshots. |
| `maps/` | Map AppVars, previews and their CC BY-SA licence. |
| `packaging/` | Text shipped in the release zip and the release notes. |
| `.github/workflows/build.yml` | Builds, packages and publishes `CalcFront.zip`. |

**Map file (`CFM2`):** name, compression flag, then for each size variant its
width, height, land tile count, scale (original tiles per CalcFront tile ×16)
and data offset, then up to 40 nations (position as a fraction of the map,
name), then the tile data, ZX0-compressed on the calculator.

## Profiling

Build with `make CFLAGS="-Wall -Wextra -Oz -DCF_PERF"`. Every 100 ticks the
bottom of the screen shows the time spent in each part: economy, attacks, units,
AI, buildings, moving sprites, erasing, tiles, cursor, full redraws and HUD.

