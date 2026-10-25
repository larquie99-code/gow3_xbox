# Texture bursts and Xbox settings

The texture translator preserves the guest's texture formats, mip addresses and aliases.
`RefreshImage` copies guest data to a persistently mapped staging pool, `TileManager`
reorders tiled blocks on the GPU, and `Runtime::UploadImage` uploads the mip regions.
BC textures (including BC7) remain compressed throughout that path; detiling reorders
blocks, it does not decompress and recompress pixels.

## Changes

- Texture conversion compute pipelines now use their own `VkPipelineCache`. The normal
  cache database stores driver data and up to 128 learned pipeline variants. Compatible
  driver data is restored before gameplay; common 8/16/32/64/128-bit Thin1D/Thin2D
  detilers and learned variants are compiled on the loading screen. Cache files are
  checked for matching GPU/driver cache UUID. Unknown variants still compile on demand.
  Driver data/variant lists are checkpointed every 30 seconds when new variants appear.
- All mip uploads in one transfer use one bounding source range for dependency tracking.
  Padding is conservatively covered; copies and required barriers retain their order.
- Staging pool profiles control block size, retained ring memory, idle lifetime, and
  optional startup reservation of host upload/device detiling buffers. Allocations
  remain VMA-backed and protected by the existing submission ticks. Outstanding readbacks
  cannot be reused. These settings are retention preferences, not a total memory cap.
- StoreLogo and both Square icons use the new user-provided artwork; the splash is unchanged.

## Settings

Under **Image**, **Texture sampling detail** offers Original, +1 and +2 mip bias.
This is live and prefers lower-resolution mip samples for filtered color reads. It
can reduce sampling bandwidth at the cost of sharpness. Depth, point-filtered and
non-mipmapped samplers are excluded. Explicit shader LOD/fetch operations may ignore
this preference. **This does not reduce resident VRAM or the bytes uploaded.**

Under **Performance → Applied at the next start**:

| Setting | Effect |
| --- | --- |
| Texture staging pool: Compact | 16 MiB blocks, retain up to 32 MiB per ring, trim after 120 idle submissions |
| Balanced (default) | 16 MiB blocks, retain up to 64 MiB per ring, trim after 600 idle submissions |
| Streaming | 32 MiB blocks, retain up to 128 MiB per ring, trim after 1800 idle submissions |
| Preallocate texture staging | Reserve 16 / 48 / 192 MiB at startup for Compact / Balanced / Streaming |
| Prewarm texture translation | Build common/learned conversion pipelines before gameplay; default on |
| Background pipeline warmup | Default off on Xbox: finish cached game pipelines during loading; on permits background work |
| Batch image barriers | Default on: merge adjacent identical subresource transitions; disabling retains individual barriers |
| Pipeline disk cache | Existing setting also controls texture translation persistence |

Three ring types exist (device, CPU sequential, CPU cached). Unused rings are not all
preallocated. Active transfers, large allocations and held readbacks may exceed the
retained ring budget. More staging can reduce repeated allocations but also increases
memory pressure in Xbox's app budget. Explicit graphics-debug/environment overrides
retain precedence; active startup values and pending edits are displayed separately.
The existing VSync, async new-shader compilation and performance profiles remain available.

## Compression and residency limits

Forcing every image to BC7/ASTC is not implemented. BC7 is unsuitable for depth, integer,
HDR and storage/render-target usages; ASTC is not generally supported by the Xbox/Dozen
path. A format enum swap would corrupt data. New compression needs an encoder, format
capability checks, persistent converted assets and handling of writable/aliased images.
Encoding textures during an effect burst would itself add work to that burst.

Resident mip eviction/resizing is also not implemented: the emulator must preserve guest
mip views, explicit LOD reads, copies, readbacks, render targets and overlapping aliases.
A separate sampled-image representation with invalidation and fallback is needed before
an actual VRAM-reduction control can be truthful. The menu shows these limitations and
preserves native BC compression instead of offering ineffective compression switches.

## Console comparison

Start with Balanced, preallocation/prewarm/cache/barrier batching enabled and background
warmup disabled. Use the launcher's Precompile shaders option. Replay the same explosion
once cold and again without restarting, then after restarting with the cache retained.
Compare frame-time spikes and app memory. Try Streaming only if memory headroom allows;
try sampling +1 separately. Record scene, profile, cache state and run.txt logs including
`Texture staging` and `Texture translation`. No measured Xbox improvement is claimed
until the same scene is retested on the console.
