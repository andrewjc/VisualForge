# Phase 15 — Alpha-tested and two-sided visibility

Status: offline implementation complete; live capture gate pending

## Implemented slice

- `EngineVisibility` (`src/renderer_core/EngineVisibility.h/.cpp`, namespace
  `vf::renderer::visibility`) owns the visibility boundary: a 32-byte
  `AlphaStateV1` and a 64-byte `VisibilityRecordV1`.
- `ClassifyAlphaState` reads a captured `AlphaPropertyCapture` and decides:
  blend enabled → `Blended`, test-only → `Tested`, neither → `Opaque` with the
  source forced to `None`. The reference is `testReference / 255` exactly,
  never a rounded 0.5. Alpha-to-coverage without a test, and a test with no
  alpha source, both return `UnclassifiedAlpha` rather than being guessed at.
- `EvaluateCoverage` ignores `CoverageContext::depthOnly` by construction,
  which is what keeps the depth prepass and the colour pass on one silhouette.
  The comparison is `>=`, the engine's convention, so the boundary sample
  survives. Constant alpha scales the sample before the test.
- `DitherThreshold` is a 4×4 Bayer matrix holding each of sixteen levels once,
  so fade coverage over a tile is exactly `round(fade * 16)` and rises
  monotonically.
- `ComputeAlphaCoverageScales` bisects a per-mip scale — coverage is monotonic
  in scale — so every mip's coverage matches mip 0's, which is what stops
  cutouts dissolving with distance. Ties prefer the scale that does not lose
  coverage.
- `ResolveShadingFrame`: only a two-sided surface flips on a back face; a
  shading normal below the geometric horizon is lifted onto it
  (`liftedToHorizon`) rather than lighting from behind; a negative determinant
  flips the bitangent to preserve tangent-basis handedness (`mirrored`).
  `EffectiveFrontFace` reverses the declared winding for a mirrored instance.
- `ValidateOpaqueRasterClass` refuses `Blended` with `BlendedNotSupported`.
  Sorted transparency is classified, not silently rendered as a cutout.
- Scene packet minor 1.2 appends a per-object visibility section.
  `ScenePacketHeaderV1` kept its exact 96 bytes by carving `visibilityCount`
  (offset 80) and `visibilityOffset` (84) out of the former `reserved[2]`.
  `EncodeScenePacket` emits the lowest version that can represent the scene,
  so a scene with no visibility records still encodes as 1.0 or 1.1 and every
  earlier artifact stays byte-identical.
- `ValidateSceneVisibility` requires exactly one record per object,
  positionally matching each object's ids. A partial table is
  `UncoveredObject`, a reordered one is `InvalidVisibility`, an unclassified
  record is `UnclassifiedWorldWriter`, and a singular model transform is
  `InvalidVisibility`. `ResolveVisibility` gives 1.0 and 1.1 scenes an
  implicit opaque, front-only, unmirrored record, so every consumer — CPU
  reference, backend, and shader alike — resolves visibility through one rule.
- Binding 13 carries `GpuVisibilityRecordV1`, reflected from
  `scene_layout.comp`, so its 64-byte size and its binding number are
  build-time assertions rather than a hand-maintained copy.

### The stored-opacity rule

The G-buffer's opacity channel is **the coverage decision's own opacity**, not
the sampled texture alpha. `CoverageResult::coverage` already computed it and
the reference was discarding it; both sides now store it:

- An **opaque** surface stores opacity 1 whatever its base texture holds in
  that channel. Fallout 4 texture sets routinely carry a mask or a height in
  the alpha of a diffuse map for surfaces that are not alpha-tested; folding
  that into a deferred G-buffer silently makes a solid surface transparent.
- A **cutout** survivor stores opacity 1. The test that admitted it was
  binary, so a survivor claiming to be semi-transparent would contradict the
  decision that let it through.
- A **blended** surface is the only one that stores a partial opacity, and it
  stores the effective alpha (sample × constant), not the raw sample.

The rule lives in exactly two places that are checked against each other:
`visibility::EvaluateCoverage` and `shaders/phase15/alpha_coverage.glsl`'s
`vfEvaluateCoverage`, which mirrors it branch for branch. Both the opaque
`scene.frag` and the two alpha shaders source their opacity from it, so the
opaque path can no longer disagree with the alpha path about what a surface's
opacity means. `scene.frag` deliberately does **not** discard — the opaque
raster class is validated before a frame is armed, so its coverage is
unconditionally one and `early_fragment_tests` stays sound.

Alpha-to-coverage quantizes to `round(alpha * samples) / samples`; at one
sample per pixel it collapses to the identical binary result as a plain alpha
test, which is why the GLSL mirror needs no separate branch for it.

### Backend

- `RecordAlphaDraws` is shared by the prepass and the colour pass, so the two
  cannot diverge in geometry, dynamic state, or push constants — only the
  bound pipeline differs. "The silhouettes match" is therefore a structural
  property, not something to be tested for.
