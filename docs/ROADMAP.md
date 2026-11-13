# Roadmap

Ideas for future work, roughly by priority. Contributions on any of them are welcome.

## Next

- [ ] **Fix speed and lag spikes on real hardware.** Everything so far was optimized for CEmu,
  some modifications needs to be made to make it deacent.
- [x] **Save and resume.** Save a game to an AppVar from the pause menu and reload it
  from the title screen.
- [ ] **Better balance for small maps.** Tune troop scale, trade and train income for
  the calculator's smaller tile counts, using the PC harness to run many games.
- [ ] **AI nukes and SAMs.** Nations rarely save enough gold for silos; make the
  economy-to-weapons path work on Medium and above.
- [x] **Faster scrolling** Scroll the screen by copying video memory instead of redrawing it all.
- [ ] **Clearer feedback.** *More > Info* now lists incoming attacks and alliance
  timers; still missing: the gold earned by trade and trains, and an alert on
  the map when a boat is heading for your coast.
- [x] **Tiny but impactful improvements** Fix attack boats not finding a way to a location
  and lagging too much the game, find a more precice and **more efficient** way to calculate map percentage. See [Issue 6](https://github.com/ArthusPVP/CalcFront/issues/6)
- [x] **Improve HUD** move non-important statistics (troops/sec, % of the land) on a sub-menu. See [Issue 4](https://github.com/ArthusPVP/CalcFront/issues/4)
- [ ] **Several save slots**, and an autosave when the calculator turns off.

## Gameplay

- Spawn-phase timer with the AI choosing spots at the same time as you.
- A tutorial map.

## Maps and content

- Convert more OpenFront maps (Britannia classic, Baltics, Hawaii, Luna, ...).
  It only takes a line in `tools/mapconv.py`.
- Large variants for more maps, and a "huge" size if memory allows.
- Map selection with previews and nation counts in the setup screen.

## Technical

- [ ] Make code smaller, to lower the RAM needed (about 130 KB today).
- [x] Fix rendering and make a clearer tutorial screen
- [ ] Faster AI decisions on big games (per-tick AI budget).
- [ ] Run the PC harness in GitHub Actions on every push, with generated maps
  cached, and check that games stay deterministic.
- [ ] Automated emulator tests in CI (needs a ROM, so probably only for forks that
  provide one as a secret).

