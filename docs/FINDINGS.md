# Routine: findings

Game: Routine (Lunar Software), Steam app 606160, build 22236094. Unreal Engine 5.5
(`++UE5+Release-5.5-CL-40574608`), DirectX 12, project `Routine`. There's no PDB, no SteamStub
and no Denuvo (the exe's sections are plain, so its code can be read from disk).

Paths:
- Game: `E:\SteamLibrary\steamapps\common\Routine`
- Executable: `Routine\Binaries\Win64\Routine-Win64-Shipping.exe`
- User config: `%LOCALAPPDATA%\Routine\Saved\Config\Windows\` (`GameUserSettings.ini`). The
  shipping build writes no log to `Saved\Logs`.

Ships with DLSS (Super Resolution and Ray Reconstruction, `nvngx_dlss.dll` / `nvngx_dlssd.dll`),
Streamline frame generation and Reflex, and FSR. HDR is handled by RenoDX
(`renodx-routine.addon64`), so this add-on has no HDR tab.

The user's earlier Engine.ini ("Ultimate Engine Tweaks" by P40L0, read-only) was moved to
`Engine.ini.UltimateEngineTweaks.bak` on 2026-11-01 so the game runs on its own values.

Menu levels (GameUserSettings): scalability goes up to 4 (Cinematic) for shadows, reflections,
post-processing, textures and effects; view distance, AA, GI, foliage, shading at 3.

## Console variable layout (UE 5.5)

Same as Directive 8020 (also UE 5.5), checked from the exe on disk:
- 5 `r.ScreenPercentage` lookups load the singleton from the same RVA (0x87058c8) and call
  `[rax+0xb8]` (FindConsoleVariable).
- Calls right after `mov r8d, 0x0D000000` (SetByCode) go to `+0x98` (Set).

So `cvars.hpp` is unchanged from DirectiveTweaks.

## Game config in the pak

`Routine-Windows.pak` is version 11 with an unencrypted index; it holds `DefaultEngine.ini`,
`DefaultScalability.ini` and the engine's Base*.ini, but the files are Oodle-compressed and no
Oodle library is installed. Instead the add-on writes the game's live values of every relevant
console variable to `RoutineTweaks.log` the first time it connects in a session.

## How the game renders (log of 2026-11-01, highest menu levels)

Connected at the title screen (5 of 6 singleton matches, IsVariableBool at +0x40 as in
Directive 8020). `r.ScreenPercentage` reads 0 at first (UE 5 "automatic"), so the layout check
also reads `r.ViewDistanceScale` (1.0).

- **No dynamic GI**: `r.DynamicGlobalIlluminationMethod=0`, `r.ReflectionMethod=2` (SSR).
  Lumen is supported by the build (`r.Lumen.Supported=1`) but unused; the add-on offers it as an
  experiment only.
- **No hardware ray tracing**: `r.RayTracing=0` (start-up only), so no RT options.
- **Classic shadow maps**: `r.Shadow.Virtual.Enable=0`; `MaxResolution` and `MaxCSMResolution`
  4096, 10 cascades, `FilterMethod=0` (PCF), `DistanceScale=1`.
- AO: SSAO (`r.AmbientOcclusion.Method=0`). SSR quality 4, `r.SSR.Temporal=0`,
  `MaxRoughness=-1` (from post-process volumes).
- Already at the top: volumetric fog 4 px / 128 slices / 16 supersamples, bloom 5 at 100%,
  SSS quality 1, shadow SMRT values (unused without VSM).
- Texture pool 3000 MB, anisotropy 8, `r.Nanite.MaxPixelsPerEdge=1`, view distance 1.
- AA method 4 (TSR path; DLSS through the plugin), `r.ScreenPercentage=60` in play, DLSS-G off.
- Scalability groups go to 4 (`NumLevels=5`).

## Shadow resolution 8192 breaks close lamps

First Ultra+ (2026-11-01) raised `r.Shadow.MaxResolution` and `MaxCSMResolution` to 8192. In the
starting room the bed-alcove fluorescent light then lost its shadow (light spilled onto the
right wall, ceiling and floor) and the room's volumetric haze disappeared. Cinematic, which also
raised `TexelsPerPixel` to 2x, looked like Game. Likely cause: a close, screen-filling local
light asks for a shadow cube up to the cap, and 6 x 8192^2 faces don't get allocated. A second try kept
`MaxResolution` at 4096 and raised only `TexelsPerPixel` (2x / 3x) and `MaxCSMResolution`
(8192): the user saw the same blow-out with either choice. Changing shadow resolution at run
time breaks that light in this game, whatever the cause, so the option was removed (the game's
top level is already 4096 with 10 cascades).

## Preset costs

Measured 2026-11-01 with "Measure preset costs" (5 s settle, 8 s measured), RTX 5080, 4K, DLSS
at 70% screen percentage, no Frame Generation, standing in the starting room:

| preset | fps | frame time vs Game |
|---|---|---|
| Game | 106.6 | - |
| Ultra+ | 105.2 | +1% |
| Cinematic | 103.4 | +3% |

The game never re-applied its settings during the session (no "put back" in the log).

## Lumen

`r.GenerateMeshDistanceFields=0` and `r.AllowStaticLighting=1`: the game is built with baked
lighting and no mesh distance fields, so software Lumen only has screen traces and coarse data.
The game accepted `r.DynamicGlobalIlluminationMethod=1`, `r.ReflectionMethod=1` and the Lumen
detail values at run time, but the user saw no change, so the Lumen options were removed. Ultra+
(+1%) was dropped too: only Game and Cinematic remain.

## Extras (2026-11-01)

- DLSS: the game shipped DLSS 310.5 (SR, RR) and DLSS-G 310.5.2 with Streamline 2.10.3; the user
  swapped all three to 310.9.16 with DLSS Swapper (originals kept as `*.dll.dlsss`).
  `r.NGX.DLSS.Preset` (game value 0) takes 1-7 = A-G, 10 = J, 11 = K, 12 = L, 13 = M, 14 = N,
  15 = O per the plugin's help text; the add-on offers K, M and L.
- SSGI is `r.DynamicGlobalIlluminationMethod=2` (`r.SSGI.Quality` 1-4, default 4). The game took
  it but the user saw no change (as with Lumen), so it was removed: the levels apparently pin
  their GI method (likely post-process volumes).
- Contact shadows: `r.ContactShadows.OverrideLength` only covers directional lights ("for all
  directional lights"), so it can't add contact shadows to lamps or the flashlight. Not offered.
- The menu has no DLSS quality modes, only a resolution-scale slider writing `r.ScreenPercentage`
  (seen at 60 and 70). The add-on's DLSS quality option sets the standard scales and holds them.
  The slider's value has a higher priority than SetByCode (a SetByCode 66.667 read back 70), so
  any refused value is retried at SetByConsole (0x0E000000) and logged once.
- `r.FilmGrain` is separate from `r.Tonemapper.Quality`; `r.MipMapLODBias` exists.

## In-game results (2026-11-01)

- DLSS quality modes work (resolution scale taken at console priority).
- Shadow resolution, Lumen and SSGI were removed (see above).
