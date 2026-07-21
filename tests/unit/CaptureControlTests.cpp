#include "renderer_host/CaptureControl.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace vf::renderer::capture;

namespace {

std::string Document(
    const std::string& sequence,
    const std::string& kind,
    const std::string& path)
{
    return "sequence=" + sequence + "\nkind=" + kind + "\npath=" + path +
        "\n";
}

}

TEST_CASE("P08L_capture_requests_parse_sequence_kind_and_path",
    "[live][capture]")
{
    RequestGate gate;
    CaptureRequest request{};
    REQUIRE(gate.Accept(
        Document("7", "texture", "F:\\captures\\live.vftex"), request) ==
        RequestError::None);
    CHECK(request.sequence == 7);
    CHECK(request.kind == CaptureKind::Texture);
    CHECK(request.path == "F:\\captures\\live.vftex");
    CHECK(gate.LastSequence() == 7);

    // Trailing whitespace, comments, and blank lines are tolerated because a
    // shell writes these files.
    CaptureRequest tolerant{};
    REQUIRE(gate.Accept(
        "# live capture\r\nsequence = 8 \r\n\r\nkind = mesh \r\n"
        "path = F:\\captures\\live.vfmesh \r\n", tolerant) ==
        RequestError::None);
    CHECK(tolerant.sequence == 8);
    CHECK(tolerant.kind == CaptureKind::Mesh);
    CHECK(tolerant.path == "F:\\captures\\live.vfmesh");
}

TEST_CASE("P08L_capture_requests_only_advance_forward", "[live][capture]")
{
    RequestGate gate;
    CaptureRequest request{};
    REQUIRE(gate.Accept(
        Document("4", "trace", "F:\\captures\\live.vftrace"), request) ==
        RequestError::None);
    // The same document is polled repeatedly until the harness replaces it,
    // so a repeated sequence must not re-arm a one-shot capture.
    CHECK(gate.Accept(
        Document("4", "trace", "F:\\captures\\live.vftrace"), request) ==
        RequestError::StaleSequence);
    CHECK(gate.Accept(
        Document("3", "trace", "F:\\captures\\live.vftrace"), request) ==
        RequestError::StaleSequence);
    REQUIRE(gate.Accept(
        Document("5", "trace", "F:\\captures\\other.vftrace"), request) ==
        RequestError::None);
    CHECK(request.sequence == 5);
}

TEST_CASE("P08L_capture_requests_fail_closed_on_malformed_input",
    "[live][capture]")
{
    RequestGate gate;
    CaptureRequest request{};
    CHECK(gate.Accept("", request) == RequestError::Empty);
    CHECK(gate.Accept("kind=mesh\npath=F:\\a.vfmesh\n", request) ==
        RequestError::MissingSequence);
    CHECK(gate.Accept("sequence=1\npath=F:\\a.vfmesh\n", request) ==
        RequestError::MissingKind);
    CHECK(gate.Accept("sequence=1\nkind=mesh\n", request) ==
        RequestError::MissingPath);
    CHECK(gate.Accept("sequence=1\nkind=weather\npath=F:\\a.vfmesh\n",
        request) == RequestError::UnknownKind);
    CHECK(gate.Accept("sequence=x\nkind=mesh\npath=F:\\a.vfmesh\n",
        request) == RequestError::MissingSequence);
    CHECK(gate.Accept("sequence=1\nkind=mesh\nunexpected=1\n"
        "path=F:\\a.vfmesh\n", request) == RequestError::UnknownField);
    CHECK(gate.Accept("sequence=1 kind=mesh\n", request) ==
        RequestError::MalformedLine);
    CHECK(gate.Accept(std::string(64 * 1024, 'x'), request) ==
        RequestError::TooLarge);
    // Nothing malformed may advance the gate.
    CHECK(gate.LastSequence() == 0);
}

TEST_CASE("P08L_capture_paths_are_absolute_scoped_and_typed",
    "[live][capture]")
{
    RequestGate gate;
    CaptureRequest request{};
    // The game process writes this file, so a request may never escape into
    // a relative path, a traversal, or a foreign extension.
    CHECK(gate.Accept(Document("1", "mesh", "captures\\live.vfmesh"),
        request) == RequestError::InvalidPath);
    CHECK(gate.Accept(Document("1", "mesh", "F:\\a\\..\\b.vfmesh"),
        request) == RequestError::InvalidPath);
    CHECK(gate.Accept(Document("1", "mesh", "F:\\captures\\live.vftex"),
        request) == RequestError::InvalidPath);
    CHECK(gate.Accept(Document("1", "mesh", "F:\\captures\\live.esp"),
        request) == RequestError::InvalidPath);
    CHECK(gate.Accept(Document("1", "texture", "\\\\host\\s\\live.vftex"),
        request) == RequestError::InvalidPath);
    REQUIRE(gate.Accept(Document("1", "texture", "F:\\c\\live.vftex"),
        request) == RequestError::None);

    // Every capture kind has exactly one accepted extension.
    const std::pair<const char*, const char*> pairs[]{
        {"mesh", "F:\\c\\a.vfmesh"},
        {"texture", "F:\\c\\a.vftex"},
        {"trace", "F:\\c\\a.vftrace"},
        {"frame", "F:\\c\\a.vfframe"},
        {"scene", "F:\\c\\a.vfscene"},
        {"deformation", "F:\\c\\a.vfdeform"},
    };
    std::uint64_t sequence = 2;
    for (const auto& [kind, path] : pairs) {
        CaptureRequest accepted{};
        REQUIRE(gate.Accept(
            Document(std::to_string(sequence), kind, path), accepted) ==
            RequestError::None);
        CHECK(accepted.kind != CaptureKind::None);
        ++sequence;
    }
}

TEST_CASE("P08L_capture_results_report_a_single_parsable_marker",
    "[live][capture]")
{
    CaptureRequest request{};
    request.sequence = 12;
    request.kind = CaptureKind::Frame;
    request.path = "F:\\c\\live.vfframe";
    CHECK(FormatResult(request, CaptureOutcome::Complete, "") ==
        "renderer-capture-request: sequence=12 kind=frame result=complete "
        "path=F:\\c\\live.vfframe detail=");
    CHECK(FormatResult(request, CaptureOutcome::Rejected, "not armed") ==
        "renderer-capture-request: sequence=12 kind=frame result=rejected "
        "path=F:\\c\\live.vfframe detail=not armed");
    CHECK(std::string{ToString(RequestError::StaleSequence)} ==
        "stale-sequence");
}
