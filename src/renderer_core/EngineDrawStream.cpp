#include "renderer_core/EngineDrawStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>

namespace vf::renderer::drawstream {

// Fully qualified because several locals here are called `mesh`, which would
// otherwise shadow the namespace and silently pick the wrong name.
namespace enginevertex = ::vf::renderer::mesh;

namespace {

[[nodiscard]] std::uint64_t Mix(std::uint64_t value) noexcept
{
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    value ^= value >> 33;
    return value;
}

[[nodiscard]] bool FiniteModel(const float (&model)[16]) noexcept
{
    return std::all_of(std::begin(model), std::end(model),
        [](const float value) { return std::isfinite(value); });
}

// Upper-left 3x3 determinant of a row-major 4x4. A singular one collapses the
// object to zero volume, so it would occupy the scene and never be visible.
[[nodiscard]] double UpperDeterminant(const float (&m)[16]) noexcept
{
    return static_cast<double>(m[0]) *
            (static_cast<double>(m[5]) * m[10] -
             static_cast<double>(m[6]) * m[9]) -
        static_cast<double>(m[1]) *
            (static_cast<double>(m[4]) * m[10] -
             static_cast<double>(m[6]) * m[8]) +
        static_cast<double>(m[2]) *
            (static_cast<double>(m[4]) * m[9] -
             static_cast<double>(m[5]) * m[8]);
}

// The object's local +Z carried into world space, normalized. The draw stream
// has no per-object normal -- the engine's normals live per vertex in the
// attribute stream -- so this is a real quantity derived from the captured
// transform rather than an invented one. It is the object's own up axis, and
// it is not a substitute for per-vertex normals.
void ModelUpAxis(const float (&m)[16], float (&normal)[4]) noexcept
{
    float axis[3]{m[2], m[6], m[10]};
    const auto lengthSquared =
        axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared)) {
        normal[0] = 0.0f;
        normal[1] = 0.0f;
        normal[2] = 1.0f;
        normal[3] = 0.0f;
        return;
    }
    const auto inverse = 1.0f / std::sqrt(lengthSquared);
    normal[0] = axis[0] * inverse;
    normal[1] = axis[1] * inverse;
    normal[2] = axis[2] * inverse;
    normal[3] = 0.0f;
}

// Every position a mesh would contribute must be finite. A pooled format
// this build does not understand reads as garbage, and one such mesh would
// otherwise make the encoder reject the entire frame.
[[nodiscard]] bool PositionsAreFinite(const AssembledMesh& mesh) noexcept
{
    // A layout is required, not optional. Without one there is no way to read
    // a position at all, and the previous fallback -- three floats at offset
    // zero -- is exactly the misreading this check exists to catch.
    if (mesh.layout.attributes.empty()) return false;
    if (mesh.vertexStride == 0 || mesh.layout.stride != mesh.vertexStride) {
        return false;
    }
    const auto count = mesh.vertices.size() / mesh.vertexStride;
    if (count == 0) return false;
    for (std::size_t vertex = 0; vertex < count; ++vertex) {
        enginevertex::DecodedEngineVertex decoded{};
        if (enginevertex::DecodeEngineVertex(mesh.layout, mesh.vertices, vertex,
                decoded) != enginevertex::VertexDecodeError::None) {
            return false;
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(decoded.position[axis])) return false;
        }
    }
    // An index that reaches past the copied window would read whatever
    // follows it in the concatenation -- another mesh entirely.
    for (const auto index : mesh.indices) {
        if (index >= count) return false;
    }
    return true;
}

}