- Pass order: depth-only pass (`loadOp` CLEAR, alpha-tested objects only),
  then the main pass with depth switched to `loadOp` **LOAD**. When no object
  is alpha tested the prepass is skipped and depth clears as before, which is
  what keeps Phases 11–14 inert under this change.
- The colour pass tests `VK_COMPARE_OP_EQUAL` with depth writes off. A
  silhouette disagreement between the passes erases the colour fragment and
  surfaces in the G-buffer comparison instead of differing quietly.
- The ordering is correct in both directions: a cutout in front of an opaque
  surface has already written the nearer depth, so the opaque draw fails LESS
  and is hidden; a cutout behind one has its prepass depth overwritten by the
  opaque draw, so its colour fragment fails EQUAL and is hidden.
- `alpha_scene.frag` and `alpha_depth.frag` deliberately omit
  `early_fragment_tests`. A discarded cutout fragment must not have already
  written depth, or the silhouette punches a hole in whatever is behind it.
- Cull mode, front face, and depth compare are dynamic state (core in Vulkan
  1.3) driven by the resolved visibility record, so one pipeline covers every
  per-draw permutation: two-sided sets `VK_CULL_MODE_NONE`, and a negative
  determinant reverses the declared winding through `EffectiveFrontFace`.

## TDD/RGR evidence

Every red below was observed and recorded before the fix.

1. **Coverage test measured an object that has no coverage.** The first
   cutout test asserted against Phase 11's object 1, which the Phase 11
   fixture deliberately occludes — it has zero coverage in *any* render, so
   the assertion passed for the wrong reason and would have passed against a
   shader that discarded everything. Retargeted at object 0.
2. **Five Vulkan validation errors in the alpha replay.** The depth prepass
   issued draws before `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`,
   because those bindings sat inside the main pass. Buffer bindings are
   command-buffer state, not render-pass state; hoisting them above the
   prepass cleared all five.
3. **`interior-mismatches=4198`, `gbuffer-max-error=1`.** With coverage and
   identity already exact, 4,198 of 46,334 interior pixels still disagreed.
   Per-channel diagnostics — added to the replay because a bare mismatch
   count cannot name a cause — isolated it to a single channel:

   ```text
   mismatch at 99,22 object=5836665117072167170
     albedo expected 0.5 0 0.5 1  actual 0.5 0 0.5 0
     geometricNormalRoughness expected -0 0 -1 0.45  actual -0 0 -1 0.45
     shadingNormalDepth expected -0 0.14834 -0.988936 0.975976  actual same
   ```

   The opaque objects sampled the cutout texture and folded its clear-block
   alpha into their stored opacity, while the reference — correctly — did
   not. This is the defect the stored-opacity rule above fixes, and it is a
   real engine-facing bug rather than a fixture artifact: any opaque surface
   whose diffuse map carries non-opacity data in its alpha channel was
   affected. Pinned by
   `P15_stored_opacity_comes_from_coverage_not_from_sampled_alpha`, which
   renders an opaque, a cutout, and a blended classification against a
   uniformly clear-alpha texture and asserts 1, 1, and 0 respectively.

The same push also corrected a latent asymmetry the fixture nearly hid: the
GPU has always modulated albedo RGB by the base texture and the CPU reference
never did. Phases 11–14 got away with it because their reference renders pass
no texture at all (opaque white). The reference now applies it, which is what
makes the four-argument overload a true generalization of the three-argument
one rather than a second, differently-behaved path.

## Replay and artifact evidence

`vf_packet_replay --render-alpha-scene`, validation enabled, Debug and Release
both report:

```text
alpha-replay extent=256x192 cutout-pixels=4422 expected-cutout-pixels=4422
gbuffer-identity-mismatches=0 gbuffer-max-error=1.21593e-05
gbuffer-mean-error=1.05739e-07
interior=46334 interior-mismatches=0
validation-errors=0 result=pass
```

Interpretation of the numbers:

- GPU and oracle cutout coverage agree **exactly** at 4,422 pixels. Coverage
  is compared as a count and per pixel, so a silhouette that was merely
  similar cannot pass. The phase gate permits a declared pixel threshold here;
  none is needed, and none is claimed.
- Zero identity mismatches: every covered pixel carries the right object id
  and the right material id, including across the cutout's holes where the
  surface behind is revealed.
- 46,334 interior pixels — every pixel whose 3×3 neighbourhood belongs to one
  surface — agree within 1e-3, with a worst channel error of 1.2e-05 and a
  mean of 1.1e-07. That is float rounding between the oracle's barycentric
  interpolation and hardware rasterization, not a rule disagreement. Silhouette
  pixels are excluded because the oracle's coverage rule and hardware
  rasterization legitimately differ there.
- Zero validation errors with the depth prepass, the EQUAL colour pass, and
  per-draw dynamic cull/front-face/depth-compare all active.

