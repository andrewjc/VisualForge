#include "renderer_host/CaptureControl.h"

#include <algorithm>
#include <array>
#include <charconv>

namespace vf::renderer::capture {

namespace {

struct KindEntry
{
    std::string_view name;
    CaptureKind kind;
    std::string_view extension;
};

constexpr std::array<KindEntry, 6> kKinds{{
    {"mesh", CaptureKind::Mesh, ".vfmesh"},
    {"texture", CaptureKind::Texture, ".vftex"},
    {"trace", CaptureKind::Trace, ".vftrace"},
    {"frame", CaptureKind::Frame, ".vfframe"},
    {"scene", CaptureKind::Scene, ".vfscene"},
    {"deformation", CaptureKind::Deformation, ".vfdeform"},
}};

std::string_view Trim(std::string_view value) noexcept
{
    const auto isSpace = [](const char character) {
        return character == ' ' || character == '\t' || character == '\r';
    };
    while (!value.empty() && isSpace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && isSpace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

bool EqualsInsensitive(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lower = [](const char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a') : character;
        };
        if (lower(left[index]) != lower(right[index])) return false;
    }
    return true;
}

const KindEntry* FindKind(const std::string_view name) noexcept
{
    const auto found = std::find_if(kKinds.begin(), kKinds.end(),
        [name](const KindEntry& entry) {
            return EqualsInsensitive(entry.name, name);
        });
    return found == kKinds.end() ? nullptr : &*found;
}

// The game process performs the write, so a request may only name an
// absolute local path with the extension its capture kind owns.
bool ValidPath(
    const std::string_view path,
    const CaptureKind kind) noexcept
{
    if (path.size() < 4 || path.size() > 1024) return false;
    const auto driveQualified = path.size() > 2 &&
        ((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    if (!driveQualified) return false;
    if (path.find("..") != std::string_view::npos) return false;
    if (path.find('\"') != std::string_view::npos) return false;
    for (const char character : path) {
        if (static_cast<unsigned char>(character) < 0x20) return false;
    }
    const auto entry = std::find_if(kKinds.begin(), kKinds.end(),
        [kind](const KindEntry& candidate) {
            return candidate.kind == kind;
        });
    if (entry == kKinds.end()) return false;
    if (path.size() <= entry->extension.size()) return false;
    const auto suffix = path.substr(path.size() - entry->extension.size());
    return EqualsInsensitive(suffix, entry->extension);
}

}

RequestError RequestGate::Accept(
    const std::string_view document,
    CaptureRequest& request) noexcept
{
    request = {};
    if (document.empty()) return RequestError::Empty;
    if (document.size() > kMaximumRequestBytes) {
        return RequestError::TooLarge;
    }
    std::uint64_t sequence = 0;
    bool haveSequence = false;
    const KindEntry* kind = nullptr;
    std::string_view path;
    std::size_t cursor = 0;
    while (cursor <= document.size()) {
        const auto newline = document.find('\n', cursor);
        const auto line = Trim(document.substr(cursor,
            newline == std::string_view::npos
                ? std::string_view::npos : newline - cursor));
        cursor = newline == std::string_view::npos
            ? document.size() + 1 : newline + 1;
        if (line.empty() || line.front() == '#') continue;
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            return RequestError::MalformedLine;
        }
        const auto field = Trim(line.substr(0, separator));
        const auto value = Trim(line.substr(separator + 1));
        if (field.empty() || value.empty() ||
            field.find(' ') != std::string_view::npos ||
            value.find('=') != std::string_view::npos) {
            return RequestError::MalformedLine;
        }
        if (EqualsInsensitive(field, "sequence")) {
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), sequence);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                sequence == 0) {
                return RequestError::MissingSequence;
            }
            haveSequence = true;
        } else if (EqualsInsensitive(field, "kind")) {
            kind = FindKind(value);
            if (kind == nullptr) return RequestError::UnknownKind;
        } else if (EqualsInsensitive(field, "path")) {
            path = value;
        } else {
            return RequestError::UnknownField;
        }
    }
    if (!haveSequence) return RequestError::MissingSequence;
    if (kind == nullptr) return RequestError::MissingKind;
    if (path.empty()) return RequestError::MissingPath;
    if (!ValidPath(path, kind->kind)) return RequestError::InvalidPath;
    if (sequence <= lastSequence_) return RequestError::StaleSequence;

    try {
        request.path.assign(path);
    } catch (...) {
        request = {};
        return RequestError::TooLarge;
    }
    request.sequence = sequence;
    request.kind = kind->kind;
    lastSequence_ = sequence;
    return RequestError::None;
}

std::uint64_t RequestGate::LastSequence() const noexcept
{
    return lastSequence_;
}

std::string FormatResult(
    const CaptureRequest& request,
    const CaptureOutcome outcome,
    const std::string_view detail)
{
    std::string text = "renderer-capture-request: sequence=";
    text += std::to_string(request.sequence);
    text += " kind=";
    text += ToString(request.kind);
    text += " result=";
    text += outcome == CaptureOutcome::Complete ? "complete" : "rejected";
    text += " path=";
    text += request.path;
    text += " detail=";
    text.append(detail);
    return text;
}

const char* ToString(const CaptureKind kind) noexcept
{
    switch (kind) {
    case CaptureKind::None: return "none";
    case CaptureKind::Mesh: return "mesh";
    case CaptureKind::Texture: return "texture";
    case CaptureKind::Trace: return "trace";
    case CaptureKind::Frame: return "frame";
    case CaptureKind::Scene: return "scene";
    case CaptureKind::Deformation: return "deformation";
    }
    return "unknown";
}

const char* ToString(const RequestError error) noexcept
{
    switch (error) {
    case RequestError::None: return "none";
    case RequestError::Empty: return "empty";
    case RequestError::TooLarge: return "too-large";
    case RequestError::MalformedLine: return "malformed-line";
    case RequestError::UnknownField: return "unknown-field";
    case RequestError::MissingSequence: return "missing-sequence";
    case RequestError::MissingKind: return "missing-kind";
    case RequestError::MissingPath: return "missing-path";
    case RequestError::UnknownKind: return "unknown-kind";
    case RequestError::InvalidPath: return "invalid-path";
    case RequestError::StaleSequence: return "stale-sequence";
    }
    return "unknown";
}

const char* ExpectedExtension(const CaptureKind kind) noexcept
{
    switch (kind) {
    case CaptureKind::Mesh: return ".vfmesh";
    case CaptureKind::Texture: return ".vftex";
    case CaptureKind::Trace: return ".vftrace";
    case CaptureKind::Frame: return ".vfframe";
    case CaptureKind::Scene: return ".vfscene";
    case CaptureKind::Deformation: return ".vfdeform";
    case CaptureKind::None: break;
    }
    return "";
}

}
