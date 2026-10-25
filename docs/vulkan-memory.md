# Vulkan memory allocation

The renderer shares one Vulkan Memory Allocator (VMA) instance between its texture
cache, buffers, presenter and FSR 4.1.1 replay. VMA is already a build dependency
(`GPUOpen::VulkanMemoryAllocator`); no additional allocator implementation is needed.

Desktop builds prefer 128 MiB blocks on large heaps. Xbox/UWP prefers 64 MiB
(previously 16 MiB), the lower end of the requested range, to limit the increase
in reserved memory under its constrained application budget. These are
preferred block sizes, not reservations at startup or hard limits: VMA adapts to
heap sizes and memory pressure and can use dedicated allocations for large resources
or when the driver requires them.

Buffers with device addresses use normal VMA suballocations. The allocator enables
`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`, so VMA supplies the required
memory allocation flags without forcing a dedicated allocation per buffer.

Sparse arena backing uses `vmaAllocateMemory` with the arena's memory type and block
alignment. Vulkan sparse binds use both `VmaAllocationInfo.deviceMemory` and its
byte `offset`. Resident ranges count sparse blocks; splitting and merging ranges
converts block counts to bytes. Backing allocations remain alive while arenas may
reference them and are freed after those buffers at cache teardown. As with other
renderer resources, GPU work must be drained before teardown.

FSR 4.1.1 receives the caller's allocator, which must outlive the upscaler. Its
constant and staging buffers retain their coherent mapped-memory requirement.
The standalone FSR benchmark owns its own allocator and releases its resources
before destroying it. External SDKs (FSR 3/v07 and Dear ImGui) retain their own
internal allocation policies.

## Regression checks

After the normal GPU configuration:

```sh
ninja -C out/gpu vulkan-suballocation-test fsr4-bench
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation out/gpu/vulkan-suballocation-test
c++ -std=c++23 -Igpu/shadps4 tests/test_sparse_backing.cpp -o /tmp/sparse-backing-test
/tmp/sparse-backing-test
```

The Vulkan test (Linux) requires Vulkan 1.2 and buffer device addresses; Mesa
lavapipe is sufficient. It creates 5,000 renderer buffers, checks that they share
fewer than 16 driver allocations, refills freed space with smaller buffers,
verifies GPU writes through every mapped allocation, and checks that all VMA
allocations are released. The CPU test covers sparse range offsets independently
of hardware sparse-binding support. Actual sparse queue binding still requires a
GPU/driver supporting sparse residency; game performance must be measured with
real game assets on the target hardware.

## Heap selection and uploads

`SPARTAN_STREAM_MEMORY=auto` is the default. Integrated/CPU Vulkan devices (and
Xbox/UWP) with a compatible DEVICE_LOCAL | HOST_VISIBLE type prefer unified
memory. Discrete GPUs keep the cached-host stream placement; a visible PCIe BAR
alone does not imply fast CPU writes. Existing `device`, `host` and `cached`
overrides remain available. Xbox startup now defaults to `auto` too; an explicit
`graphics-debug.txt`/environment override still takes precedence.

GPU arenas prefer device-local memory. On unified devices VMA may persistently
map them; uploads then write and flush the allocation directly, avoiding staging
and `vkCmdCopyBuffer`. This is allowed only after all previous GPU use has
completed and before any use in the current recording tick. Otherwise the
existing asynchronous staging path runs. Noncoherent allocations are flushed;
coherent memory remains preferred for frequently written host buffers. Host
writes participate in the renderer's barrier tracking. Mapping lasts for the
allocation lifetime.

The existing 32 MiB constant ring, 128 MiB stream buffer and shader push constants
remain in use. Descriptor-set layouts convert up to eight single storage-buffer
bindings (within device limits) to dynamic descriptors. Push-descriptor layouts
retain their required non-dynamic types. Draws supply aligned dynamic offsets;
immutable sets with identical contents are reused within a submission. The cache
is bounded to 256 entries and invalidated on submission/pool rotation. Resources
with different ranges or handles require distinct sets. This reduces set updates;
draws still bind the appropriate set and offsets.

## Pipeline compilation and persistence

The driver `VkPipelineCache` is restored in on-demand and background startup as
well as bulk warm-up. `driver.vkcache` lives in the existing per-game cache
folder. New pipelines trigger checkpoints at most once every 30 seconds; worker
caches are merged before saving through the existing asynchronous, atomic file
writer. A forced process exit can lose recent compilation work or queued writes.
`Sync()` also saves before closing the store. `SPARTAN_PIPELINE_CACHE=0` disables
persistence.

When supported, `VK_EXT_graphics_pipeline_library` splits graphics pipelines into
four parts, compiles pre-raster shaders on an additional bounded worker and links
the parts. Libraries remain owned until their linked executable pipelines are
destroyed. Existing background/early pipeline compilation remains active. Drivers
without the extension use monolithic creation; compilation/link failures fall
back to that path too. Pipeline binary caching takes precedence when available.
`SPARTAN_PIPELINE_LIBRARIES=0` disables library compilation for comparisons.

## Diagnostics and texture formats

Use `SPARTAN_VK_VALIDATION=1` and optionally `SPARTAN_VK_VALIDATION_SYNC=1` with the
Khronos validation layer installed. Complete orderly shutdown to check live Vulkan
objects; forced process termination bypasses destructor diagnostics.
`SPARTAN_GPU_MEMORY_STATS=1` reports VMA live allocation/block usage, heap budgets,
direct-upload bytes, descriptor cache hits/writes and texture live/peak allocation
bytes per format at teardown. Heap budgets include driver estimates and should
not be interpreted as application allocation totals.

BC1–BC7 guest textures already retain their compressed formats. Automatic
recompression or downscaling is not enabled: guest images can also be render
targets, storage images or aliases of uncompressed views, so changing their
format/extent can break rendering and readback. Use the per-format counters and a
RenderDoc, Nsight or Radeon GPU Profiler capture on target hardware to select
static sampled assets for an offline compression/size experiment. Compare memory
peaks, image quality, frame times and stability with the same scene; no texture
quality or FPS claim can be made without the original assets and hardware.

Additional Linux regression (Vulkan 1.3, unified mapped heap, synthetic shaders):

```sh
glslangValidator -V --target-env vulkan1.3 tests/memory_probe.vert -o /tmp/memory_probe.vert.spv
glslangValidator -V --target-env vulkan1.3 tests/memory_probe.frag -o /tmp/memory_probe.frag.spv
ninja -C out/gpu gpu-memory-optimizations-test
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation SPARTAN_PIPELINE_BINARIES=0 \
  SPARTAN_GPU_MEMORY_STATS=1 out/gpu/gpu-memory-optimizations-test \
  /tmp/memory_probe.vert.spv /tmp/memory_probe.frag.spv
```

Repeat with `SPARTAN_PIPELINE_LIBRARIES=0` to cover monolithic fallback and with
`SPARTAN_TEST_THREADED_RECORDING=1` to exercise deferred descriptor recording. The probe
checks mapped writes/readback, submission retirement, descriptor reuse with dynamic
offsets, library compilation/linking and driver cache restoration. It does not
exercise game shader translation, real game cache warm-up or console performance.

See [the renderer optimization audit](vulkan-optimization-status.md) for current
indirect/threading support, image-barrier compaction and the prerequisites for
generated commands, shader objects and dedicated async compute.
