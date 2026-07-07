# Phase 5 — D3D11–Vulkan interop bridge

Status: complete

## Implemented slice

- ABI minor 1 adds optional, size-negotiated bridge create, pattern-submit,
  destroy, and status functions while preserving the Phase 4 prefix.
- An explicit R8G8B8A8 single-sample format/usage contract; unknown formats,
  extents, usages, and sample counts fail before either GPU API is called.
- Three D3D11 default textures created as shared NT-handle resources and one
  shared `ID3D11Fence` created through the D3D11.4 interfaces.
- Vulkan imports each texture as
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` and imports the fence as a
  timeline semaphore using the D3D11/D3D12 fence handle type.
- One monotonic ownership sequence per exchange: D3D release, Vulkan wait and
  copy, Vulkan ready signal, then D3D wait and copy.
- A tested bridge state machine for image ownership, ticket matching, epoch
  invalidation, wrap prevention, timeout, backend fault, and device removal.
- Per-ring-slot Vulkan completion fences. Command buffers and mapped upload
  slots are not reused until both the external timeline and Vulkan retirement
  oracles report completion.
- Deterministic Vulkan output with four color quadrants and a 24-bit frame
  number encoded in pixel 0.
- GPU-only D3D compositing into the current DXGI backbuffer. There is no CPU
  image readback in the game path and no spin wait.
- Destructive resize drain and epoch recreation. CPU waiting and
  `vkDeviceWaitIdle` are confined to explicit destroy/resize diagnostics.
- Opt-in game mode through `VISUALFORGE_BRIDGE_PATTERN=1`; validation remains
  independently controlled by `VISUALFORGE_VULKAN_VALIDATION=1`.
- Mirror health and trace writer classification. Vanilla world submission is
  still active and suppression remains off.

The host retains and closes every NT handle. The backend owns imported Vulkan
references until the bridge is drained. The renderer DLL remains loaded for
process lifetime, consistent with Phase 4.

## TDD/RGR evidence

The pure red implementation returned inert results. All seven initial Phase 5
cases failed with 14 intended assertions covering the format table, extent and
usage rules, configuration, epochs, ownership, resize, faults, and overflow.
The D3D integration then reached the backend and failed with the deliberate
`BridgeUnsupported` stub before any Vulkan bridge resource existed.

The first real exchange went green. A 256-exchange run then exposed a genuine
command-buffer retirement error: pixel output was correct, but validation
reported reset/begin/submit use of pending command buffers. Per-slot Vulkan
completion fences were added, and the same stress became clean. The diagnostic
also stopped printing a constant validation value and now propagates the real
backend error count.

Final focused result:

- 84 assertions across 9 Phase 5 unit cases.
- Debug and Release bridge GPU contracts passed.
- Accumulated Debug: 60/60 CTest tests passed.
- Accumulated Release: 60/60 CTest tests passed.
- Backend and plugin export contracts remain unchanged.

## GPU promotion stress

The Release stress result was:

    bridge-probe exchanges=10000 extent=64x64
    format=R8G8B8A8_UNORM ring=3
    quadrants=pass frame-index=pass
    resize-cycles=16 final-epoch=17
    validation-errors=0 handles=closed

Every exchange performed a D3D release, Vulkan wait/copy/signal, D3D wait/copy,
and blocking diagnostic readback comparison. The readback is confined to the
standalone test. Sixteen destructive drain/recreate cycles varied the extent
and incremented the history epoch.

## Live Mirror gate

Artifact:

    artifacts/phase-05/mirror-20260815-140000

The Release DLLs ran against Fallout's real 3440x1440 R8G8B8A8 swap chain and
logged:

    renderer-health schema=1 mode=Mirror backend=loaded suppression=off
    renderer-bridge: ready extent=3440x1440 format=R8G8B8A8_UNORM ring=3 epoch=1 sync=d3d11-fence-timeline validation-errors=0
    renderer-bridge: first-frame displayed release=1 ready=2 image=0 validation-errors=0 suppression=off

The four-quadrant debug image was copied through the real pre-Present DXGI path
while Fallout's world draw calls remained untouched underneath it.

## Default-Off regression

Artifact:

    artifacts/phase-05/off-20260815-143100

With no bridge/backend environment flags and no renderer DLL staged, no backend
or bridge log was emitted. The exact Off health, hook, and post markers passed.
Both live runs recorded an empty `restoreErrors` array, left zero game
processes, removed the staged backend, and restored the installed plugin hash:

    FCBB0032169C05BF992F77F96A6F3B5EA6C8AD7A5840DB8F516FCE90278FB29F

Window-system fullscreen/alt-tab permutations remain in the final display-mode
acceptance matrix; the ownership path they exercise is already covered by the
live swap chain and repeated destructive recreation tests.

## Promotion decision

Phase 5 is promoted. Phase 6 may render a synthetic indexed mesh into a Vulkan
HDR/depth frame and deliver its tone-mapped result through this bridge. Mirror
is the fallback oracle for that slice; Off remains the fallback for any bridge
or backend failure.
