# More Blood decal capacity companion

Build 6 keeps the enlarged **50x storage** confirmed in-game with build 4 and adds pressure-aware cleanup and bounded decal creation. Build-5 minigun logs confirmed exhaustion of the renderer's separate shared render-object registry before decal slots filled. Build 6 compiles and passes policy tests and native-code emulation; live minigun and recording-overlay validation is pending. The GPU storage implementation previously passed a separate actual D3D12 allocation/upload/readback test.

The companion enlarges the dynamic decal manager used by blood and bullet impacts. At the default setting it provides:

- 65,535 usable slots, capped by native 16-bit slot IDs (vanilla: 1,600).
- Global count-based retirement starting above 60,000, with a hard threshold of 65,471 (vanilla: 1,200 / 1,437). Category thresholds also apply.
- 100 MiB combined decal geometry storage and a 50 MiB global geometry retirement budget (vanilla: 2 MiB / 1 MiB).

Here, geometry means the vertices and indices that draw the decals. These buffers do not store shell casings or ordinary particle geometry. Blood, bullet holes and other effects that emit dynamic decals still share this manager. Existing fade timers continue to apply; keep the SMF mod's **Persistent Blood** option enabled for its extended blood timers. There is no guarantee of 60,000 visible marks: category/geometry pressure and fade timers can retire them earlier.

The renderer also has a separate 65,536-entry render-object registry shared with other scene/effect objects. Build 6 starts bounded expiry of old decals when this registry has 12,288 or fewer free handles, continuing until 16,384 are free. It temporarily rejects new decal geometry at 8,192 free handles or fewer, before creating persistent render clones. This keeps headroom for unrelated renderer allocations; these are observed counters with a safety margin, not an atomic reservation against all other threads.

Creation is limited to 256 additions per decal update and a token-bucket rate of 4,096 additions/second. Rounded geometry upload cost has a separate 8 MiB/second budget with a 256 KiB burst. Excess additions follow the existing native failure/unused-slot cleanup path. Capacity cleanup examines at most 1,024 old marks and schedules at most 256 new/accelerated expirations per category per update. Ordinary budget pressure uses a 0.15-second fade; shared-registry or hard-count pressure expires old marks immediately. A native geometry-allocation failure also requests a bounded immediate cleanup batch on the next update, covering individual index/vertex exhaustion or fragmentation below the combined budget. Actual reference, geometry and slot release remains native. Large buffers remain available, but the safe visible population depends on the map and other effects. More retained visible geometry can still increase rendering cost.

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

`Retail/MoreBloodDecals.diagnostic.log` records startup, actual allocations, dedicated buffer creation, live pool usage every five seconds, sampled controller fade settings, capacity-triggered retirement and exception details. Shared render-object usage is sampled twice per second and reported every five seconds with its sampled high-water mark. Policy reads also check this registry on each admission/update. Totals distinguish registry rejection, creation/upload throttling, allocation failure and expiry scheduling. Retirement reports aggregate selections at most once per second per category; selections can repeatedly include a decal already fading. Normal telemetry uses cached writes; fault/startup evidence flushes explicitly. No reports are uploaded. Retain the log if marks disappear unexpectedly or the game crashes. `ZHMModLoader.log` also reports runtime allocation confirmation.

Buildable source is in `../sdk_plugin_src` in the `Charc0al.MoreBloodCOMPLETE` repository. Research, validators and backups remain under the workspace's `agentTools/MoreBloodMelee`. Third-party notices are in `licenses`.
