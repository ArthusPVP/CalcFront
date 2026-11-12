# Roadmap

Ideas for future work, roughly by priority. Contributions on any of them are welcome.

## Next

- [ ] **Fix speed and lag spikes on real hardware.** Everything so far was optimized for CEmu,
  some modifications needs to be made to make it deacent.
- [ ] **Save and resume.** Save a game to an AppVar from the pause menu and reload it
  from the title screen.
- [ ] **Better balance for small maps.** Tune troop scale, trade and train income for
  the calculator's smaller tile counts, using the PC harness to run many games.
- [ ] **AI nukes and SAMs.** Nations rarely save enough gold for silos; make the
  economy-to-weapons path work on Medium and above.
- [ ] **Clearer feedback.** Show your incoming attacks and their troops, an alliance
  timer, and the gold earned by trade and trains.

## Gameplay

- Spawn-phase timer with the AI choosing spots at the same time as you.
- A tutorial map.

## Maps and content

- Convert more OpenFront maps (Britannia classic, Baltics, Hawaii, Luna, ...).
  It only takes a line in `tools/mapconv.py`.
- Large variants for more maps, and a "huge" size if memory allows.
- Map selection with previews and nation counts in the setup screen.

## Technical

- [ ] Make code smaller, to lower the RAM needed (about 120 KB today).
- [ ] Scroll the screen by copying video memory instead of redrawing it all. (important)
- [ ] Faster AI decisions on big games (per-tick AI budget).
- [ ] Run the PC harness in GitHub Actions on every push, with generated maps
  cached, and check that games stay deterministic.
- [ ] Automated emulator tests in CI (needs a ROM, so probably only for forks that
  provide one as a secret).

