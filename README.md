# Radahn League

An Elden Ring × Rocket League mashup (work in progress): drive an Octane and hit a ball into Radahn.

**Status:** early development. The Octane model now renders inside Elden Ring (tested in place of a Stormveil ballista through ModEngine2). Not playable yet.

## How it works
- **Elden Ring** is the host game, loaded through ModEngine2, offline with Easy Anti-Cheat off.
- **Rocket League** is a companion: its sounds will be read from the player's own install at runtime. No Rocket League files are in this repository.
- The car is a fan-made Octane model (CC BY 4.0), see [CREDITS.md](CREDITS.md).

## Tools
- `tools/octane2flver`: converts the Octane FBX into Elden Ring's model format, replacing a template character model's mesh.
- `tools/erextract`: development helper that copies individual files out of the developer's own Elden Ring install for use as templates. Game files are never committed here.
