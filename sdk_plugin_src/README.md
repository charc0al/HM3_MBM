# MoreBloodDecals source

This is the canonical source for the companion DLL in `../sdk_plugin`. Build 4 at multiplier 50 has been confirmed in-game to retain many more decals. Further development of this mod belongs in `Charc0al.MoreBloodCOMPLETE`.

Build prerequisites: Visual Studio 2022 C++ Build Tools, a Windows SDK, CMake, and the ZHMModSDK **4.1.1 developer package**. Extract the SDK outside this repository and pass its location as `SDK_DIR`. SDK headers/libraries are external dependencies; the compatibility header forwards the SDK's DirectX include to the Windows SDK.

From this repository in the current workspace:

```powershell
cmake -S sdk_plugin_src -B ../../agentTools/MoreBloodMelee/repo-sdk-build -G "Visual Studio 17 2022" -A x64 -DSDK_DIR="X:/Games/HITMAN3_3.280/Simple Mod Framework/agentTools/NoHoldButtons/sdk-dev-4.1.1"
cmake --build ../../agentTools/MoreBloodMelee/repo-sdk-build --config Release --target MoreBloodDecals
```

Output is `<build directory>/bin/MoreBloodDecals.dll`. Builds stay under `agentTools`; copy a new DLL into `../sdk_plugin` and the game's `Retail/mods` only when ready to replace the tested binary. This source migration preserves that existing binary. Compilation uses C++23, `/MT`, `/W4` and UTF-8.

The companion checks the exact executable SHA-256, original capacity instructions and native call instructions before patching. `PatchSites.h` and `CallSites.h` contain generated guards; their generator remains under `agentTools/MoreBloodMelee/build_decal_patch_table.py`. Native slot IDs remain 16-bit; slots cap at 65,535 while count retirement thresholds and dedicated GPU geometry buffers scale up to 50x. `NativeResources.h` owns large D3D12 allocations, and `NativeBridge.h` connects their allocation/release and retention diagnostics to the native lifecycle. The module is pinned until game exit; fully restart to replace, disable or change settings. Do not restore old layout offsets while the enlarged manager exists.

See `../sdk_plugin/README.md` for installation, settings and testing, and `../sdk_plugin/licenses` for third-party notices. Research, validators, GPU test programs, dependencies, historical source snapshots and backups remain under `agentTools/MoreBloodMelee`. The source is kept in Git but omitted from the mod release ZIP and GitHub source archives, matching NoHoldButtons.
