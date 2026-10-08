# Credits

## Octane car model
- "Rocket League - Octane (SUPREME Edition)" by **claytor4**
- Source: https://sketchfab.com/3d-models/a9f5978c07304747a33de2646a2a0d80
- License: Creative Commons Attribution 4.0 (CC BY 4.0), https://creativecommons.org/licenses/by/4.0/
- Changes: Supreme logos removed; body repainted solid blue (`assets/octane/textures/Carroceria_BaseColor.png`); red trim recolored to blue (`Objetos_BaseColor.png`).

This is a fan-made recreation of the Octane. Rocket League and the Octane design belong to Psyonix / Epic Games; this project is not affiliated with or endorsed by them.

## Games
- Elden Ring (FromSoftware / Bandai Namco): host game, read from the player's own install. The DLC is not required.
- Rocket League (Psyonix / Epic Games): sounds read from the player's own install at runtime. No Rocket League files are included in this project.

## Libraries and tools shipped with the mod
- **vgmstream** (https://github.com/vgmstream/vgmstream), ISC license. Bundled `vgmstream-cli.exe` decodes two Rocket League sounds (ball hit, goal explosion) from the player's own Rocket League install into a local cache on first launch. Its bundled libraries (libvorbis, FFmpeg, mpg123 and others) keep their own licenses, listed in vgmstream's COPYING file.
- **ModEngine2** (https://github.com/soulsmods/ModEngine2) is installed by Melty, not shipped here.

## Research credits
- Memory layouts and byte patterns used by the runtime add-on were worked out with help from the community's Elden Ring Cheat Engine tables: The Grand Archives (https://github.com/The-Grand-Archives/Elden-Ring-CT-TGA) and Hexinton's table (https://github.com/Hexinton/eldenringcheatengine). No code from those tables is included.
- Effect and projectile names from Paramdex (https://github.com/soulsmods/Paramdex).
