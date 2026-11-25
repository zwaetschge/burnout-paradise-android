# Remastered vehicles (optional)

*Burnout Paradise Remastered* contains 41 vehicles the Ultimate Box does not have: 33 cop cars,
Big Surf Island cars and toy versions of the legendary cars. If you own **both** games, the converter
in `tools/remastered_cars/` turns them into an overlay folder that the port reads on top of the
Ultimate Box data. Nothing in your game folders is modified.

```bash
./scripts/build-remastered-cars.sh --game-dir /path/to/BurnoutParadiseUltimateBox \
    --remastered-dir /path/to/BurnoutParadiseRemastered
./scripts/prepare-game-files.sh --game-dir /path/to/BurnoutParadiseUltimateBox --mods ./mods
```

What the overlay contains (all generated from your files, never redistribute it):

- converted vehicle and wheel bundles (Direct3D 11 resources of the Remastered rewritten for Direct3D 9;
  resource ids that collide with Ultimate Box ids are renamed),
- extended vehicle and wheel lists,
- language files with the new names and descriptions; cop cars without a description of their own
  show the description of their base model,
- junkyard pictures: the cop cars get the Remastered cop icon library in their own library slot; two
  island cars show the picture of their base model. The four toy legends currently show an empty card.

What the runtime does with it:

- the cars are added to the garage from the start (like in the Remastered), without any DLC pack being
  marked as owned,
- cop cars appear in their own **Cop Cars** tab, island and legend cars under **Paradise Cars**.

Set `PORT_MOD_ROOT=0` in `port.env` to turn the overlay off. Cars that were already added to a save stay
in it.
