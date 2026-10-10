# More Blood decal capacity companion

Build 5 keeps the **50x retention** behavior confirmed in-game with build 4. It adds diagnostics for a separate shared render-object registry and reduces disk logging during capacity pressure. Build 5 compiles and passes isolated diagnostic tests; its new telemetry still needs an in-game stress test. The capacity/resource implementation previously passed native-code emulation and a separate actual D3D12 buffer allocation/upload/readback test.

The companion enlarges the dynamic decal manager used by blood and bullet impacts. At the default setting it provides:

- 65,535 usable slots, capped by native 16-bit slot IDs (vanilla: 1,600).
- Global count-based retirement starting above 60,000, with a hard threshold of 65,471 (vanilla: 1,200 / 1,437). Category thresholds also apply.
- 100 MiB combined decal geometry storage and a 50 MiB global geometry retirement budget (vanilla: 2 MiB / 1 MiB).

Here, geometry means the vertices and indices that draw the decals. These buffers do not store shell casings or ordinary particle geometry. Blood, bullet holes and other effects that emit dynamic decals still share this manager. Existing fade timers continue to apply; keep the SMF mod's **Persistent Blood** option enabled for its extended blood timers. There is no guarantee of 60,000 visible marks: category/geometry pressure and fade timers can retire them earlier.

Large decal buffers now use dedicated D3D12 resources instead of the generic renderer's fixed 4 MiB pages. The native upload and deferred destruction paths remain responsible for their use and retirement. Allocation failure rejects the new decal before reserving geometry. The DLL stays resident until game exit so native callbacks remain valid.

Requires **ZHMModSDK 4.1.1** and supported HITMAN 3 **3.280** executable SHA-256 `005412cf59daa1aadfba8deb05f097f5418aec57ddc98216483d93bf5b0119b1`.

## Install and configure

1. Close HITMAN 3.
2. Copy `MoreBloodDecals.dll` and `MoreBloodDecals.ini` to `Retail/mods`.
3. Enable MoreBloodDecals in the SDK, or add an empty `[MoreBloodDecals]` section to `Retail/mods.ini`, preserving existing sections.
4. Fully restart the game. SMF does not install this companion automatically.

```ini
[decals]
enabled = true
capacity_multiplier = 50
```

Multipliers 1..50 are accepted; one uses vanilla capacity. Geometry and soft retirement thresholds scale fully; physical slots stop increasing at 65,535. Settings changes, disable and live unload require a full game restart to take effect. The executable file on disk is unchanged. More persistent decals can increase GPU memory use and rendering cost.

## Test and diagnostics

Test repeated shots, NPC blood, glass impacts, save/load, mission restart and map changes. Compare the first marks against the previous build. Native initialization, free-list traversal, retirement, high-ID geometry insertion, upload and cleanup pass offline checks. Separate real GPU tests verify the tail regions of both full-size buffers; they do not prove live game integration.

`Retail/MoreBloodDecals.diagnostic.log` records startup, actual allocations, dedicated buffer creation, live pool usage every five seconds, controller fade settings, capacity-triggered retirement and exception details. Shared render-object usage is sampled twice per second and reported every five seconds with its sampled high-water mark. This allocator includes non-decal render objects and is separate from the enlarged decal slots. Retirement reports aggregate selections at most once per second per category; selections can repeatedly include a decal already fading. Entries flush immediately; no reports are uploaded. Retain the log if marks disappear unexpectedly or the game crashes. `ZHMModLoader.log` also reports runtime allocation confirmation.

Buildable source is in `../sdk_plugin_src` in the `Charc0al.MoreBloodCOMPLETE` repository. Research, validators and backups remain under the workspace's `agentTools/MoreBloodMelee`. Third-party notices are in `licenses`.
