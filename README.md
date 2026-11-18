# Dynamic_Terrain_Deformation_Skse

Ground in Skyrim that actually takes a mark. Footprints, trenches through deep snow, wheel ruts, and blast craters are pressed into the landscape itself — real displaced geometry, not decals — and recover over time.

A standalone SKSE plugin for Skyrim SE/AE, with or without ENB.

Copyright (c) 2026 NearMidnightNow (NMN).

Licensed under the **GNU General Public License, version 3 only (GPL-3.0-only)**. See [LICENSE](LICENSE).

This is a heavily reduced standalone SKSE port of my own Community Shaders features — cs-footprints, cs-snowblanket, cs-snowcollision, and cs-snowbuildup.

Includes code adapted from Community Shaders. See [NOTICE](NOTICE) for credits and third-party licences.

## Snow height for animation mods

`NMN_SnowHeight` is a float behaviour graph variable: how far the snow surface is above an actor's root (feet), in Skyrim units. Flat snow reads about 35 at default settings and slopes can reach about 45. Players can change the snow height, so use thresholds rather than exact values.

It needs Behavior Data Injector. The plugin ships the JSON that declares the variable (`SKSE/Plugins/BehaviorDataInjector/NMN_DeformableTerrain_BDI.json`), so you don't need your own. Without Behavior Data Injector the variable doesn't exist, so nothing can read it.

Reading it:

- Open Animation Replacer: a Compare Values condition with a Graph Variable of type Float.

  ```json
  {
    "condition": "CompareValues",
    "requiredVersion": "1.0.0.0",
    "Value A": { "graphVariable": "NMN_SnowHeight", "graphVariableType": "Float" },
    "Comparison": ">=",
    "Value B": { "value": 20.0 }
  }
  ```

- Papyrus: `akActor.GetAnimationVariableFloat("NMN_SnowHeight")`
- SKSE: `actor->GetGraphVariableFloat("NMN_SnowHeight", value)` returns false when the variable isn't declared. `SetGraphVariableFloat` returns true either way, so don't use it as a test.

It reads 0 when:

- the actor is mounted, swimming, in the air, on ground that isn't snow, standing on something well above the land, or where snow is held back (roads, shelter);
- the actor is out of range: it covers the nearest 128 actors within 576 units, or up to 2304 with `EnableExtendedActorRange`;
- the snow raise or `EnableSnowHeightVariable` is off.

Footprints and trails don't change it: it reads the untouched snow surface. The plugin overwrites it every frame, so treat it as read-only. To check values in game, set `EnableLogging = 1` and `LogSnowHeight = 1` in `NMN_DeformableTerrain.ini`: the log gets a line a second with the player's value and the reason for it.

Tested in game, including an Open Animation Replacer condition.

## Building

Requirements:

- Windows x64.
- Visual Studio 2022 or Build Tools 2022, with the **Desktop development with C++** workload, MSVC v143, and a Windows SDK.
- Git, CMake 3.21 or newer, and Ninja available on `PATH`.
- A bootstrapped vcpkg installation, with `VCPKG_ROOT` pointing to its directory.

If you do not already have vcpkg, run the following in PowerShell, choosing a suitable installation directory:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat
```

Then clone and build the plugin from PowerShell:

```powershell
git clone --recurse-submodules https://github.com/maglarnet/Dynamic_Terrain_Deformation_Skse.git
cd Dynamic_Terrain_Deformation_Skse
$env:VCPKG_ROOT = "C:\dev\vcpkg"
.\build.cmd
```

Adjust `VCPKG_ROOT` if your installation is elsewhere. The script locates Visual Studio 2022 in its standard installation location, configures a Release build, installs the required vcpkg dependencies, builds the DLL, and runs the offline shader and deformation checks. The first build requires internet access for dependencies.

If you cloned without submodules, run `git submodule update --init --recursive` before building.

The built plugin is `build/release/NMN_DeformableTerrain.dll`. The existing DLL, INI, and texture names are intentionally retained. Runtime settings and textures are in `dist/`.

Building does not install the mod by default. If `SKYRIM_FOLDER` or CMake's `OUTPUT_FOLDER` is configured, CMake can copy the DLL to that location.
