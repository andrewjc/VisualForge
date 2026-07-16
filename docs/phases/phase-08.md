# Phase 8 — Texture, sampler, view, and mip mirroring

Status: offline implementation complete; live promotion gate pending

## Implemented slice

- A versioned, pointer-free `.vftex` container with a fixed 200-byte header,
  CRC, checked section ranges, zero-padding validation, immutable resource/view
  metadata, normalized sampler state, and per-subresource payloads.
- Exact observed DXGI contracts for typeless resources and legal linear/sRGB
  views across BC1–BC7. The Vulkan backend uploads the original compressed
  blocks rather than expanding source assets or guessing a color space.
- Checked block footprints, row-pitch repacking, cube/array metadata, mip and
  array addressing, and strict rejection of incomplete or malformed payloads.
- CPU BC1–BC5 decode and sampling oracles, including all six Vulkan cube-face
  selections. BC6H/BC7 remain GPU-native and are covered by mapping and packet
  contracts rather than an invented CPU decoder.
- Canonical white, flat-normal, and neutral-mask textures. Missing resources
  are explicit semantic fallbacks; they are not inferred from filenames.
- Sampler normalization and stable cache keys for filter, addressing,
  comparison, anisotropy, LOD, and border state.
- A residency state machine for contiguous mip publication, generation-safe
  promotion/eviction, descriptor-index quarantine, and timeline retirement.
- Raster packet version 2 carries interpolated UV coordinates and rejects all
  non-finite position/color/UV channels before submission.
- Backend ABI minor 3 consumes optional `.vftex` bytes through the two reserved
  frame words at offsets 48 and 56 while preserving the 80-byte request and all
  earlier offsets.
- A Vulkan sampled-image path with explicit compressed format, view, sampler,
  copy/barrier, descriptor, and lifetime handling. The legacy Phase 6 path
  remains available when no texture packet is supplied.
- An opt-in, one-shot D3D11 observation path for `CreateTexture2D`,
  `CreateShaderResourceView`, `PSSetShaderResources`, and `PSSetSamplers`. It
  correlates the exact SRV and sampler on a pixel-shader slot, keeps at most
  eight bounded candidates within 64 MiB, calls every original D3D operation
  exactly once, and never suppresses a draw.

## TDD/RGR evidence

The red suite covered every observed DXGI resource/view pair, odd compressed
extents, padded rows, cube faces, mip ordering, stale generations, descriptor
reuse, all semantic fallbacks, BC4/BC5 channels, sampler cache identity,
packet corruption, interpolated UVs, and non-finite vertices. GPU promotion
began only after the CPU sampling oracle was deterministic.

Final focused Phase 8 result:

- 202 assertions across 16 Phase 8 cases.
- 106/106 Debug CTest tests passed at the completed Phase 8 baseline.
- 106/106 Release CTest tests passed at the completed Phase 8 baseline.
- The current accumulated suite remains green at 120/120 in both Debug and
  Release after Phase 9.

## Replay and Vulkan evidence

The Release synthetic BC1 sRGB replay reported:

```text
extent=96x64 submission=1 differing=1229
max-error=4 mean-error=0.129517
probes=pass tolerance=pass validation-errors=0 result=pass
```

Debug and Release produced the identical PPM SHA-256:

    0995B678A4568BD8FCA2487A7ADB90DDB27E48FE036CDD634D2D41E480281A49

The captured Phase 7 mesh was then rendered at 128x96 with the same BC1 sRGB
fixture and validation enabled:

```text
source-vertices=289 source-indices=1536 attributes=7
translated-vertices=289 translated-indices=1536
submission=1 differing=3921 max-error=4 mean-error=0.276815
validation-errors=0 result=pass
```

Its PPM SHA-256 is:

    3CF61E74376762A3CAEFFDF7C093F8929BBAFA072913EAF5A1554013065C0355

The lossless artifacts are under `artifacts/phase-08/`.

## Live gate and promotion decision

The live capture is deliberately gated by
`VISUALFORGE_CAPTURE_TEXTURE_ONCE=1`; the output path can be selected with
`VISUALFORGE_CAPTURE_TEXTURE_PATH`. The hook transaction and milestone
diagnostics are implemented, but the final in-game `.vftex` observation has
not been run because Fallout is currently owned by an existing interactive
process. The harness will not attach to, terminate, or replace that process.

Phase 8 is therefore not promoted yet. Its offline format, sampling,
residency, ABI, replay, and Vulkan portions are complete and green. Promotion
requires a fresh bounded game-smoke run after the existing process exits,
followed by replay of the captured diffuse/normal/mask resource. Draw
suppression remains off.
