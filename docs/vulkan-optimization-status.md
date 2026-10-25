# Renderer optimization audit

This app translates the original game's graphics/compute command stream. It does
not own an engine scene graph with independent object bounds, particle systems or
physics tasks. Optimizations must preserve guest memory writes, queries, aliases
and draw ordering. Enabling an extension alone does not move that work to the GPU.

## Implemented paths

| Request | Current implementation and limits |
| --- | --- |
| Persistent `VkPipelineCache` | `vk_pipeline_serialization.cpp` restores/merges driver cache data on startup, checkpoints new compilations at 30-second intervals and saves during `Sync()`. The per-game store writes `driver.vkcache`. Worker caches are merged before checkpoints. This reduces repeated driver work; it does not guarantee instant pipeline creation or eliminate compilation after a driver/game/profile change. |
| VMA suballocation | Buffers, texture cache, sparse backing and FSR 4.1.1 share VMA. Preferred large-heap blocks are 128 MiB on desktop, 64 MiB on Xbox. Dedicated allocations remain possible for driver-required/large allocations. The Xbox Vulkan/D3D12 exported presentation image retains its dedicated external-memory allocation; it is not a per-texture allocation loop. Third-party SDK allocators have their own policies. |
| Indirect rendering | `Rasterizer::DrawIndirectRecord` already issues `drawIndirect`, `drawIndexedIndirect` and their count variants from guest argument/count buffers, including GPU-written arguments. `DispatchIndirectRecord` supports indirect compute dispatch. These paths preserve the guest's own generated arguments; they do not add a new culling algorithm or generated pipeline/state commands. |
| Threaded command recording | `Scheduler` owns a `CommandPool`, records deferred commands on its recorder thread, and synchronizes before direct access/submission. Xbox enables `SPARTAN_DRAW_PIPE=1` and `SPARTAN_VK_RECORD_THREAD=1` by default. This overlaps preparation and recording; it is not scene-wide parallel recording into many secondary command buffers. |
| Batched image dependencies | `Runtime::FlushBarriers` already emits the accumulated image/memory barriers in one `pipelineBarrier2`. This change also compacts equivalent partial mip/layer ranges within each `Image::GetBarriers` transition. It preserves exact slice coverage, stages, access masks, layouts, aspects, image and queue ownership. Read-after-read skips remain; write-after-write dependencies remain. |
| CPU-visible GPU memory | Mapped uploads and stream-placement selection already exist. Xbox startup now defaults `SPARTAN_STREAM_MEMORY` to `auto` instead of forcing `cached`, allowing compatible unified device-local/host-visible memory to be preferred. Explicit overrides still win. Discrete desktop auto preserves cached-host placement; `device` is an existing opt-in preference for BAR/SAM experiments. VMA can fall back when resource memory-type compatibility/budgets require it. BAR visibility alone does not prove fast CPU access. |
| Less rigid pipeline compilation | Feature-gated graphics pipeline libraries and background compilation already exist, with monolithic fallback and pipeline binary support. These are separate from shader objects. |

Xbox exposes startup preferences for pipeline disk caching, the dedicated recorder
and upload-memory placement through the Performance settings tab. Four presentation
profiles combine resolution, VSync, frame limit and queued frames; these do not
enable additional rendering backends. See [settings combinations](../xbox/README.md#settings-combinations-and-renderer-controls).

See [memory policies and regression commands](vulkan-memory.md) for allocator,
cache, mapped-write retirement and descriptor-cache details.

## Extensions and architectural work not implemented

### GPU culling / generated commands

`VK_NV_device_generated_commands` is a vendor extension, not a portable Xbox/AMD
requirement; Direct3D `ExecuteIndirect` is a different API. Neither its support nor
support for the newer `VK_EXT_device_generated_commands` is negotiated by this
renderer. No NV/EXT generated-command path has been added.

Adding host-side GPU culling needs a validated source of per-object bounds,
camera and visibility rules, plus a way to classify draws with shader/storage
side effects. The current guest command stream does not expose a generic safe
object-bounds contract. Dropping apparently off-screen draws could suppress
atomics, queries, writes or effects the game observes. Retain the existing indirect
paths until a specific game workload supplies that contract and can be tested.

### Parallel scene command buffers

A scheduler pool belongs to one recording stream. It is never safe to concurrently
record against the same pool without external synchronization. Parallel scene
recording would require independent pools and descriptor/command allocation per
worker, immutable draw-state snapshots, explicit work dependencies, render-pass
boundaries and ordered execution. Shared guest register/resource state currently
requires ordered handling. No new worker count is presented as if it enabled
parallel scene recording.

### `VK_EXT_shader_object`

Vulkan 1.3 by itself does not guarantee this extension. The renderer currently
binds pipeline handles with pipeline-derived dynamic/static state and specialization
keys. Shader objects require extension feature negotiation, linked shader/interface
handling and complete dynamic-state emission in every draw path, including
Dozen's effective PSO state workarounds. Replacing pipeline handles alone would
break that contract. This is an evaluated future backend; no shader-object path
is claimed. Existing pipeline libraries/cache are the working compilation path.

### Dedicated async compute

Guest compute queues are decoded, but host dispatches run through the ordered
graphics scheduler. Device creation requests graphics/present and sparse queues;
it does not create an independent compute submission scheduler. Moving those
dispatches to another queue would need a cross-queue dependency graph, timeline
semaphore waits/signals, resource lifetime and queue-family ownership handling,
including graphics/compute aliases and guest-visible memory fences. Postprocessing
that immediately consumes the frame also has a real graphics dependency. No
unused queue is created as a substitute for actual overlap. A measured independent
workload and target-driver queue support are prerequisites for this work.

## Validation and target checks

The Linux `image-barriers-test` covers exact coverage on mixed mip/layer states,
nonmatching dependency fields, gaps, range overflow, opaque `pNext` chains and
transition-call boundaries. Its live path uses production `Image::GetBarriers`
on a VMA-backed image, clears a partial mip/layer rectangle and reads pixels back.

```sh
ninja -C out/gpu spartanportgpu image-barriers-test
out/gpu/image-barriers-test --cpu
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation SPARTAN_VK_VALIDATION_SYNC=1 \
  out/gpu/image-barriers-test
```

Repeat the live test with `SPARTAN_TEST_THREADED_RECORDING=1` for deferred recording.
The uniform CPU case reduces 128 image barrier entries to one; this is a count
assertion, not a game frame-time or memory-saving claim. Target console testing
must compare the same scene, game/profile cache, TV settings and frame-time
captures. Compare `SPARTAN_STREAM_MEMORY=auto` with `cached` explicitly; device
heap selection and CPU write performance depend on Dozen and the Xbox driver.
Windows SDK/console validation and original-game profiling are still required.
