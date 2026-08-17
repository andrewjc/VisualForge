#include "renderer_core/ShaderReflection.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace vf::renderer::shader;

namespace {

void AppendU32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    std::byte encoded[sizeof(value)]{};
    std::memcpy(encoded, &value, sizeof(value));
    bytes.insert(bytes.end(), std::begin(encoded), std::end(encoded));
}

void WriteU32(
    std::vector<std::byte>& bytes, const std::size_t offset, const std::uint32_t value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

[[nodiscard]] std::uint32_t ReadU32(
    const std::vector<std::byte>& bytes, const std::size_t offset)
{
    std::uint32_t value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

struct VariableSpec
{
    std::string name;
    std::uint32_t offset{};
    std::uint32_t size{};
};

struct BufferSpec
{
    std::string name;
    std::uint32_t size{};
    std::vector<VariableSpec> variables;
};

// D3D_SHADER_INPUT_TYPE. Only the two this parser cares about are named; the
// fixture uses the raw values for everything else, matching what a real
// compiler emits for constant buffers (0) and UAVs.
constexpr std::uint32_t kSitTexture = 2;
constexpr std::uint32_t kSitSampler = 3;

struct ResourceSpec
{
    std::string name;
    std::uint32_t type{};
    std::uint32_t bindPoint{};
    std::uint32_t bindCount{1};
};

// Builds the reflection chunk body the way the compiler lays it out: a fixed
// header, the bound-resource table, the buffer table, then each buffer's
// variable table, then every name in one string pool at the end.
[[nodiscard]] std::vector<std::byte> BuildReflectionChunk(
    const std::vector<BufferSpec>& buffers,
    const std::uint8_t majorVersion,
    const std::vector<ResourceSpec>& resources = {})
{
    const std::size_t variableStride = majorVersion >= 5 ? 40u : 24u;
    constexpr std::size_t kResourceEntrySize = 32;
    const std::size_t headerSize = 28;
    const std::size_t resourceTable = headerSize;
    const std::size_t resourceTableBytes = resources.size() * kResourceEntrySize;
    const std::size_t bufferTable = resourceTable + resourceTableBytes;
    const std::size_t bufferTableBytes = buffers.size() * 24;

    std::size_t variableTable = bufferTable + bufferTableBytes;
    std::size_t variableBytes = 0;
    for (const auto& buffer : buffers) {
        variableBytes += buffer.variables.size() * variableStride;
    }

    const std::size_t stringPool = variableTable + variableBytes;

    std::vector<std::byte> names;
    const auto intern = [&names, stringPool](const std::string& text) {
        const auto offset = static_cast<std::uint32_t>(stringPool + names.size());
        for (const char character : text) {
            names.push_back(static_cast<std::byte>(character));
        }
        names.push_back(std::byte{0});
        return offset;
    };

    std::vector<std::byte> resourceEntries;
    for (const auto& resource : resources) {
        AppendU32(resourceEntries, intern(resource.name));
        AppendU32(resourceEntries, resource.type);
        AppendU32(resourceEntries, 0);  // return type
        AppendU32(resourceEntries, 4);  // dimension: TEXTURE2D
        AppendU32(resourceEntries, 0);  // sample count
        AppendU32(resourceEntries, resource.bindPoint);
        AppendU32(resourceEntries, resource.bindCount);
        AppendU32(resourceEntries, 0);  // flags
    }

    std::vector<std::byte> table;
    std::vector<std::byte> variables;
    for (const auto& buffer : buffers) {
        AppendU32(table, intern(buffer.name));
        AppendU32(table, static_cast<std::uint32_t>(buffer.variables.size()));
        AppendU32(table, static_cast<std::uint32_t>(variableTable + variables.size()));
        AppendU32(table, buffer.size);
        AppendU32(table, 0);  // flags
        AppendU32(table, 0);  // type

        for (const auto& variable : buffer.variables) {
            const std::size_t start = variables.size();
            AppendU32(variables, intern(variable.name));
            AppendU32(variables, variable.offset);
            AppendU32(variables, variable.size);
            AppendU32(variables, 0);  // flags
            AppendU32(variables, 0);  // type offset
            AppendU32(variables, 0);  // default value offset
            variables.resize(start + variableStride, std::byte{0});
        }
    }

    std::vector<std::byte> chunk;
    AppendU32(chunk, static_cast<std::uint32_t>(buffers.size()));
    AppendU32(chunk, static_cast<std::uint32_t>(bufferTable));
    AppendU32(chunk, static_cast<std::uint32_t>(resources.size()));
    AppendU32(chunk, static_cast<std::uint32_t>(resourceTable));
    chunk.push_back(std::byte{0});             // minor version
    chunk.push_back(std::byte{majorVersion});  // major version
    chunk.push_back(std::byte{0xFF});          // shader type: pixel
    chunk.push_back(std::byte{0xFF});
    AppendU32(chunk, 0);  // flags
    AppendU32(chunk, 0);  // creator offset

    chunk.insert(chunk.end(), resourceEntries.begin(), resourceEntries.end());
    chunk.insert(chunk.end(), table.begin(), table.end());
    chunk.insert(chunk.end(), variables.begin(), variables.end());
    chunk.insert(chunk.end(), names.begin(), names.end());
    return chunk;
}

// Wraps chunk bodies in a container, exactly as the shader compiler emits it.
[[nodiscard]] std::vector<std::byte> BuildContainer(
    const std::vector<std::pair<std::uint32_t, std::vector<std::byte>>>& chunks)
{
    std::vector<std::byte> container;
    AppendU32(container, 0x43425844u);  // 'DXBC'
    for (int index = 0; index < 16; ++index) {
        container.push_back(std::byte{0});  // digest
    }
    AppendU32(container, 1);  // version
    AppendU32(container, 0);  // total size, patched below
    AppendU32(container, static_cast<std::uint32_t>(chunks.size()));

    const std::size_t offsetTable = container.size();
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        AppendU32(container, 0);
    }

    for (std::size_t index = 0; index < chunks.size(); ++index) {
        WriteU32(
            container,
            offsetTable + index * sizeof(std::uint32_t),
            static_cast<std::uint32_t>(container.size()));
        AppendU32(container, chunks[index].first);
        AppendU32(container, static_cast<std::uint32_t>(chunks[index].second.size()));
        container.insert(
            container.end(), chunks[index].second.begin(), chunks[index].second.end());
    }

    WriteU32(container, 24, static_cast<std::uint32_t>(container.size()));
    return container;
}

[[nodiscard]] std::vector<std::byte> BuildShader(
    const std::vector<BufferSpec>& buffers,
    const std::uint8_t majorVersion = 5,
    const std::vector<ResourceSpec>& resources = {})
{
    return BuildContainer(
        {{0x46454452u, BuildReflectionChunk(buffers, majorVersion, resources)}});
}

const std::vector<BufferSpec> kLightingLikeShader{
    {"PerGeometry",
     752,
     {{"DirLightDirection", 0, 16}, {"DirLightColor", 16, 16}, {"AmbientColor", 32, 16}}},
    {"PerMaterial", 64, {{"MaterialData", 0, 16}}},
};

}