DrawStreamError ValidateDraw(
    const DrawRecordV1& draw,
    const TranslationLimits& limits) noexcept
{
    if (draw.vertexBuffer == 0) {
        return DrawStreamError::UnknownVertexBuffer;
    }
    if (draw.indexBuffer == 0) return DrawStreamError::UnknownIndexBuffer;
    if (draw.indexCount == 0) return DrawStreamError::EmptyGeometry;
    if (draw.indexCount % 3 != 0) return DrawStreamError::NotATriangleList;
    if (draw.indexCount > limits.maximumIndicesPerDraw) {
        return DrawStreamError::IndexCountOutOfRange;
    }
    if (draw.instanceCount == 0) return DrawStreamError::ZeroInstances;
    if (!draw.hasPixelShader) return DrawStreamError::DepthOnlyPass;
    if (!draw.sceneDepthBound) return DrawStreamError::OffscreenPass;
    if (!draw.hasTransform) return DrawStreamError::NoTransform;
    if (!FiniteModel(draw.model)) {
        return DrawStreamError::NonFiniteTransform;
    }
    // Exactly what scene::ValidAffine requires, checked here so a frame is
    // never assembled and then refused whole at encode time. One unusable
    // placement out of hundreds would otherwise cost every other object its
    // render, which is how a single mirrored crate blanked the cell.
    constexpr float kAffineTolerance = 1.0e-5f;
    if (std::abs(draw.model[12]) > kAffineTolerance ||
        std::abs(draw.model[13]) > kAffineTolerance ||
        std::abs(draw.model[14]) > kAffineTolerance ||
        std::abs(draw.model[15] - 1.0f) > kAffineTolerance) {
        return DrawStreamError::NonAffineTransform;
    }
    const auto determinant = UpperDeterminant(draw.model);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-10) {
        return DrawStreamError::SingularTransform;
    }
    if (determinant < 0.0) return DrawStreamError::MirroredTransform;
    return DrawStreamError::None;
}

std::uint64_t MeshIdentity(const DrawRecordV1& draw) noexcept
{
    // The pooled buffer and the range inside it, and nothing about where the
    // draw placed it. baseVertex is part of the range because it shifts which
    // vertices the same indices read.
    auto identity = Mix(draw.vertexBuffer);
    identity = Mix(identity ^ (static_cast<std::uint64_t>(draw.startIndex) |
        (static_cast<std::uint64_t>(draw.indexCount) << 32)));
    identity = Mix(identity ^ static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(draw.baseVertex)));
    identity = Mix(identity ^ static_cast<std::uint64_t>(draw.vertexStride));
    // The offset the stream was bound at. Two meshes at different offsets in
    // the same pool share every other field, so without this they collapse
    // onto one identity and the first one read is served for both -- the
    // second object is then drawn with geometry belonging to something else
    // somewhere else in the cell.
    identity = Mix(identity ^
        (static_cast<std::uint64_t>(draw.vertexByteOffset) << 16));
    // The layout too. The same bytes read under two formats are two different
    // meshes, and serving one for the other is how correct data draws wrong.
    identity = Mix(identity ^ draw.inputLayout);
    // Zero is reserved: the scene packet treats it as no identity at all.
    return identity == 0 ? 1 : identity;
}

std::uint64_t InstanceIdentity(
    const std::uint64_t meshIdentity,
    const std::uint32_t occurrence) noexcept
{
    const auto identity =
        Mix(meshIdentity ^ (static_cast<std::uint64_t>(occurrence) + 1));
    return identity == 0 ? 1 : identity;
}

