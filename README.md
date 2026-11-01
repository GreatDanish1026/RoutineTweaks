# RoutineTweaks 0.1.0

Graphics add-on for **Routine** (PC, Steam), for ReShade with add-on support. Raises the game's
lighting and detail settings beyond what its menu offers, live, from an in-game
overlay. HDR is left to RenoDX.

- **Lighting**: contact-hardening soft shadows, longer shadow distance,
  GTAO ambient occlusion, reflections on rougher surfaces and steadier reflections.
- **DLSS**: the quality modes the menu lacks (DLAA, Quality, Balanced, Performance, Ultra
  Performance; they override the menu's resolution slider), and the DLSS 4 (K) or 4.5 (M, L)
  transformer model (needs DLSS 310.2 or
  newer in the game folder, for example via DLSS Swapper).
- **Detail**: texture memory beyond the game's 3 GB, 16x anisotropic filtering, texture
  sharpness, finer geometry, longer draw distance, and switches for bloom, motion blur, chromatic aberration, lens flares,
  depth of field, film grain and vignette.
- **Cinematic preset**: the lighting and detail upgrades at once (not texture sharpness or the
  DLSS settings, which are left to taste), about 3% slower (RTX 5080,
  4K, DLSS). **Measure preset costs** shows what it costs on your PC.
- **F8** switches every tweak off and on, to compare.

Every option starts at the game's own value.

## Requirements

- Routine for PC (Steam build 22236094 tested). If the game's code doesn't match, the add-on
  changes nothing and says so in the overlay and in `RoutineTweaks.log`.
- [ReShade](https://reshade.me) 6.8 or newer **with full add-on support**, installed for
  `Routine-Win64-Shipping.exe` with DirectX 10/11/12.

## Install

1. Install ReShade with add-on support into `...\Routine\Routine\Binaries\Win64`.
2. Copy `RoutineTweaks.addon64` into the same folder.
3. Start the game and press **Home** to open ReShade. The settings are on the **Add-ons** tab,
   under RoutineTweaks.

Settings are saved in `ReShade.ini` under `[RoutineTweaks]`. Nothing else is written to disk;
deleting the `.addon64` file removes the mod completely.

## Building

`.\build.ps1` builds `bin\RoutineTweaks.addon64` (needs Visual Studio 2022 with the C++ tools).
`.\build.ps1 -Install` also copies it into the game folder (close the game first). See
`docs/FINDINGS.md` for how the game's settings were found.

## License

MIT (see `LICENSE`). Third-party headers: see `THIRD_PARTY_NOTICES.txt`.