TEST_CASE("shader bytecode names its own constant buffers")
{
    SECTION("every buffer and field is recovered with its offset")
    {
        const auto bytecode = BuildShader(kLightingLikeShader);
        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);

        REQUIRE(reflection.buffers.size() == 2);
        CHECK(reflection.buffers[0].name == "PerGeometry");
        CHECK(reflection.buffers[0].size == 752);
        REQUIRE(reflection.buffers[0].variables.size() == 3);
        CHECK(reflection.buffers[0].variables[0].name == "DirLightDirection");
        CHECK(reflection.buffers[0].variables[0].offset == 0);
        CHECK(reflection.buffers[0].variables[0].size == 16);
        CHECK(reflection.buffers[0].variables[2].name == "AmbientColor");
        CHECK(reflection.buffers[0].variables[2].offset == 32);

        CHECK(reflection.buffers[1].name == "PerMaterial");
        CHECK(reflection.buffers[1].size == 64);
        REQUIRE(reflection.buffers[1].variables.size() == 1);
        CHECK(reflection.buffers[1].variables[0].name == "MaterialData");
    }

    SECTION("a shader model 4 container uses the shorter variable stride")
    {
        // The two models differ only in how many bytes each variable record
        // occupies. Reading a model 4 chunk at the model 5 stride walks off
        // the end of the table and produces names from unrelated bytes, so
        // both strides have to be exercised against the same fields.
        const auto bytecode = BuildShader(kLightingLikeShader, 4);
        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);

        REQUIRE(reflection.buffers.size() == 2);
        REQUIRE(reflection.buffers[0].variables.size() == 3);
        CHECK(reflection.buffers[0].variables[1].name == "DirLightColor");
        CHECK(reflection.buffers[0].variables[1].offset == 16);
        CHECK(reflection.buffers[0].variables[2].name == "AmbientColor");
    }

    SECTION("the reflection chunk is found among other chunks")
    {
        // A real container puts the input signature, the output signature and
        // the bytecode itself around the reflection chunk. Finding it means
        // walking the offset table, not assuming a position.
        auto reflectionChunk = BuildReflectionChunk(kLightingLikeShader, 5);
        const std::vector<std::byte> filler(64, std::byte{0x7E});
        const auto bytecode = BuildContainer({
            {0x4E475349u, filler},  // 'ISGN'
            {0x4E47534Fu, filler},  // 'OSGN'
            {0x46454452u, std::move(reflectionChunk)},
            {0x52444853u, filler},  // 'SHDR'
        });

        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);
        REQUIRE(reflection.buffers.size() == 2);
        CHECK(reflection.buffers[0].variables[0].name == "DirLightDirection");
    }

    SECTION("a shader with no reflection chunk is reported, not guessed at")
    {
        const std::vector<std::byte> filler(64, std::byte{0x7E});
        const auto bytecode = BuildContainer({{0x52444853u, filler}});
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection)
              == ReflectionError::MissingReflectionChunk);
    }

    SECTION("a shader with no constant buffers reflects as empty")
    {
        const auto bytecode = BuildShader({});
        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);
        CHECK(reflection.buffers.empty());
    }
}