Prior-phase regression, checked by SHA-256 rather than by assumption — the
stored-opacity change and the `scene.frag` change are behaviour-preserving
because every earlier fixture's material and instance alphas are 1.0 and every
earlier object classifies opaque:

| Artifact | SHA-256 | Matches archived |
| --- | --- | --- |
| `phase11-scene.vfgbuf` | `430372FF…1AE3CC7C` | yes |
| `phase12-instanced.vfgbuf` | `A68D118E…3724B307` | yes |
| `phase13-deformed.vfgbuf` | `CCF50E28…D006FB25` | yes |
| `phase14-terrain.vfgbuf` | `BDCBBEA7…AB38DF89` | yes |

Artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `artifacts/phase-15/alpha-debug.ppm` | 147,471 | `0760B53F5C67E3ED9CCD85B599D3BB451B1157258FC1BBFCCBFEFB2B20232676` |
| `artifacts/phase-15/alpha-release.ppm` | 147,471 | `0760B53F5C67E3ED9CCD85B599D3BB451B1157258FC1BBFCCBFEFB2B20232676` |
| `artifacts/phase-15/alpha-debug.vfgbuf` | 3,145,728 | `7614FA37C6F4E3766E917D4AF4B034E42491BFB1AA5DBC57A7933457D4C67F45` |
| `artifacts/phase-15/alpha-release.vfgbuf` | 3,145,728 | `7614FA37C6F4E3766E917D4AF4B034E42491BFB1AA5DBC57A7933457D4C67F45` |
| `artifacts/phase-15/alpha-debug.vfscene` | 960 | `20E8D643C354DA3318E35AF60CE83A8E13587269AAA3655AD8CBFE9450712970` |
| `artifacts/phase-15/alpha-release.vfscene` | 960 | `20E8D643C354DA3318E35AF60CE83A8E13587269AAA3655AD8CBFE9450712970` |
| `artifacts/phase-15/alpha-debug.png` | 9,401 | rendered from the Debug PPM |

Debug and Release produce byte-identical scene packets, colour output, and
G-buffer readbacks. The PNG was visually inspected: the left triangle is the
alpha-tested object and shows the 2×2 block checker punched through it, with
the opaque triangle behind visible through the cleared blocks and the
background visible where nothing is behind. The holes have hard edges, which
is what a binary test should produce; a blend would have shown a gradient.

### Fixture note

The cutout is a 4×4 `R8G8B8A8Unorm` texture with opaque white RGB and a 2×2
block checker in alpha, sampled nearest with `maxLod = 0`. The block size is
deliberate: it is coarse enough that whole 3×3 pixel neighbourhoods fall
inside one cell of the pattern, so the interior comparison is not dominated by
pattern edges. RGB is uniform so the comparison isolates the alpha rule; a
spatially varying RGB would fold hardware/oracle filtering differences into
the same number and make a failure ambiguous. Object 0 is alpha-tested at
reference 0.5 against the base-colour texture, object 2 is two-sided, and
object 1 is the Phase 11 fixture's occluded object and is left opaque.

Representative commands:

```powershell
cmake --build --preset vs2022-x64-debug
out/build/vs2022-x64-debug/tests/Debug/vf_unit_tests.exe "[phase15]"
ctest --test-dir out/build/vs2022-x64-debug -C Debug `
  -R "^contract\.alpha_scene_frame$" --output-on-failure
ctest --preset vs2022-x64-debug --output-on-failure

cmake --build --preset vs2022-x64-release
ctest --preset vs2022-x64-release --output-on-failure
```

## Deferred within this phase

Stated gaps, not silent ones:

- **The mip coverage-scale ramp is unit-tested only.** The replay fixture
  carries a single mip level, so `ComputeAlphaCoverageScales` is exercised by
  the unit suite and not by the GPU comparison. A distance-ramped replay that
  shows a cutout keeping its coverage across mips is the natural form and is
  not implemented here.
- **Blended surfaces are classified, not rendered.** `AlphaClass::Blended` is
  captured, validated, and refused from the opaque raster class. Sorted
  transparency is a later phase; nothing here renders it.
- **Alpha-to-coverage is single-sample only.** The quantization exists and is
  unit-tested at higher sample counts, but the mirror renders one sample per
  pixel, so the multi-sample path has no GPU evidence.

## Promotion decision

The visibility record, alpha classification, coverage contract, dither fade,
mip coverage scales, shading-frame resolution, mirrored winding, scene packet
minor 1.2, GPU binding 13, depth prepass, EQUAL colour pass, per-draw dynamic
state, the stored-opacity rule, and the Debug/Release regression are offline
complete. 206/206 tests pass in both configurations.

Phase 15 is **not live-promoted**. Its exit gate additionally requires
captured engine alpha properties — real `NiAlphaProperty` state, real test
references, and real two-sided flags read from a running Fallout 4 — compared
against this mirror. That capture is blocked behind the same unresolved item
as Phases 8–14: the world camera has not been located, so no live frame can be
mirrored yet. See `journal.md` for the camera provenance conclusion.
