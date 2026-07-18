# Phase 9 — Authored specular/smoothness material translation

Status: offline implementation complete; live material-capture gate pending

## Implemented slice

- `MaterialCapture` resolves every scalar, vector, and semantic texture from
  explicit provenance. Higher-provenance sources win; contradictory values at
  equal provenance fail instead of being selected by iteration order.
- Raw authored values and their provenance survive translation alongside
  bounded evaluation values. Alpha, cutoff, smoothness, specular scale,
  Fresnel power, UV transforms, and specular color all have explicit ranges.
- Base color owns an sRGB view. Normal and smooth/spec data own linear views.
  The selected material semantics—not compression or filename suffixes—decide
  color space and channel meaning.
- The default lighting contract decodes smooth/spec R as specular weight and G
  as smoothness, distinguishes tangent-space BC5 from model-space RGB normals,
  applies UV transforms, and distinguishes opaque alpha from coverage alpha.
- Missing base, normal, and smooth/spec maps resolve to the Phase 8 white,
  flat-normal, and neutral-mask resources with fallback provenance.
- No metalness map is inferred. The shared evaluator derives colored `F0` and
  perceptual roughness from authored specular color, R/G texture data, and
  scalar controls, then evaluates a GGX direct-light lobe.
- The 16-entry transfer table is explicitly versioned. Version 1 is the only
  implemented contract; encode, decode, CPU evaluation, GPU-record creation,
  and the Vulkan backend reject every other version fail-closed.
- A bindless identity table maps stable texture resource/generation pairs to
  descriptor indices. Separate static and dynamic records avoid descriptor
  churn when only controller-like material values change.
- Shader reflection generates and verifies a 64-byte static record and a
  48-byte dynamic record. Their layout hash is
  `0xF97A35789BC84031`; the checked reflection dump is
  `artifacts/phase-09-material-layout-reflection.yaml`.
- A pointer-free `.vfmat` bundle has a fixed 80-byte header and 288-byte
  material record, embeds three exact `.vftex` packets, validates CRC and zero
  padding, and rejects semantic texture substitution.
- Backend ABI minor 4 reuses the final reserved frame words for material data
  at offsets 64 and 72 while retaining the 80-byte request and the Phase 8
  texture offsets.
- The Vulkan material path binds base, normal, and smooth/spec sampled images
  plus the reflected static/dynamic records. Legacy mesh and single-texture
  pipelines remain intact. The shared GLSL GGX evaluator mirrors the CPU
  oracle.
- `vf_packet_replay` can create/replay material bundles, render the controlled
  sphere sweep, render a synthetic Vulkan frame, and apply the same `.vfmat`
  to an engine-captured `.vfmesh`.

## TDD/RGR evidence

The material suite started red for provenance precedence, ambiguous sources,
semantic view ownership, fallbacks, raw-versus-bounded values, alpha and
normal modes, UV transforms, transfer response, GGX highlight shape, record
offsets, revision behavior, bundle checksums, and texture substitution. A
separate red regression now proves unsupported transfer versions cannot reach
the shader.

Final accumulated result:

- 11 focused material unit cases plus two standalone/GPU material contracts.
- 120/120 Debug CTest tests passed.
- 120/120 Release CTest tests passed.
- All five GPU-labeled contracts passed, including the legacy raster,
  textured raster, material raster, Vulkan probe, and D3D11 bridge.
- Vulkan core validation reported zero errors.

## Material and GPU evidence

The deterministic bundle is:

    artifacts/phase-09/synthetic-default-material.vfmat
    SHA-256 A21657D4CF79EB9C8AF54B9F7755D1A836380FE770A66D783F3A0C1692EFAB36

Encoding and replaying the controlled GGX sphere produce byte-identical PPMs:

    SHA-256 095894ACD90D221BD3508F2B4DB41E4FFE53848C55864E17DA70EB637E1E819C

The sweep shaded 21,408 pixels and exercised smoothness/specular ranges from
0.12549 through 0.941176.

The validation-enabled Vulkan synthetic frame reported:

```text
submission=1 differing=1128 max-error=3 mean-error=0.0953776
probes=pass tolerance=pass validation-errors=0 result=pass
```

Debug and Release are byte-identical:

    SHA-256 EBE512B469C69D44F7EED23D46F1A92B9F06288C7D46DD948B6F79F0491891CE

Applying the same material bundle to the real Phase 7 mesh reported:

```text
source-vertices=289 source-indices=1536 attributes=7
submission=1 differing=1993 max-error=4 mean-error=0.269979
validation-errors=0 result=pass
```

Debug and Release again produced identical output:

    SHA-256 9B689F19DDB98CB84BDF3A9E4710C004125A767CCB9C56C6AB043A2876420764

## Promotion decision

The authored material model, replay bundle, reflected GPU records, CPU/GPU
evaluators, Vulkan pipeline, and captured-mesh application are complete and
green. Phase 9 is not yet promoted because its exit gate calls for a default
lighting material captured from the live engine boundary. The current capture
still uses a controlled authored fixture, and the existing interactive Fallout
process must not be adopted or stopped.

The next live step is a build-gated one-shot capture at
`BSShaderProperty::SetMaterial`/the concrete lighting material boundary,
correlated with the already implemented bound texture capture. Unknown
material classes remain diagnostic, and world draw suppression remains off.
