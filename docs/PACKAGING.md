# Packaging plan (Melty)

Melty installs ModEngine2 and starts Elden Ring offline with Easy Anti-Cheat off. Melty does not allow the games' own
content in the upload, so every file derived from Elden Ring is **built on the player's PC from their own copy** on
first launch. Rocket League stays a companion: only two sounds are decoded from the player's install.

## What ships
| Component | Contents | License |
|---|---|---|
| `rlmod/rlmod.dll`, `rlmod/rlmod_core.dll` | runtime add-on (loader + hot-reloadable core) | this project |
| `rlmod/setup/` | self-contained build tool (see below), published as one .NET 8 executable | GPL-3.0 (links SoulsFormats) |
| `rlmod/assets/` | fan-made Octane (CC BY 4.0, claytor4), generated ball mesh/texture, car textures | CC BY 4.0 / this project |
| `rlmod/vgmstream/` | vgmstream-cli and its libraries | ISC (+ bundled libs' own licenses) |

## First-launch build (on the player's PC)
1. Read the needed files from the player's Elden Ring archives (`Data0-3.bdt`, `DLC.bdt`): `chr/c8000`, `c8002`,
   `c8002_l` texbnd, `c8101`, `c8120_h` texbnd (texture template), `map/mapstudio/m60_42_36_00.msb.dcx`.
   Our own reader (archive headers use the public RSA keys every Elden Ring modding tool uses); no UXM code.
2. Build into ModEngine2's mod folder:
   - `chr/c8002.chrbnd.dcx`: Torrent body replaced by the Octane (4 m, reversed, double-sided, bone `c8002_Body`,
     material based on `ROPE`, cloth removed), plus exhaust marker 900 at (0, 0.7, 1.75)
   - `chr/c8000.chrbnd.dcx`: exhaust marker 900
   - `chr/c8002_l.texbnd.dcx`, `chr/c8002_h.texbnd.dcx`: car textures
   - `chr/c8101.chrbnd.dcx`, `chr/c8101_h.texbnd.dcx`: the ball
   - `map/mapstudio/m60_42_36_00.msb.dcx`: ball placed next to Tree Sentinel
3. Decode the Rocket League ball-hit and goal sounds with vgmstream into `%LOCALAPPDATA%\RadahnLeague\sounds`.

Rebuild when the Elden Ring version changes (store the exe version next to the outputs).

## Recipe (draft)
- Elden Ring: host, loader `modengine2`, `external_dlls = rlmod\rlmod.dll`, mod folder with the built files.
- Rocket League: companion, never launched; its folder passed as `RADAHN_LEAGUE_RL_DIR`.
- Solo only.

## Open items
- Replace the UXM-derived archive code with our own reader.
- Publish the setup tool self-contained (players don't have .NET installed).
- Add `LICENSE` (GPL-3.0 for tools) and confirm the content license / remix choice with the owner.
- Real gameplay clip for the listing.