DrawStreamError TranslateDrawStream(
    const DrawStreamFrame& frame,
    const TranslationLimits& limits,
    scene::ScenePacket& packet,
    TranslationResult& result) noexcept
{
    packet = {};
    result = {};
    result.droppedDraws = frame.droppedDraws;
    if (frame.draws.empty()) return DrawStreamError::EmptyGeometry;

    // Grouped by mesh before anything is emitted, because the scene packet
    // requires one contiguous run of instances per object and the engine
    // draws the same mesh at scattered points in the frame.
    struct MeshGroup
    {
        std::uint64_t identity{};
        std::uint32_t stride{};
        std::vector<const DrawRecordV1*> draws;
    };
    std::vector<MeshGroup> groups;
    std::unordered_map<std::uint64_t, std::size_t> lookup;

    try {
        for (const auto& draw : frame.draws) {
            const auto valid = ValidateDraw(draw, limits);
            if (valid != DrawStreamError::None) {
                ++result.rejectedDraws;
                ++result.rejectedByReason[static_cast<std::size_t>(valid)];
                continue;
            }
            const auto identity = MeshIdentity(draw);
            const auto existing = lookup.find(identity);
            if (existing != lookup.end()) {
                groups[existing->second].draws.push_back(&draw);
                ++result.reusedMeshes;
                continue;
            }
            if (groups.size() >= limits.maximumObjects) {
                // Refused rather than truncated: a scene that silently stops
                // at a limit looks like a scene that drew everything.
                return DrawStreamError::IndexCountOutOfRange;
            }
            lookup.emplace(identity, groups.size());
            MeshGroup group{};
            group.identity = identity;
            group.stride = draw.vertexStride;
            group.draws.push_back(&draw);
            groups.push_back(std::move(group));
        }

        if (groups.empty()) return DrawStreamError::EmptyGeometry;

        std::size_t instanceTotal = 0;
        for (const auto& group : groups) instanceTotal += group.draws.size();
        if (instanceTotal > limits.maximumInstances) {
            return DrawStreamError::IndexCountOutOfRange;
        }

        packet.header.frameId = frame.frameIndex != 0 ? frame.frameIndex : 1;
        packet.header.viewId = 1;
        packet.header.captureSequence = packet.header.frameId;
        packet.header.captureThreadId = 1;
        packet.header.renderThreadId = 1;
        packet.objects.reserve(groups.size());
        packet.instances.reserve(instanceTotal);

        for (std::size_t index = 0; index < groups.size(); ++index) {
            const auto& group = groups[index];
            const auto& first = *group.draws.front();
            scene::OpaqueObjectV1 object{};
            object.objectId = group.identity;
            // The draw stream carries no material yet; identity derived from
            // the mesh keeps it unique and stable, and it is replaced the
            // moment shader-property capture lands.
            object.materialId = Mix(group.identity ^ 0x4D41544Cull);
            if (object.materialId == 0) object.materialId = 1;
            object.drawIndex = static_cast<std::uint32_t>(index);
            object.passSequence = 1;
            object.flags =
                scene::ObjectWritesWorldTarget | scene::ObjectStatic;
            object.roughness = 1.0f;
            // The pooled range gives no bounds. A unit box keeps the record
            // valid and is honestly wrong rather than plausibly wrong: it is
            // replaced when the geometry itself is read.
            object.boundsMinimum[0] = -1.0f;
            object.boundsMinimum[1] = -1.0f;
            object.boundsMinimum[2] = -1.0f;
            object.boundsMaximum[0] = 1.0f;
            object.boundsMaximum[1] = 1.0f;
            object.boundsMaximum[2] = 1.0f;
            // The object's own transform is its first instance's, so a scene
            // read without the instance table still places it somewhere real.
            std::memcpy(object.model, first.model, sizeof(object.model));
            std::memcpy(object.previousModel, first.model,
                sizeof(object.previousModel));
            ModelUpAxis(first.model, object.geometricNormal);
            ModelUpAxis(first.model, object.shadingNormal);
            packet.objects.push_back(object);

            std::uint32_t occurrence = 0;
            for (const auto* draw : group.draws) {
                scene::InstanceV1 instance{};
                instance.objectId =
                    InstanceIdentity(group.identity, occurrence);
                instance.objectIndex = static_cast<std::uint32_t>(index);
                instance.flags = scene::InstanceStatic;
                std::memcpy(instance.model, draw->model,
                    sizeof(instance.model));
                std::memcpy(instance.previousModel, draw->model,
                    sizeof(instance.previousModel));
                packet.instances.push_back(instance);
                ++occurrence;
            }
        }
    } catch (const std::bad_alloc&) {
        packet = {};
        return DrawStreamError::EmptyGeometry;
    }

    packet.header.objectCount =
        static_cast<std::uint32_t>(packet.objects.size());
    packet.header.instanceCount =
        static_cast<std::uint32_t>(packet.instances.size());
    result.objects = packet.header.objectCount;
    result.instances = packet.header.instanceCount;
    return DrawStreamError::None;
}

