**CalcFront** is a territory conquest strategy game for the TI-84 Plus CE, inspired by [OpenFront](https://openfront.io). Spawn anywhere on a real-world map, grow your army, push your borders, build ports, factories and railways, fight at sea, make and break alliances, and launch nukes, against up to 30 computer players.

- 15 maps converted from OpenFront (World, Europe, Africa, Asia, the Americas, Oceania, MENA, Mars...), seven of them also in a Large size
- OpenFront's rules: troop growth and caps, terrain, defense posts, boats, ports and trade ships, factories, railways and trains, warships, silos, SAMs, atom and hydrogen bombs, MIRVs, alliances and betrayal
- smart nations with four difficulties (Easy to Impossible) and passive tribes
- save and resume, a clean screen, and a hidden pro mode that shows every number
- hand-optimised eZ80 code: the full 10 ticks per second, even on Large maps with 30 players

**CalcFront.zip** contains the game (`CALCFRNT.8xp`) and all the maps (`maps/*.8xv`) for the TI-84 Plus CE.

### You also need the CE C libraries

CalcFront uses the standard CE C libraries (graphx, keypadc, fileioc...). Download **`clibs.8xg`** from
**https://github.com/CE-Programming/libraries/releases/latest** and send it to your calculator, together with the game and the maps.

### Install

1. Send `clibs.8xg`, `CALCFRNT.8xp` and the `CF*.8xv` map files with TI Connect CE or TiLP (sending to the Archive is fine).
2. Run `CALCFRNT` from the `prgm` menu (OS 5.3–5.4), or `Asm(prgmCALCFRNT)` (OS 5.0–5.2). On OS 5.5 and newer, TI disabled assembly/C programs: see [arTIfiCE](https://yvantt.github.io/arTIfiCE/).
3. Keep about 125 KB of RAM free while playing.

Full instructions, controls and licences: see the [README](https://github.com/ArthusPVP/CalcFront#readme).