TEST_CASE("malformed shader bytecode is refused rather than parsed")
{
    SECTION("a foreign container is rejected by magic")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        WriteU32(bytecode, 0, 0x43425845u);  // 'EXBC'
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::BadMagic);
    }

    SECTION("a container shorter than its own header is refused")
    {
        const std::vector<std::byte> bytecode(16, std::byte{0});
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::TruncatedContainer);
    }

    SECTION("a container whose declared size disagrees with its bytes is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        WriteU32(bytecode, 24, static_cast<std::uint32_t>(bytecode.size() + 1));
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::TruncatedContainer);
    }

    SECTION("a chunk count that overruns the offset table is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        WriteU32(bytecode, 28, 0xFFFFFFFFu);
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::TruncatedContainer);
    }

    SECTION("a chunk offset past the end is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        WriteU32(bytecode, 32, static_cast<std::uint32_t>(bytecode.size() - 4));
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    SECTION("a reflection chunk that claims more bytes than remain is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        const std::uint32_t chunkOffset = ReadU32(bytecode, 32);
        WriteU32(bytecode, chunkOffset + 4, static_cast<std::uint32_t>(bytecode.size()));
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::TruncatedChunk);
    }

    SECTION("a reflection chunk shorter than its own header is refused")
    {
        const std::vector<std::byte> stub(12, std::byte{0});
        const auto bytecode = BuildContainer({{0x46454452u, stub}});
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::TruncatedChunk);
    }

    SECTION("a buffer count that overruns the buffer table is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        const std::size_t body = ReadU32(bytecode, 32) + 8;
        WriteU32(bytecode, body, 0xFFFFFFFFu);
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    SECTION("a variable count that overruns its table is refused")
    {
        // The count sits in the buffer record, which is what makes this
        // distinct from the buffer-count bound: a buffer table that fits can
        // still point at a variable table that does not.
        auto bytecode = BuildShader(kLightingLikeShader);
        const std::size_t body = ReadU32(bytecode, 32) + 8;
        const std::size_t bufferTable = body + 28;
        WriteU32(bytecode, bufferTable + 4, 0xFFFFFFFFu);
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    // The next two build a container whose reflection chunk is followed by
    // another chunk, then aim a table just past the reflection chunk's end.
    //
    // The bound has to be checked before the remaining-byte subtraction: an
    // offset past the end makes that subtraction wrap to a huge number, and
    // every count then looks like it fits. What makes this worth a test of
    // its own is that the overrun does not announce itself. The bytes after
    // the chunk belong to the next chunk, so they parse -- the fields below
    // are arranged so a parser that reads them succeeds and reports a
    // constant buffer that the shader does not have. Returning a wrong layout
    // is the failure that matters here, because a layout is believed.

    SECTION("a buffer table starting past the end is refused")
    {
        auto bytecode = BuildContainer({
            {0x46454452u, BuildReflectionChunk(kLightingLikeShader, 5)},
            {0x52444853u, std::vector<std::byte>(64, std::byte{0x7E})},  // 'SHDR'
        });
        const std::uint32_t chunkOffset = ReadU32(bytecode, 32);
        const std::size_t body = chunkOffset + 8;
        const std::uint32_t chunkSize = ReadU32(bytecode, chunkOffset + 4);
        const std::uint32_t validName = ReadU32(bytecode, body + 28);

        WriteU32(bytecode, body, 1);              // one buffer
        WriteU32(bytecode, body + 4, chunkSize + 4);  // table starts past the end
        const std::size_t entry = body + chunkSize + 4;
        WriteU32(bytecode, entry, validName);     // a name that does resolve
        WriteU32(bytecode, entry + 4, 0);         // no variables
        WriteU32(bytecode, entry + 8, 0);
        WriteU32(bytecode, entry + 12, 999);      // a size that cannot be real

        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    SECTION("a variable table starting past the end is refused")
    {
        auto bytecode = BuildContainer({
            {0x46454452u, BuildReflectionChunk(kLightingLikeShader, 5)},
            {0x52444853u, std::vector<std::byte>(64, std::byte{0x7E})},  // 'SHDR'
        });
        const std::uint32_t chunkOffset = ReadU32(bytecode, 32);
        const std::size_t body = chunkOffset + 8;
        const std::uint32_t chunkSize = ReadU32(bytecode, chunkOffset + 4);
        const std::uint32_t validName = ReadU32(bytecode, body + 28);

        WriteU32(bytecode, body, 1);                       // one buffer
        WriteU32(bytecode, body + 28 + 4, 1);              // holding one variable
        WriteU32(bytecode, body + 28 + 8, chunkSize + 4);  // whose table is past the end
        const std::size_t record = body + chunkSize + 4;
        WriteU32(bytecode, record, validName);
        WriteU32(bytecode, record + 4, 0x1234);
        WriteU32(bytecode, record + 8, 0x5678);

        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    SECTION("a name offset past the end of the chunk is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader);
        const std::size_t body = ReadU32(bytecode, 32) + 8;
        const std::size_t bufferTable = body + 28;
        WriteU32(bytecode, bufferTable, 0xFFFFFFFFu);
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    SECTION("an unterminated name is refused rather than run past the chunk")
    {
        // The string pool is last in the chunk, so a name whose terminator is
        // overwritten would otherwise be assembled from whatever the container
        // stores after it. A plausible-looking field name is precisely the
        // thing that would then be believed.
        auto bytecode = BuildShader(kLightingLikeShader);
        const std::uint32_t chunkOffset = ReadU32(bytecode, 32);
        const std::size_t body = chunkOffset + 8;
        const std::size_t chunkSize = ReadU32(bytecode, chunkOffset + 4);
        for (std::size_t index = body; index < body + chunkSize; ++index) {
            if (bytecode[index] == std::byte{0}) {
                continue;
            }
        }
        // Fill the whole string pool, terminators included, with printable
        // bytes so no name in it ends inside the chunk.
        const std::size_t poolStart = body + 28 + (2 * 24) + (4 * 40);
        for (std::size_t index = poolStart; index < body + chunkSize; ++index) {
            bytecode[index] = std::byte{'A'};
        }
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }
}

TEST_CASE("a shader names its own bound textures and samplers")
{
    // The constant-buffer work found the sun, but it could not find which PS
    // shader-resource slot a draw's base-colour texture is bound at: the
    // engine has no single fixed convention, and asking a live capture "what
    // is bound at slot 0" answers a different question for every technique.
    // The bind point is a property of the shader that declares it, the same
    // way a constant's offset is -- so it is read the same way.
    const std::vector<ResourceSpec> resources{
        {"BaseColorTexture", kSitTexture, 3},
        {"BaseColorSampler", kSitSampler, 3},
        {"NormalTexture", kSitTexture, 4},
    };

    SECTION("name, kind and bind point are recovered for every resource")
    {
        const auto bytecode = BuildShader(kLightingLikeShader, 5, resources);
        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);

        REQUIRE(reflection.resources.size() == 3);
        CHECK(reflection.resources[0].name == "BaseColorTexture");
        CHECK(reflection.resources[0].kind == ResourceKind::Texture);
        CHECK(reflection.resources[0].bindPoint == 3);
        CHECK(reflection.resources[1].name == "BaseColorSampler");
        CHECK(reflection.resources[1].kind == ResourceKind::Sampler);
        CHECK(reflection.resources[1].bindPoint == 3);
        CHECK(reflection.resources[2].name == "NormalTexture");
        CHECK(reflection.resources[2].kind == ResourceKind::Texture);
        CHECK(reflection.resources[2].bindPoint == 4);
        // The constant buffers this same shader declares are unaffected --
        // the two tables are independent, and a bug that let one overwrite
        // the other would still pass a test that only ever looked at one.
        REQUIRE(reflection.buffers.size() == 2);
        CHECK(reflection.buffers[0].name == "PerGeometry");
    }

    SECTION("a resource type this parser does not name is kept, not dropped")
    {
        // A constant buffer or a UAV entry in the same table is not a texture
        // or a sampler, but it is still a real declaration, and silently
        // dropping it would undercount how many resources the shader has.
        const std::vector<ResourceSpec> mixed{
            {"BaseColorTexture", kSitTexture, 0},
            {"SomeConstantBuffer", /*D3D_SIT_CBUFFER=*/0, 1},
        };
        const auto bytecode = BuildShader({}, 5, mixed);
        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);
        REQUIRE(reflection.resources.size() == 2);
        CHECK(reflection.resources[0].kind == ResourceKind::Texture);
        CHECK(reflection.resources[1].kind == ResourceKind::Other);
    }

    SECTION("a shader with no bound resources reflects as empty")
    {
        const auto bytecode = BuildShader(kLightingLikeShader, 5, {});
        ReflectedShader reflection{};
        REQUIRE(ReflectShader(bytecode, reflection) == ReflectionError::None);
        CHECK(reflection.resources.empty());
    }

    SECTION("a resource count that overruns its table is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader, 5, resources);
        const std::size_t body = ReadU32(bytecode, 32) + 8;
        // Offset 8 within the chunk header is resourceCount, not bufferCount
        // (offset 0) -- writing to the wrong one exercises the buffer-table
        // bound this test does not claim to cover, and would pass even with
        // the resource-table bound deleted entirely.
        WriteU32(bytecode, body + 8, 0xFFFFFFFFu);
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }

    SECTION("a resource name offset past the end of the chunk is refused")
    {
        auto bytecode = BuildShader(kLightingLikeShader, 5, resources);
        const std::size_t body = ReadU32(bytecode, 32) + 8;
        const std::size_t resourceTable = body + 28;
        WriteU32(bytecode, resourceTable, 0xFFFFFFFFu);
        ReflectedShader reflection{};
        CHECK(ReflectShader(bytecode, reflection) == ReflectionError::InvalidOffset);
    }
}