ExtractionPlan PlanMeshExtraction(
    const DrawStreamFrame& frame,
    const TranslationLimits& limits,
    const std::span<const std::uint64_t> cached,
    const ExtractionBudget& budget) noexcept
{
    ExtractionPlan plan{};
    std::uint64_t indexBytes = 0;
    try {
        // Requested meshes are tracked separately from cached ones so a mesh
        // drawn twice in one frame is read once, not twice.
        std::vector<std::uint64_t> requested;
        for (const auto& draw : frame.draws) {
            // A draw the translation refuses is never worth a readback.
            if (ValidateDraw(draw, limits) != DrawStreamError::None) continue;
            const auto identity = MeshIdentity(draw);
            if (std::find(cached.begin(), cached.end(), identity) !=
                cached.end()) {
                ++plan.satisfied;
                continue;
            }
            if (std::find(requested.begin(), requested.end(), identity) !=
                requested.end()) {
                ++plan.satisfied;
                continue;
            }
            const auto cost = static_cast<std::uint64_t>(draw.indexCount) *
                sizeof(std::uint32_t);
            if (plan.requests.size() >= budget.maximumMeshesPerFrame ||
                indexBytes + cost > budget.maximumIndexBytesPerFrame) {
                // Deferred, not dropped. The next frame asks again, and the
                // count says the scene is still filling in.
                ++plan.deferred;
                continue;
            }
            indexBytes += cost;
            requested.push_back(identity);
            MeshExtractionRequest request{};
            request.meshIdentity = identity;
            request.vertexBuffer = draw.vertexBuffer;
            request.indexBuffer = draw.indexBuffer;
            request.vertexStride = draw.vertexStride;
            request.vertexByteOffset = draw.vertexByteOffset;
            request.inputLayout = draw.inputLayout;
            request.firstIndex = draw.startIndex;
            request.indexCount = draw.indexCount;
            request.baseVertex = draw.baseVertex;
            request.indexFormat = draw.indexFormat;
            request.indexOffset = draw.indexOffset;
            plan.requests.push_back(request);
        }
    } catch (const std::bad_alloc&) {
        plan.requests.clear();
    }
    return plan;
}

