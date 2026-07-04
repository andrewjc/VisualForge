# Phase 4 — Backend ABI and Vulkan capability probe

Status: complete

## Implemented slice

- Stable C ABI 1.0 with size-negotiated POD host callbacks, probe request,
  capability report, and backend API records.
- Lazy `VisualForgeRenderer.dll` loading with an exact
  `VFRenderer_QueryInterface` export contract.
- Fail-open host errors for a missing backend, missing Vulkan loader, load or
  export failure, ABI mismatch, invalid callbacks/API, probe failure, and
  repeated lifecycle requests.
- Process-lifetime backend-module policy. A loaded renderer DLL is never
  unsafely unloaded while Fallout can retain backend code or Vulkan objects.
- Vulkan instance and device probe with optional validation/debug messenger.
- Exact D3D11 adapter-to-Vulkan physical-device matching through the Windows
  adapter LUID.
- Required extension, feature, queue, BC-format, descriptor-limit,
  acceleration-structure, ray-tracing, and external-handle checks.
- D3D11 texture-import and D3D12 fence-import candidate queries.
- Opt-in in-game handshake through `VISUALFORGE_BACKEND_PROBE=1`; the existing
  D3D11 path remains authoritative and no draw is suppressed.
- Standalone `vf_backend_probe` diagnostic executable and a GPU CTest gate.
- Reversible smoke staging for the backend DLL, including delayed F4SE/Steam
  handoff tracking before restoring the installed files.

## TDD/RGR evidence

The pure-contract red suite produced 24 intended failures spanning ABI layout,
size negotiation, callbacks, module/export failures, LUID selection, missing
capabilities, queue selection, typed device failure, and lifecycle behavior.
The first live GPU red failed at the deliberately unimplemented Vulkan device
creation boundary. The implementation then made the same tests green without
weakening their expected values.

Final focused result:

- 68 assertions across 8 Phase 4 unit cases.
- Backend DLL export contract: exactly one named function.
- Live Vulkan GPU contract: passed with validation enabled.

Accumulated results:

- Debug: 50/50 CTest tests passed.
- Release: 50/50 CTest tests passed.
- Plugin export contract remains exactly `F4SEPlugin_Load` and
  `F4SEPlugin_Version`.

## Capability gate

The selected adapter report is:

    device="NVIDIA GeForce RTX 4090" driver-name="NVIDIA"
    vendor=0x10de device=0x2684 api=0x404149
    capabilities=0x3fff missing=0x0 required=pass queue=0
    bc=on d3d11-import=on d3d12-fence=on
    per-stage-sampled=1048576 set-sampled=1048576 push-constants=256
    ray-recursion=31 shader-group=32 as-scratch-align=128
    validation-errors=0 unload=deferred

This phase creates and destroys a probe device only. It does not submit work,
import a D3D resource, or present a Vulkan image.

## Live game gates

Backend capability artifact:

    artifacts/phase-04/backend-20260815-131100

The Release plugin loaded the Release backend at the actual swap-chain device,
matched the RTX 4090 LUID, logged all required capabilities plus both interop
candidates, and reported zero validation errors. Renderer health remained
`mode=Off backend=absent suppression=off` because capability readiness is not
yet an active rendering mode.

Default-Off regression artifact:

    artifacts/phase-04/off-20260815-131000

With the opt-in variables absent and no backend staged, the plugin produced no
backend log and preserved the Phase 2/3 Off health, hook, and post markers.

An earlier Off rerun exposed that F4SE can complete a delayed Fallout handoff
after its initially observed process exits. The harness refused a concurrent
run, but its first restore attempt then encountered a loaded DLL. The exact
run-owned process exited, all six recorded backups were restored and hash
verified, and the harness was hardened to stop the loader first, watch for all
run-owned delayed Fallout processes, collect every restore error, and always
write restoration status. Both promotional smokes report an empty
`restoreErrors` array and zero surviving game processes.

Restored installed DLL SHA-256:

    FCBB0032169C05BF992F77F96A6F3B5EA6C8AD7A5840DB8F516FCE90278FB29F

The installed backend returned to absent after the backend smoke.

## Promotion decision

Phase 4 is promoted. Phase 5 may create an isolated D3D11/Vulkan shared-image
bridge in Mirror/debug mode. Off remains the inherited fallback oracle, and no
vanilla draw may be suppressed.
