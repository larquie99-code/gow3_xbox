# Texture transfers and memory pressure on Xbox

Target: Series S, 720p, a reported 5 GiB app limit and roughly 4.1 GiB average usage.
This is a resource-lifetime and transfer optimization, not a measured console FPS claim.

## Contracts and changes

### Dense buffer arenas

`GetDenseArena` must retain replaced buffers while local CPU draw preparation and
recorded/submitted GPU commands can still reference them. Previously, replacement
buffers accumulated in `BufferCache::arenas` until cache destruction.

`DenseArenaStore` keeps active and retired buffers in stable-address lists. Arena
merges still copy the full old contents and update all logical DMA views. Retired
objects stay alive across internal scheduler submissions. Only `Rasterizer::OnSubmit`,
after `DrainDrawPipe` and guest command completion, queues their destruction for the
current GPU timeline tick. No extra GPU wait is introduced. The log reports active
and pending-retirement bytes; VMA may retain empty heap blocks for reuse.

### Staging pressure relief

Before Xbox texture GC, the app usage/budget is compared with the existing threshold
(budget minus 768 MiB). Above it, reclaim completed staging allocations with a target
of up to 64 MiB per pass. Allocations are indivisible, so one large block can exceed
that target. This is not a hard memory cap or a guarantee of OS memory reduction.

Reclamation ignores normal idle retention under pressure, but preserves allocations
reserved in the current guest submission, incomplete GPU work and CPU-held deferred
readbacks. Both reservation ticks and later `last_gpu_tick` accesses are checked.
These latter-use checks also protect ordinary trimming and large-buffer reuse.
The ring's hint is reset after removals. There is no wait or forced submission.
Re-read actual app usage after releasing resources; keep existing texture eviction
and synchronous writeback safeguards if pressure persists.

On UMA, host memory and GPU allocations share an app budget. Do not add their numbers
as if they were independent RAM and VRAM capacities. Freeing a VMA allocation may
make space reusable without immediately returning an entire OS/driver heap block.

### Partial linear texture uploads

After existing dirty/hash checks select mips, linear images upload only the bounding
source range of those mips, including per-mip padding and all layers/depth slices.
Copy offsets are rebased without changing destination mip indices. The source start
is aligned down to 16 bytes to retain transfer/block alignment. Invalid ranges use
the original full range. Tiled translation still receives the complete original
layout. Native BC contents and resident image dimensions remain unchanged. Full
source ranges are retained for the explicit Xbox texture dump diagnostic.

The upload byte counter now counts the selected source range, after the empty-copy
check. It no longer counts a full image upload when all hashes matched.

## Measurement

With `SPARTAN_FRAME_STATS=1` in LocalState/graphics-debug.txt, `loader.log` reports:

- `Memory optimizations (total MiB)`: cumulative linear upload bytes avoided and
  completed staging allocation bytes released (not OS working-set savings).
- `App memory peak observed at submission`: sampled maximum, not all transient peaks.
- Existing frame times, shader/queue waits and GC reports.

Dense arena replacement logs report active/pending bytes separately. Compare the same
scene cold, warm and after app restart, keeping resolution/preset constant. Use short
instrumented captures and repeat without diagnostics because profiling has overhead.

## Verification

`texture-streaming-test` exercises actual Vulkan copies/readbacks, VMA allocation
counts, arena retirement across internal submissions, held and in-flight staging,
ring reuse after complete trimming, pressure thresholds and partial linear mip data.
Run in both direct and deferred recording modes with synchronization validation.
Existing image-barrier and GPU-memory tests cover related copy/descriptor boundaries.
UWP syntax checks cover the actual Xbox integration; native lavapipe cannot establish
Xbox/Dozen timing, total app memory savings or Windows package correctness.

## Research basis / further work

- [VMA budgeting](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/staying_within_budget.html): allocation reuse and budget limits are distinct.
- [Microsoft D3D12 residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency): dynamic budgets and UMA's shared pool.
- [Khronos memory allocation](https://github.com/KhronosGroup/Vulkan-Guide/blob/main/chapters/memory_allocation.adoc): pooled allocation and host-visible/device-local paths.
- [Khronos barriers](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/pipeline_barriers/README.adoc): preserve dependencies while avoiding unnecessary waits.

Other researched suspects still require console evidence: Dozen's late D3D12 PSO
variants, descriptor-cache turnover, guest CPU work and GPU overdraw/bandwidth.
Resident mip eviction, forced transcoding, automatic draw reordering and new queues
require further alias/format/order compatibility work; these changes do not add them.

## Validation result and remaining limitation

The native build, Xbox integration syntax checks, direct/deferred texture probes and
image-barrier test passed. The separate GPU-memory probe crashed once during graphics
pipeline library creation with `VUID-VkGraphicsPipelineCreateInfo-renderPass-06579`.
Eight repetitions of that configuration and additional GPL/monolithic checks passed,
but the intermittent failure remains unexplained. No pipeline implementation was
changed in this round; passing repetitions do not establish a fix. Console testing
and a full Windows package build remain necessary.