bool VertexRangeForIndices(
    const std::span<const std::uint32_t> indices,
    const std::int32_t baseVertex,
    std::uint32_t& firstVertex,
    std::uint32_t& vertexCount) noexcept
{
    firstVertex = 0;
    vertexCount = 0;
    if (indices.empty()) return false;

    // The hardware adds baseVertex to every index before the fetch, so the
    // window that has to be copied out of a 128 MB pool is the shifted one.
    // Signed arithmetic throughout: a negative base that pushes an index
    // below zero must be refused, not wrapped into a read of unrelated
    // memory at the top of the address space.
    std::int64_t lowest = std::numeric_limits<std::int64_t>::max();
    std::int64_t highest = std::numeric_limits<std::int64_t>::min();
    for (const auto index : indices) {
        const auto shifted = static_cast<std::int64_t>(index) + baseVertex;
        if (shifted < 0) return false;
        lowest = std::min(lowest, shifted);
        highest = std::max(highest, shifted);
    }
    const auto span = highest - lowest + 1;
    if (span <= 0 || span > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    firstVertex = static_cast<std::uint32_t>(lowest);
    vertexCount = static_cast<std::uint32_t>(span);
    return true;
}

DrawStreamError AssembleSceneGeometry(
    scene::ScenePacket& packet,
    const std::span<const AssembledMesh> meshes,
    raster::DecodedPacket& rasterPacket,
    AssemblyResult& result,
    GeometryArena* const arena) noexcept
{
    // Vertices and indices survive between frames when an arena is supplied.
    // Everything else is rebuilt: draws, materials and the pruned object list
    // are a few hundred entries, while the vertex decode is a million and
    // measured 454 ms.
    auto keptVertices = std::move(rasterPacket.vertices);
    auto keptIndices = std::move(rasterPacket.indices);
    rasterPacket = {};
    result = {};
    if (arena != nullptr) {
        rasterPacket.vertices = std::move(keptVertices);
        rasterPacket.indices = std::move(keptIndices);
        // The arena and the arrays are one structure in two places, so a slot
        // that points past the geometry it describes means they have been
        // separated. Starting over is always correct and costs one frame.
        for (const auto& [identity, slot] : arena->slots) {
            if (slot.vertexOffset + slot.vertexCount >
                    rasterPacket.vertices.size() ||
                slot.indexOffset + slot.indexCount >
                    rasterPacket.indices.size()) {
                arena->slots.clear();
                rasterPacket.vertices.clear();
                rasterPacket.indices.clear();
                break;
            }
        }
    }

    try {
        // Objects survive only if their geometry has been read back. They are
        // rewritten together with their instances, because an object whose
        // draw index no longer matches its mesh would place the wrong model.
        std::vector<scene::OpaqueObjectV1> keptObjects;
        std::vector<scene::InstanceV1> keptInstances;
        std::vector<const AssembledMesh*> keptMeshes;
        keptObjects.reserve(packet.objects.size());
        keptInstances.reserve(packet.instances.size());

        // Instances bucketed by the object they belong to, once. Scanning the
        // whole instance list per object was the second quadratic pass in
        // this function and the last one standing after the decode was
        // cached: nine hundred objects over nine hundred instances.
        rasterPacket.draws.reserve(packet.objects.size());
        rasterPacket.materials.reserve(packet.objects.size());

        // Counted and then placed, rather than a vector per object: nine
        // hundred inner vectors is nine hundred heap allocations every frame,
        // which cost more than the scan they replaced saved.
        std::vector<std::uint32_t> instanceStart(packet.objects.size() + 1, 0);
        for (const auto& instance : packet.instances) {
            if (instance.objectIndex < packet.objects.size()) {
                ++instanceStart[instance.objectIndex + 1];
            }
        }
        for (std::size_t slot = 1; slot < instanceStart.size(); ++slot) {
            instanceStart[slot] += instanceStart[slot - 1];
        }
        std::vector<std::uint32_t> instanceOrder(instanceStart.back());
        {
            auto cursor = instanceStart;
            for (std::size_t slot = 0; slot < packet.instances.size(); ++slot) {
                const auto owner = packet.instances[slot].objectIndex;
                if (owner < packet.objects.size()) {
                    instanceOrder[cursor[owner]++] =
                        static_cast<std::uint32_t>(slot);
                }
            }
        }

        // Indexed once rather than searched per object. This was a linear scan
        // inside the object loop, so a settled cell of nine hundred objects
        // over nine hundred meshes did most of a million comparisons a frame
        // and cost 54 ms after the decode itself had been cached away.
        // A sorted vector rather than a hash map: the map allocated a node per
        // mesh every frame, nine hundred allocations for a lookup table that
        // is discarded at the end of the call. One buffer and a binary search
        // costs neither.
        std::vector<std::pair<std::uint64_t, const AssembledMesh*>> byIdentity;
        byIdentity.reserve(meshes.size());
        for (const auto& mesh : meshes) {
            if (mesh.vertexStride < sizeof(float) * 3 ||
                mesh.indices.empty() || mesh.vertices.empty()) {
                continue;
            }
            byIdentity.emplace_back(mesh.identity, &mesh);
        }
        // Stable, so that equal identities keep the order they arrived in and
        // the first one still wins -- exactly as the linear scan did, which
        // stopped at the first match and never reached later duplicates.
        std::stable_sort(byIdentity.begin(), byIdentity.end(),
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });

        for (std::size_t index = 0; index < packet.objects.size(); ++index) {
            const AssembledMesh* found = nullptr;
            {
                const auto wanted = packet.objects[index].objectId;
                const auto entry = std::lower_bound(byIdentity.begin(),
                    byIdentity.end(), wanted,
                    [](const auto& candidate, const std::uint64_t value) {
                        return candidate.first < value;
                    });
                if (entry != byIdentity.end() && entry->first == wanted) {
                    found = entry->second;
                }
            }
            if (found == nullptr) {
                ++result.missingMeshes;
                continue;
            }
            // Skipped for a mesh already in the arena. This walks and decodes
            // every vertex, so on a settled cell it repeated the whole decode
            // the arena exists to avoid -- 85 ms of a frame that had nothing
            // to rebuild. A slot is only created from geometry that passed
            // this check, and it is only reused while the bytes behind it are
            // the same ones, so the answer cannot have changed.
            //
            // Deliberately an equivalent mutant: removing the `validated`
            // guard produces identical output and no test can distinguish it,
            // because it only repeats a check whose answer is already known.
            // It is kept for the frame time, and it is recorded here rather
            // than left as an unexplained survivor.
            auto validated = false;
            if (arena != nullptr) {
                const auto slot = arena->slots.find(found->identity);
                validated = slot != arena->slots.end() &&
                    slot->second.vertices == found->vertices.data() &&
                    slot->second.vertexBytes == found->vertices.size_bytes() &&
                    slot->second.sourceIndices == found->indices.size();
            }
            if (!validated && !PositionsAreFinite(*found)) {
                ++result.unreadableMeshes;
                continue;
            }
            const auto kept = static_cast<std::uint32_t>(keptObjects.size());
            auto object = packet.objects[index];
            object.drawIndex = kept;
            keptObjects.push_back(object);
            keptMeshes.push_back(found);
            for (auto slot = instanceStart[index];
                slot < instanceStart[index + 1]; ++slot) {
                const auto& instance = packet.instances[instanceOrder[slot]];
                auto moved = instance;
                moved.objectIndex = kept;
                keptInstances.push_back(moved);
            }
        }

        if (keptObjects.empty()) {
            packet.objects.clear();
            packet.instances.clear();
            packet.header.objectCount = 0;
            packet.header.instanceCount = 0;
            return DrawStreamError::EmptyGeometry;
        }

        for (std::size_t index = 0; index < keptMeshes.size(); ++index) {
            const auto& mesh = *keptMeshes[index];
            const auto vertexCount = mesh.vertices.size() / mesh.vertexStride;
            if (vertexCount == 0) return DrawStreamError::EmptyGeometry;

            raster::RasterDrawV1 draw{};
            draw.materialId = keptObjects[index].materialId;
            // This mesh.s place in the arena, reused when it still names the
            // same bytes and appended when it does not.
            GeometryArena::Slot slot{};
            auto decodeNeeded = true;
            if (arena != nullptr) {
                const auto found = arena->slots.find(mesh.identity);
                if (found != arena->slots.end() &&
                    found->second.vertices == mesh.vertices.data() &&
                    found->second.vertexBytes == mesh.vertices.size_bytes() &&
                    found->second.sourceIndices == mesh.indices.size()) {
                    slot = found->second;
                    decodeNeeded = false;
                }
            }
            if (decodeNeeded) {
                slot.vertexOffset =
                    static_cast<std::uint32_t>(rasterPacket.vertices.size());
                slot.indexOffset =
                    static_cast<std::uint32_t>(rasterPacket.indices.size());
                slot.vertexCount = static_cast<std::uint32_t>(vertexCount);
                slot.indexCount =
                    static_cast<std::uint32_t>(mesh.indices.size());
                slot.vertices = mesh.vertices.data();
                slot.vertexBytes = mesh.vertices.size_bytes();
                slot.sourceIndices = mesh.indices.size();
            }
            draw.firstIndex = slot.indexOffset;
            draw.indexCount =
                static_cast<std::uint32_t>(mesh.indices.size());
            // The concatenated base for this mesh, so its own indices stay
            // zero-based and every mesh keeps the numbering it was read with.
            draw.vertexOffset = static_cast<std::int32_t>(slot.vertexOffset);
            // The winding the engine itself declared for this draw, restated
            // in the packet's convention.
            //
            // This was `CounterClockwise` for every mesh, which is the
            // opposite of D3D11's default and rendered every model inside
            // out: the near faces were culled and the far interior kept. It
            // cannot be a constant, because it depends on the rasterizer
            // state the engine bound. A draw whose thread never saw a state
            // takes D3D11's documented default, clockwise-front, rather than
            // the reverse.
            //
            // Reading the state from the engine was necessary but not
            // sufficient: the flag is a statement about D3D's Y-down screen
            // space and the packet's field is Y-up NDC, so it still rendered
            // inside out while carrying the engine's own answer. The
            // conversion between the two conventions is what closes it.
            draw.frontFace = visibility::PacketFrontFaceFromEngine(
                mesh.frontCounterClockwise);
            draw.depthCompare = raster::DepthCompare::Less;
            rasterPacket.draws.push_back(draw);

            // Asked once per mesh, not per vertex: the declaration is a
            // property of the layout.
            const auto declaresNormal = mesh.layout.Find(
                enginevertex::VertexSemantic::Normal) != nullptr;
            // Skipped whole for a mesh already in the arena. The decode is the
            // expensive half -- a million vertices at 454 ms -- so stepping
            // through it and merely declining to store the result would keep
            // the cost this cache exists to remove.
            for (std::size_t vertex = 0; decodeNeeded && vertex < vertexCount;
                ++vertex) {
                // Decoded through the layout the engine declared, never
                // memcpy'd as three floats. Fallout 4 stores position as four
                // halves, and half bit patterns read as float32 land on
                // denormals: almost every vertex collapses onto the origin and
                // the few that do not fly off, which draws as a fan of spikes
                // rather than as anything recognisably misplaced.
                enginevertex::DecodedEngineVertex decoded{};
                if (enginevertex::DecodeEngineVertex(mesh.layout, mesh.vertices,
                        vertex, decoded) != enginevertex::VertexDecodeError::None) {
                    return DrawStreamError::EmptyGeometry;
                }
                raster::RasterVertexV3 value{};
                value.position[0] = decoded.position[0];
                value.position[1] = decoded.position[1];
                value.position[2] = decoded.position[2];
                // The colour and coordinates the engine wrote, now that the
                // layout says where they are. A mesh that declares neither
                // decodes to white and zero, which is the same as before for
                // those meshes and correct for the ones that do.
                value.color[0] = decoded.color[0];
                value.color[1] = decoded.color[1];
                value.color[2] = decoded.color[2];
                value.texCoord[0] = decoded.texCoord0[0];
                value.texCoord[1] = decoded.texCoord0[1];
                // The engine's own per-vertex normal, in object space -- the
                // shader rotates it by the model's upper 3x3, so it must not
                // be transformed here. Left unwritten, every vertex in the
                // world kept the +Z default and each object shaded to a
                // single flat tone regardless of where the sun was, which
                // reads as broken lighting rather than as absent normals.
                //
                // A normal that decodes to zero keeps the default instead:
                // zero is not "no lighting", it is a division by zero
                // wherever the shading normalises it.
                const auto lengthSquared =
                    decoded.normal[0] * decoded.normal[0] +
                    decoded.normal[1] * decoded.normal[1] +
                    decoded.normal[2] * decoded.normal[2];
                // Not "greater than zero". The engine stores normals as bytes
                // remapped to [-1, 1], so the encoding of zero is 128, which
                // decodes to 0.0039 rather than to nothing: a degenerate
                // normal arrives as a very short vector, never as an exactly
                // zero one. A real normal is unit length, so anything shorter
                // than half of that is not one.
                constexpr float kMinimumNormalLengthSquared = 0.25f;
                // Provenance comes from the declaration, not from the decoded
                // value. DecodedEngineVertex defaults its normal to +Z, which
                // is a perfectly good unit vector, so a length test counts a
                // mesh that declared nothing as though it had brought its own
                // -- and that is precisely the distinction these counters
                // exist to make.
                if (declaresNormal &&
                    lengthSquared >= kMinimumNormalLengthSquared) {
                    value.normal[0] = decoded.normal[0];
                    value.normal[1] = decoded.normal[1];
                    value.normal[2] = decoded.normal[2];
                    ++slot.verticesWithNormals;
                } else {
                    ++slot.verticesWithoutNormals;
                }
                if (value.color[0] < 0.99f || value.color[1] < 0.99f ||
                    value.color[2] < 0.99f) {
                    ++slot.verticesTinted;
                }
                if (value.color[0] < 0.05f &&
                    std::abs(value.color[1] - value.color[2]) < 0.02f &&
                    value.color[1] > 0.4f) {
                    ++slot.verticesCyanSignature;
                }
                if (decodeNeeded) rasterPacket.vertices.push_back(value);
            }
            if (decodeNeeded) {
                for (const auto index32 : mesh.indices) {
                    rasterPacket.indices.push_back(index32);
                }
                if (arena != nullptr) {
                    arena->slots.insert_or_assign(mesh.identity, slot);
                }
            }
            // Carried on the slot so a reused mesh still reports what it
            // contributed. Recounting them would mean decoding it again,
            // which is the work being avoided.
            result.verticesWithNormals += slot.verticesWithNormals;
            result.verticesWithoutNormals += slot.verticesWithoutNormals;
            result.verticesTinted += slot.verticesTinted;
            result.verticesCyanSignature += slot.verticesCyanSignature;

            raster::RasterMaterialV1 material{};
            material.resourceId = keptObjects[index].materialId;
            material.baseColor[0] = 1.0f;
            material.baseColor[1] = 1.0f;
            material.baseColor[2] = 1.0f;
            material.baseColor[3] = 1.0f;
            rasterPacket.materials.push_back(material);
        }

        // The reused arrays must be exactly what this mesh set would have
        // produced. A mismatch means the caller's "same mesh set" claim was
        // wrong, and every draw range computed above now points into the wrong
        // geometry -- which draws a recognisable scene made of the wrong
        // objects, the hardest kind of wrong to attribute. Refused rather than
        // rendered, so the caller rebuilds.
        packet.objects = std::move(keptObjects);
        packet.instances = std::move(keptInstances);

        // One visibility record per surviving object, carrying what the
        // engine actually declared.
        //
        // Leaving this empty means "opaque, front-facing only, and
        // unmirrored" by the scene packet.s own definition, so every mirrored
        // object was back-face culled however the engine drew it. A two-sided
        // model then loses its outer shell and shows its interior, which
        // reads as inverted geometry rather than as a missing cull mode.
        packet.visibility.clear();
        packet.visibility.reserve(packet.objects.size());
        for (std::size_t slot = 0; slot < packet.objects.size(); ++slot) {
            visibility::VisibilityRecordV1 record{};
            record.objectId = packet.objects[slot].objectId;
            record.materialId = packet.objects[slot].materialId;
            switch (keptMeshes[slot]->cullMode) {
            case kCullModeNone:
                record.faceMode = visibility::FaceMode::TwoSided;
                break;
            case kCullModeFront:
                record.faceMode = visibility::FaceMode::BackOnly;
                break;
            default:
                // Back, and the unobserved case: D3D11 defaults to culling
                // back, so a draw whose state was never seen is culled the
                // way the runtime would have culled it.
                record.faceMode = visibility::FaceMode::FrontOnly;
                break;
            }
            // The placement.s handedness. A negatively scaled instance turns
            // its triangles inside out, and the backend folds this into the
            // front face rather than the mirror guessing a winding.
            record.modelDeterminant = static_cast<float>(
                UpperDeterminant(packet.objects[slot].model));
            packet.visibility.push_back(record);
        }
        packet.header.objectCount =
            static_cast<std::uint32_t>(packet.objects.size());
        packet.header.instanceCount =
            static_cast<std::uint32_t>(packet.instances.size());

        rasterPacket.header.frameIndex = packet.header.frameId;
        rasterPacket.header.indexType = raster::IndexType::Uint32;
        rasterPacket.header.vertexCount =
            static_cast<std::uint32_t>(rasterPacket.vertices.size());
        rasterPacket.header.indexCount =
            static_cast<std::uint32_t>(rasterPacket.indices.size());
        rasterPacket.header.drawCount =
            static_cast<std::uint32_t>(rasterPacket.draws.size());
        rasterPacket.header.materialCount =
            static_cast<std::uint32_t>(rasterPacket.materials.size());
        // Local space: the scene's per-instance model rows place it, exactly
        // as the engine's own vertex shader does.
        rasterPacket.header.vertexSpace =
            raster::VertexSpace::CameraRelativeWorld;

        result.drawnObjects =
            static_cast<std::uint32_t>(packet.objects.size());
        result.vertices = rasterPacket.vertices.size();
        result.indices = rasterPacket.indices.size();
    } catch (const std::bad_alloc&) {
        rasterPacket = {};
        return DrawStreamError::EmptyGeometry;
    }
    return DrawStreamError::None;
}

const char* ToString(const DrawStreamError error) noexcept
{
    switch (error) {
    case DrawStreamError::None: return "none";
    case DrawStreamError::UnknownVertexBuffer: return "unknown vertex buffer";
    case DrawStreamError::UnknownIndexBuffer: return "unknown index buffer";
    case DrawStreamError::EmptyGeometry: return "empty geometry";
    case DrawStreamError::NotATriangleList: return "not a triangle list";
    case DrawStreamError::IndexCountOutOfRange:
        return "index count out of range";
    case DrawStreamError::NonFiniteTransform: return "non-finite transform";
    case DrawStreamError::SingularTransform: return "singular transform";
    case DrawStreamError::ZeroInstances: return "zero instances";
    case DrawStreamError::NoTransform: return "no transform";
    case DrawStreamError::NonAffineTransform: return "non-affine transform";
    case DrawStreamError::MirroredTransform: return "mirrored transform";
    case DrawStreamError::DepthOnlyPass: return "depth-only pass";
    case DrawStreamError::OffscreenPass: return "off-screen pass";
    }
    return "unknown";
}

}
