#include "bgmedia/vrm/VrmModel.hpp"

#include "cgltf.h"
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bgmedia::vrm {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kMaxFileBytes = 512ull * 1024ull * 1024ull;
constexpr std::size_t kMaxNodes = 200'000;
constexpr std::size_t kMaxMeshes = 100'000;
constexpr std::size_t kMaxPrimitives = 200'000;
constexpr std::size_t kMaxVerticesPerPrimitive = 5'000'000;
constexpr std::size_t kMaxIndicesPerPrimitive = 20'000'000;
constexpr std::size_t kMaxSkinJoints = 2'048;
constexpr std::size_t kMaxTextures = 4'096;
constexpr std::size_t kMaxDecodedBytes = 512ull * 1024ull * 1024ull;
constexpr int kMaxImageDimension = 8'192;

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

Quat quatFromArray(const float* values) {
    return Quat{values[0], values[1], values[2], values[3]}.normalized();
}

Vec3 vec3FromArray(const float* values) {
    return {values[0], values[1], values[2]};
}

// ---------------------------------------------------------------------------
// Minimal JSON DOM, used only for the VRM extension block. The glTF core is
// handled by cgltf. Keeping this parser small and bounded means a corrupted
// VRM cannot recurse without limit or read outside the JSON chunk.
// ---------------------------------------------------------------------------
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;

    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
            if (it->first == key) {
                return &it->second;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const JsonValue* at(std::size_t index) const {
        return index < items.size() ? &items[index] : nullptr;
    }
    [[nodiscard]] bool isObject() const { return type == Type::Object; }
    [[nodiscard]] bool isArray() const { return type == Type::Array; }
    [[nodiscard]] bool isString() const { return type == Type::String; }
    [[nodiscard]] bool isNumber() const { return type == Type::Number; }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& out, std::string& error) {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            error = lastError_;
            return false;
        }
        skipWhitespace();
        if (position_ != text_.size()) {
            error = "unexpected trailing JSON data at byte " +
                    std::to_string(position_);
            return false;
        }
        return true;
    }

private:
    bool fail(std::string message, int depth) {
        lastError_ = std::move(message) + " at byte " + std::to_string(position_);
        if (depth > 0) {
            lastError_ += " (depth " + std::to_string(depth) + ")";
        }
        return false;
    }

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] char peek() const {
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > 128) {
            lastError_ = "JSON nesting exceeds 128 levels";
            return false;
        }
        skipWhitespace();
        if (position_ >= text_.size()) {
            return fail("unexpected end of JSON", depth);
        }
        const char c = text_[position_];
        if (c == '{') {
            return parseObject(out, depth);
        }
        if (c == '[') {
            return parseArray(out, depth);
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.string, depth);
        }
        if (c == 't') {
            return parseLiteral("true", out, true, depth);
        }
        if (c == 'f') {
            return parseLiteral("false", out, false, depth);
        }
        if (c == 'n') {
            return parseNull(out, depth);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber(out, depth);
        }
        return fail("invalid JSON value", depth);
    }

    bool parseObject(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        ++position_;
        skipWhitespace();
        if (peek() == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                return fail("expected object key", depth);
            }
            std::string key;
            if (!parseString(key, depth)) {
                return false;
            }
            skipWhitespace();
            if (peek() != ':') {
                return fail("expected ':' after object key", depth);
            }
            ++position_;
            JsonValue value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out.fields.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == '}') {
                ++position_;
                return true;
            }
            return fail("expected ',' or '}' in object", depth);
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        ++position_;
        skipWhitespace();
        if (peek() == ']') {
            ++position_;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(value));
            skipWhitespace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == ']') {
                ++position_;
                return true;
            }
            return fail("expected ',' or ']' in array", depth);
        }
    }

    bool parseLiteral(std::string_view literal, JsonValue& out, bool value,
                      int depth) {
        if (text_.substr(position_, literal.size()) != literal) {
            return fail("invalid literal", depth);
        }
        position_ += literal.size();
        out.type = JsonValue::Type::Bool;
        out.boolean = value;
        return true;
    }

    bool parseNull(JsonValue& out, int depth) {
        if (text_.substr(position_, 4) != "null") {
            return fail("invalid literal", depth);
        }
        position_ += 4;
        out.type = JsonValue::Type::Null;
        return true;
    }

    bool parseNumber(JsonValue& out, int depth) {
        const std::size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        if (peek() == '0') {
            ++position_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        } else {
            return fail("invalid number", depth);
        }
        if (peek() == '.') {
            ++position_;
            if (!(peek() >= '0' && peek() <= '9')) {
                return fail("invalid number fraction", depth);
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') {
                ++position_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                return fail("invalid number exponent", depth);
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        const std::string token(text_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) {
            return fail("number is not finite", depth);
        }
        out.type = JsonValue::Type::Number;
        out.number = value;
        return true;
    }

    bool parseString(std::string& out, int depth) {
        if (peek() != '"') {
            return fail("expected string", depth);
        }
        ++position_;
        out.clear();
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return fail("control character in string", depth);
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= text_.size()) {
                return fail("truncated escape", depth);
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code = 0;
                if (!readHex4(code, depth)) {
                    return false;
                }
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u') {
                        return fail("unpaired high surrogate", depth);
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!readHex4(low, depth)) {
                        return false;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return fail("invalid low surrogate", depth);
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return fail("unpaired low surrogate", depth);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("unknown escape", depth);
            }
        }
        return fail("unterminated string", depth);
    }

    bool readHex4(std::uint32_t& value, int depth) {
        if (position_ + 4 > text_.size()) {
            return fail("truncated unicode escape", depth);
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid unicode escape", depth);
            }
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::string lastError_;
};

const JsonValue* objectField(const JsonValue* object, std::string_view key) {
    return object && object->isObject() ? object->find(key) : nullptr;
}

std::string stringField(const JsonValue* object, std::string_view key) {
    const JsonValue* value = objectField(object, key);
    return value && value->isString() ? value->string : std::string{};
}

double numberField(const JsonValue* object, std::string_view key,
                   double fallback = 0.0) {
    const JsonValue* value = objectField(object, key);
    return value && value->isNumber() ? value->number : fallback;
}

Vec3 vec3Field(const JsonValue* object, std::string_view key, Vec3 fallback) {
    const JsonValue* value = objectField(object, key);
    if (!value) {
        return fallback;
    }
    if (value->isObject()) {
        return {static_cast<float>(numberField(value, "x", fallback.x)),
                static_cast<float>(numberField(value, "y", fallback.y)),
                static_cast<float>(numberField(value, "z", fallback.z))};
    }
    if (value->isArray() && value->items.size() >= 3 &&
        value->items[0].isNumber() && value->items[1].isNumber() &&
        value->items[2].isNumber()) {
        return {static_cast<float>(value->items[0].number),
                static_cast<float>(value->items[1].number),
                static_cast<float>(value->items[2].number)};
    }
    return fallback;
}

Vec4 vec4Field(const JsonValue* object, std::string_view key, Vec4 fallback) {    const JsonValue* value = objectField(object, key);
    if (!value || !value->isArray() || value->items.size() < 4) {
        return fallback;
    }
    if (!value->items[0].isNumber() || !value->items[1].isNumber() ||
        !value->items[2].isNumber() || !value->items[3].isNumber()) {
        return fallback;
    }
    return {static_cast<float>(value->items[0].number),
            static_cast<float>(value->items[1].number),
            static_cast<float>(value->items[2].number),
            static_cast<float>(value->items[3].number)};
}

bool parseGlbChunks(std::span<const std::uint8_t> bytes, std::string_view& json,
                    std::span<const std::uint8_t>& binary, std::string& error) {
    if (bytes.size() < 12) {
        error = "file is smaller than a GLB header";
        return false;
    }
    const auto readU32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const std::uint32_t magic = readU32(0);
    const std::uint32_t version = readU32(4);
    const std::uint32_t declaredLength = readU32(8);
    if (magic != 0x46546C67u) {
        error = "not a binary glTF (missing glTF magic)";
        return false;
    }
    if (version != 2) {
        error = "only glTF 2.0 GLB containers are supported";
        return false;
    }
    if (declaredLength != bytes.size()) {
        error = "GLB declared length does not match file size";
        return false;
    }
    std::size_t position = 12;
    if (position + 8 > bytes.size()) {
        error = "GLB has no JSON chunk";
        return false;
    }
    const std::uint32_t jsonLength = readU32(position);
    const std::uint32_t jsonType = readU32(position + 4);
    position += 8;
    if (jsonType != 0x4E4F534Au) {
        error = "first GLB chunk is not JSON";
        return false;
    }
    if (jsonLength == 0 || jsonLength > bytes.size() - position) {
        error = "GLB JSON chunk exceeds file bounds";
        return false;
    }
    json = std::string_view(reinterpret_cast<const char*>(bytes.data() + position),
                            jsonLength);
    position += jsonLength;
    if (position + 8 <= bytes.size()) {
        const std::uint32_t binLength = readU32(position);
        const std::uint32_t binType = readU32(position + 4);
        position += 8;
        if (binType == 0x004E4942u && binLength > 0) {
            if (binLength > bytes.size() - position) {
                error = "GLB BIN chunk exceeds file bounds";
                return false;
            }
            binary = bytes.subspan(position, binLength);
        }
    }
    return true;
}

std::string cgltfResultName(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "data too short";
    case cgltf_result_unknown_format:
        return "unknown format";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "legacy glTF 1.0";
    default:
        return "cgltf error " + std::to_string(static_cast<int>(result));
    }
}

bool validateAccessor(const cgltf_accessor* accessor, std::string_view label,
                      cgltf_type expectedType,
                      std::initializer_list<cgltf_component_type> allowedTypes,
                      std::string& error) {
    if (!accessor) {
        error = std::string(label) + " accessor is missing";
        return false;
    }
    if (accessor->is_sparse) {
        error = std::string(label) + " uses sparse accessors, which this importer "
                                     "does not support";
        return false;
    }
    if (accessor->type != expectedType) {
        error = std::string(label) + " has an unexpected element type";
        return false;
    }
    const bool componentAllowed =
        std::find(allowedTypes.begin(), allowedTypes.end(), accessor->component_type) !=
        allowedTypes.end();
    if (!componentAllowed) {
        error = std::string(label) + " has an unsupported component type";
        return false;
    }
    if (!accessor->buffer_view) {
        error = std::string(label) + " has no buffer view";
        return false;
    }
    const cgltf_buffer_view* view = accessor->buffer_view;
    if (!view->buffer || !view->buffer->data) {
        error = std::string(label) + " buffer data is unavailable";
        return false;
    }
    if (view->size > view->buffer->size) {
        error = std::string(label) + " buffer view exceeds its buffer";
        return false;
    }
    const std::size_t elementSize =
        cgltf_calc_size(accessor->type, accessor->component_type);
    if (elementSize == 0) {
        error = std::string(label) + " has zero-sized elements";
        return false;
    }
    const std::size_t stride =
        accessor->stride != 0 ? accessor->stride : elementSize;
    if (accessor->count > 0) {
        if (stride == 0 ||
            accessor->count > (std::numeric_limits<std::uint64_t>::max() /
                               static_cast<std::uint64_t>(stride))) {
            error = std::string(label) + " accessor size overflows";
            return false;
        }
        const std::uint64_t span =
            static_cast<std::uint64_t>(accessor->offset) +
            static_cast<std::uint64_t>(stride) *
                static_cast<std::uint64_t>(accessor->count - 1) +
            static_cast<std::uint64_t>(elementSize);
        if (span > view->size) {
            error = std::string(label) + " accessor reads past its buffer view";
            return false;
        }
    }
    return true;
}

struct GltfDeleter {
    void operator()(cgltf_data* data) const {
        if (data) {
            cgltf_free(data);
        }
    }
};
using GltfPtr = std::unique_ptr<cgltf_data, GltfDeleter>;

std::optional<int> nodeIndexOf(
    const std::unordered_map<const cgltf_node*, int>& index, const cgltf_node* node) {
    if (!node) {
        return std::nullopt;
    }
    const auto it = index.find(node);
    if (it == index.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<int> textureIndexOf(
    const std::unordered_map<const cgltf_texture*, int>& index,
    const cgltf_texture* texture) {
    if (!texture) {
        return std::nullopt;
    }
    const auto it = index.find(texture);
    if (it == index.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool decodeTexture(const cgltf_image* image, Texture& out, std::string& warning) {
    out.width = 0;
    out.height = 0;
    out.rgba.clear();
    out.decoded = false;
    if (!image) {
        warning = "texture has no image";
        return false;
    }
    if (!image->buffer_view) {
        warning = "external or URI texture is not supported: " +
                  std::string(image->uri ? image->uri : "(no buffer view)");
        return false;
    }
    const cgltf_buffer_view* view = image->buffer_view;
    if (!view->buffer || !view->buffer->data) {
        warning = "texture buffer data is unavailable";
        return false;
    }
    if (view->size == 0 || view->size > view->buffer->size ||
        view->size > 128ull * 1024ull * 1024ull) {
        warning = "texture buffer view exceeds its buffer";
        return false;
    }
    const auto* data = static_cast<const unsigned char*>(view->buffer->data) +
                       view->offset;
    int channels = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(view->size), &out.width,
                               &out.height, &channels)) {
        warning = "texture image could not be decoded";
        out.width = 0;
        out.height = 0;
        return false;
    }
    if (out.width <= 0 || out.height <= 0 || out.width > kMaxImageDimension ||
        out.height > kMaxImageDimension) {
        warning = "texture dimensions exceed the supported limit";
        out.width = 0;
        out.height = 0;
        return false;
    }
    int decodedChannels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(view->size),
                                            &out.width, &out.height,
                                            &decodedChannels, 4);
    if (!pixels) {
        warning = "texture decode failed: " +
                  std::string(stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }
    const std::size_t byteCount = static_cast<std::size_t>(out.width) *
                                  static_cast<std::size_t>(out.height) * 4;
    out.rgba.assign(pixels, pixels + byteCount);
    out.decoded = true;
    stbi_image_free(pixels);
    return true;
}

std::string missingBoneList(const VrmModel& model) {
    std::string missing;
    for (std::size_t i = 0; i < kHumanoidBoneCount; ++i) {
        const auto bone = static_cast<HumanoidBone>(i);
        if (model.humanoidNodes[i] >= 0) {
            continue;
        }
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += humanoidBoneName(bone);
    }
    return missing;
}

int countMissingCoreBones(const VrmModel& model) {
    int missing = 0;
    for (int node : model.humanoidNodes) {
        if (node < 0) {
            ++missing;
        }
    }
    return missing;
}

} // namespace

LoadResult VrmLoader::load(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        LoadResult result;
        result.error = "cannot stat VRM file: " + toUtf8(path);
        return result;
    }
    if (fileSize > kMaxFileBytes) {
        LoadResult result;
        result.error = "VRM file is larger than the 512 MiB safety limit";
        return result;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        LoadResult result;
        result.error = "cannot open VRM file: " + toUtf8(path);
        return result;
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxFileBytes) {
        LoadResult result;
        result.error = "VRM file has an invalid size";
        return result;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() &&
        !file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
        LoadResult result;
        result.error = "failed to read VRM file: " + toUtf8(path);
        return result;
    }
    LoadResult result = loadFromMemory(bytes, toUtf8(path));
    if (result.model) {
        result.model->sourcePath = toUtf8(path);
    }
    return result;
}

LoadResult VrmLoader::loadFromMemory(std::span<const std::uint8_t> bytes,
                                     std::string sourceName) {
    const auto started = Clock::now();
    LoadResult result;
    auto model = std::make_unique<VrmModel>();
    model->fileSize = bytes.size();
    model->sourcePath = std::move(sourceName);

    if (bytes.size() > kMaxFileBytes) {
        result.error = "VRM is larger than the 512 MiB safety limit";
        return result;
    }
    std::string_view jsonText;
    std::span<const std::uint8_t> binary;
    std::string error;
    if (!parseGlbChunks(bytes, jsonText, binary, error)) {
        result.error = error;
        return result;
    }
    (void)binary;

    // GLB producers occasionally pad the JSON chunk with NUL bytes; the JSON
    // parser only accepts whitespace. Trim only trailing padding.
    while (!jsonText.empty() &&
           (jsonText.back() == '\0' || jsonText.back() == ' ' ||
            jsonText.back() == '\t' || jsonText.back() == '\r' ||
            jsonText.back() == '\n')) {
        jsonText.remove_suffix(1);
    }

    JsonValue rootJson;
    JsonParser jsonParser(jsonText);
    if (!jsonParser.parse(rootJson, error)) {
        result.error = "VRM JSON: " + error;
        return result;
    }
    const JsonValue* extensions = objectField(&rootJson, "extensions");
    const JsonValue* vrm = objectField(extensions, "VRM");
    if (!vrm) {
        if (objectField(extensions, "VRMC_vrm")) {
            result.error =
                "this file uses VRM 1.0 (VRMC_vrm); this build implements the "
                "VRM 0.x extensions.VRM schema used by Model1.vrm";
        } else {
            result.error = "not a VRM file: extensions.VRM is missing";
        }
        return result;
    }
    if (!vrm->isObject()) {
        result.error = "extensions.VRM is not an object";
        return result;
    }

    cgltf_options options{};
    cgltf_data* rawData = nullptr;
    cgltf_result gltfResult = cgltf_parse(&options, bytes.data(), bytes.size(), &rawData);
    if (gltfResult != cgltf_result_success) {
        result.error = "glTF parse failed: " + cgltfResultName(gltfResult);
        return result;
    }
    GltfPtr data(rawData);
    if (!data) {
        result.error = "glTF parser returned no data";
        return result;
    }
    if (data->file_type != cgltf_file_type_glb) {
        result.error = "VRM must use the binary GLB container";
        return result;
    }
    gltfResult = cgltf_load_buffers(&options, data.get(), nullptr);
    if (gltfResult != cgltf_result_success) {
        result.error = "glTF buffers could not be loaded: " +
                       cgltfResultName(gltfResult);
        return result;
    }
    gltfResult = cgltf_validate(data.get());
    if (gltfResult != cgltf_result_success) {
        // Some legal VRM 0.x files carry optional extension data that cgltf's
        // validator is stricter about. Only the accessors and indices we read
        // below are authoritative, so keep the warning rather than rejecting a
        // file outright.
        model->warnings.push_back(
            "glTF validator warning: " + cgltfResultName(gltfResult));
    }
    if (data->nodes_count == 0 || data->nodes_count > kMaxNodes) {
        result.error = "VRM node count is missing or unreasonable";
        return result;
    }

    model->exporterVersion.clear();
    const JsonValue* exporter = objectField(vrm, "exporterVersion");
    if (exporter && exporter->isString()) {
        model->exporterVersion = exporter->string;
    }
    const JsonValue* spec = objectField(vrm, "specVersion");
    if (spec && spec->isString()) {
        model->specVersion = spec->string;
    }
    if (model->specVersion.empty()) {
        model->specVersion = "0.x";
    }

    const JsonValue* meta = objectField(vrm, "meta");
    model->metadata.title = stringField(meta, "title");
    model->metadata.author = stringField(meta, "author");
    model->metadata.licenseName = stringField(meta, "licenseName");
    model->metadata.allowedUserName = stringField(meta, "allowedUserName");
    model->metadata.commercialUsage =
        stringField(meta, "commercialUssageName");
    if (model->metadata.commercialUsage.empty()) {
        model->metadata.commercialUsage = stringField(meta, "commercialUsageName");
    }
    model->metadata.sexualUsage = stringField(meta, "sexualUssageName");
    model->metadata.violentUsage = stringField(meta, "violentUssageName");
    model->metadata.otherPermission = stringField(meta, "otherPermissionUrl");
    model->metadata.thumbnailTexture =
        static_cast<int>(numberField(meta, "texture", -1.0));
    model->name = model->metadata.title;
    if (model->name.empty()) {
        const std::size_t slash = model->sourcePath.find_last_of("/\\");
        model->name = slash == std::string::npos
                          ? model->sourcePath
                          : model->sourcePath.substr(slash + 1);
    }
    if (const JsonValue* blendShapes = objectField(vrm, "blendShapeMaster")) {
        if (const JsonValue* groups = objectField(blendShapes, "blendShapeGroups")) {
            if (groups->isArray()) {
                model->diagnostics.blendShapeCount = static_cast<int>(groups->items.size());
            }
        }
    }

    // ------------------------------------------------------------------
    // Nodes, hierarchy and bind transforms
    // ------------------------------------------------------------------
    std::unordered_map<const cgltf_node*, int> nodeIndex;
    nodeIndex.reserve(data->nodes_count);
    model->nodes.resize(data->nodes_count);
    for (std::size_t i = 0; i < data->nodes_count; ++i) {
        nodeIndex.emplace(&data->nodes[i], static_cast<int>(i));
    }
    for (std::size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& source = data->nodes[i];
        Node& node = model->nodes[i];
        node.name = source.name ? source.name : std::string{};
        if (source.has_matrix) {
            for (int k = 0; k < 16; ++k) {
                node.local.m[k] = source.matrix[k];
            }
            Vec3 translation;
            Quat rotation;
            Vec3 scale;
            if (!math::decomposeTRS(node.local, &translation, &rotation, &scale)) {
                result.error = "node '" + node.name +
                               "' has a singular or non-finite transform matrix";
                return result;
            }
            node.translation = translation;
            node.rotation = rotation;
            node.scale = scale;
        } else {
            if (source.has_translation) {
                node.translation = vec3FromArray(source.translation);
            }
            if (source.has_rotation) {
                node.rotation = quatFromArray(source.rotation);
            }
            if (source.has_scale) {
                node.scale = vec3FromArray(source.scale);
            }
            node.local = Mat4::fromTRS(node.translation, node.rotation, node.scale);
        }
        if (!node.local.finite() || !math::finite(node.translation) ||
            !math::finite(node.scale)) {
            result.error = "node '" + node.name + "' contains non-finite data";
            return result;
        }
        node.mesh = -1;
        node.skin = -1;
        if (source.mesh) {
            const auto it = std::find_if(
                data->meshes, data->meshes + data->meshes_count,
                [&](const cgltf_mesh& mesh) { return &mesh == source.mesh; });
            node.mesh = it != data->meshes + data->meshes_count
                            ? static_cast<int>(it - data->meshes)
                            : -1;
            if (node.mesh < 0) {
                result.error = "node references an out-of-range mesh";
                return result;
            }
        }
        if (source.skin) {
            const auto it = std::find_if(
                data->skins, data->skins + data->skins_count,
                [&](const cgltf_skin& skin) { return &skin == source.skin; });
            node.skin = it != data->skins + data->skins_count
                            ? static_cast<int>(it - data->skins)
                            : -1;
            if (node.skin < 0) {
                result.error = "node references an out-of-range skin";
                return result;
            }
        }
    }

    for (std::size_t i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& source = data->nodes[i];
        for (std::size_t c = 0; c < source.children_count; ++c) {
            const auto child = nodeIndexOf(nodeIndex, source.children[c]);
            if (!child) {
                result.error = "node hierarchy contains an invalid child";
                return result;
            }
            Node& childNode = model->nodes[static_cast<std::size_t>(*child)];
            if (childNode.parent >= 0 && childNode.parent != static_cast<int>(i)) {
                result.error = "node '" + childNode.name +
                               "' has more than one parent";
                return result;
            }
            childNode.parent = static_cast<int>(i);
            model->nodes[i].children.push_back(*child);
        }
    }

    if (data->scene) {
        const cgltf_scene& scene = *data->scene;
        for (std::size_t i = 0; i < scene.nodes_count; ++i) {
            const auto root = nodeIndexOf(nodeIndex, scene.nodes[i]);
            if (!root) {
                result.error = "default scene references an invalid node";
                return result;
            }
            if (model->nodes[static_cast<std::size_t>(*root)].parent >= 0) {
                result.error = "default scene root has a parent";
                return result;
            }
            if (std::find(model->sceneRoots.begin(), model->sceneRoots.end(), *root) !=
                model->sceneRoots.end()) {
                result.error = "default scene lists a root twice";
                return result;
            }
            model->sceneRoots.push_back(*root);
        }
    }
    if (model->sceneRoots.empty()) {
        for (std::size_t i = 0; i < model->nodes.size(); ++i) {
            if (model->nodes[i].parent < 0) {
                model->sceneRoots.push_back(static_cast<int>(i));
            }
        }
    }
    if (model->sceneRoots.empty()) {
        result.error = "VRM has no scene roots";
        return result;
    }

    model->bindGlobals.resize(model->nodes.size(), Mat4::identity());
    model->bindGlobalRotations.resize(model->nodes.size());
    std::vector<bool> visited(model->nodes.size(), false);
    std::vector<int> stack;
    for (auto it = model->sceneRoots.rbegin(); it != model->sceneRoots.rend(); ++it) {
        stack.push_back(*it);
    }
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();
        if (visited[static_cast<std::size_t>(index)]) {
            result.error = "node hierarchy contains a cycle or duplicate root";
            return result;
        }
        visited[static_cast<std::size_t>(index)] = true;
        Node& node = model->nodes[static_cast<std::size_t>(index)];
        if (node.parent >= 0) {
            const std::size_t parent = static_cast<std::size_t>(node.parent);
            if (!visited[parent]) {
                result.error = "node hierarchy is not topologically ordered";
                return result;
            }
            model->bindGlobals[static_cast<std::size_t>(index)] =
                model->bindGlobals[parent] * node.local;
            model->bindGlobalRotations[static_cast<std::size_t>(index)] =
                (model->bindGlobalRotations[parent] * node.rotation).normalized();
        } else {
            model->bindGlobals[static_cast<std::size_t>(index)] = node.local;
            model->bindGlobalRotations[static_cast<std::size_t>(index)] =
                node.rotation.normalized();
        }
        for (auto child = node.children.rbegin(); child != node.children.rend();
             ++child) {
            stack.push_back(*child);
        }
    }
    if (std::find(visited.begin(), visited.end(), false) != visited.end()) {
        result.error = "node hierarchy contains an unreachable node or cycle";
        return result;
    }

    // ------------------------------------------------------------------
    // Humanoid mapping from VRM 0.x
    // ------------------------------------------------------------------
    const JsonValue* humanoid = objectField(vrm, "humanoid");
    const JsonValue* humanBones = objectField(humanoid, "humanBones");
    if (!humanBones || !humanBones->isArray()) {
        result.error = "extensions.VRM.humanoid.humanBones is missing";
        return result;
    }
    for (const JsonValue& entry : humanBones->items) {
        const std::string name = stringField(&entry, "bone");
        const double rawNode = numberField(&entry, "node", -1.0);
        if (name.empty() || rawNode < 0.0 ||
            rawNode > static_cast<double>(model->nodes.size() - 1) ||
            std::floor(rawNode) != rawNode) {
            model->warnings.push_back("ignored malformed humanoid bone entry");
            continue;
        }
        const int node = static_cast<int>(rawNode);
        HumanoidEntry parsed;
        parsed.name = name;
        parsed.node = node;
        parsed.core = humanoidBoneFromVrmName(name);
        model->humanoidEntries.push_back(parsed);
        if (parsed.core) {
            const std::size_t index = static_cast<std::size_t>(*parsed.core);
            if (model->humanoidNodes[index] >= 0 &&
                model->humanoidNodes[index] != node) {
                model->warnings.push_back("duplicate humanoid bone '" + name +
                                          "'; keeping the first mapping");
            } else {
                model->humanoidNodes[index] = node;
            }
        }
    }
    model->diagnostics.humanoidCount =
        static_cast<int>(model->humanoidEntries.size());
    for (int node : model->humanoidNodes) {
        if (node >= 0) {
            ++model->diagnostics.humanoidCoreCount;
        }
    }
    if (countMissingCoreBones(*model) > 0) {
        model->warnings.push_back("humanoid bindings missing: " +
                                  missingBoneList(*model));
    }

    // ------------------------------------------------------------------
    // Optional finger mapping (VRM 0.x humanoid bone names)
    // ------------------------------------------------------------------
    {
        static constexpr const char* kSides[2] = {"left", "right"};
        static constexpr const char* kFingers[5] = {"Thumb", "Index", "Middle",
                                                    "Ring", "Little"};
        static constexpr const char* kJoints[3] = {"Proximal", "Intermediate",
                                                   "Distal"};
        for (const HumanoidEntry& entry : model->humanoidEntries) {
            if (entry.node < 0 ||
                static_cast<std::size_t>(entry.node) >= model->nodes.size()) {
                continue;
            }
            bool matched = false;
            for (int side = 0; side < 2 && !matched; ++side) {
                for (int finger = 0; finger < kFingerCount && !matched; ++finger) {
                    for (int joint = 0; joint < kFingerJointCount && !matched; ++joint) {
                        const std::string name =
                            std::string(kSides[side]) + kFingers[finger] + kJoints[joint];
                        if (entry.name == name) {
                            model->fingerNodes[static_cast<std::size_t>(side)]
                                               [static_cast<std::size_t>(finger)]
                                               [static_cast<std::size_t>(joint)] = entry.node;
                            matched = true;
                        }
                    }
                }
            }
        }
        model->diagnostics.fingerBoneCount = model->fingerBoneCount();
    }

    // ------------------------------------------------------------------
    // Secondary animation (VRM 0.x extensions.VRM.secondaryAnimation)
    // ------------------------------------------------------------------
    {
        const JsonValue* secondary = objectField(vrm, "secondaryAnimation");
        if (secondary && secondary->isObject()) {
            model->springSource = "VRM.secondaryAnimation";
            const auto validNode = [&](double raw) {
                return raw >= 0.0 &&
                       raw <= static_cast<double>(model->nodes.size() - 1) &&
                       std::floor(raw) == raw;
            };
            if (const JsonValue* colliderGroups =
                    objectField(secondary, "colliderGroups")) {
                if (colliderGroups->isArray()) {
                    for (const JsonValue& group : colliderGroups->items) {
                        SpringColliderGroup parsed;
                        const double rawNode = numberField(&group, "node", -1.0);
                        if (!validNode(rawNode)) {
                            model->warnings.push_back(
                                "secondaryAnimation: ignored collider group with an invalid node");
                            // Keep an empty placeholder: boneGroups reference collider
                            // groups by their ORIGINAL index, so skipping the entry
                            // would silently re-point every later reference.
                            model->springColliderGroups.push_back(std::move(parsed));
                            continue;
                        }
                        parsed.node = static_cast<int>(rawNode);
                        if (const JsonValue* colliders =
                                objectField(&group, "colliders")) {
                            if (colliders->isArray()) {
                                for (const JsonValue& collider : colliders->items) {
                                    SpringCollider item;
                                    item.offset = vec3Field(&collider, "offset", {});
                                    item.radius = static_cast<float>(
                                        std::max(0.0, numberField(&collider, "radius", 0.0)));
                                    if (!math::finite(item.offset) ||
                                        !std::isfinite(item.radius) ||
                                        item.radius <= 0.0f) {
                                        model->warnings.push_back(
                                            "secondaryAnimation: ignored invalid collider sphere");
                                        continue;
                                    }
                                    parsed.colliders.push_back(item);
                                }
                            }
                        }
                        model->springColliderGroups.push_back(std::move(parsed));
                    }
                }
            }
            const int colliderGroupCount =
                static_cast<int>(model->springColliderGroups.size());
            if (const JsonValue* groups = objectField(secondary, "boneGroups")) {
                if (groups->isArray()) {
                    for (const JsonValue& group : groups->items) {
                        SpringBoneGroup parsed;
                        parsed.comment = stringField(&group, "comment");
                        const double center = numberField(&group, "center", -1.0);
                        if (validNode(center)) {
                            parsed.center = static_cast<int>(center);
                        }
                        double stiffness = numberField(&group, "stiffiness",
                                                      numberField(&group, "stiffness", 0.5));
                        parsed.stiffness = std::clamp(static_cast<float>(stiffness), 0.0f, 1.0f);
                        parsed.gravityPower = std::clamp(
                            static_cast<float>(numberField(&group, "gravityPower", 0.0)),
                            0.0f, 10.0f);
                        parsed.dragForce = std::clamp(
                            static_cast<float>(numberField(&group, "dragForce", 0.1)),
                            0.0f, 1.0f);
                        parsed.hitRadius = std::max(
                            0.0f, static_cast<float>(numberField(&group, "hitRadius", 0.01)));
                        parsed.gravityDir = vec3Field(&group, "gravityDir",
                                                      {0.0f, -1.0f, 0.0f});
                        if (!math::finite(parsed.gravityDir) ||
                            parsed.gravityDir.lengthSquared() < 1.0e-6f) {
                            parsed.gravityDir = {0.0f, -1.0f, 0.0f};
                        }
                        if (const JsonValue* bones = objectField(&group, "bones")) {
                            if (bones->isArray()) {
                                for (const JsonValue& bone : bones->items) {
                                    if (!bone.isNumber() || !validNode(bone.number)) {
                                        model->warnings.push_back(
                                            "secondaryAnimation: ignored invalid spring bone node");
                                        continue;
                                    }
                                    parsed.roots.push_back(
                                        static_cast<int>(bone.number));
                                }
                            }
                        }
                        if (const JsonValue* refs =
                                objectField(&group, "colliderGroups")) {
                            if (refs->isArray()) {
                                for (const JsonValue& ref : refs->items) {
                                    if (!ref.isNumber() ||
                                        ref.number < 0.0 ||
                                        ref.number >= static_cast<double>(colliderGroupCount) ||
                                        std::floor(ref.number) != ref.number) {
                                        model->warnings.push_back(
                                            "secondaryAnimation: ignored invalid collider group reference");
                                        continue;
                                    }
                                    parsed.colliderGroups.push_back(
                                        static_cast<int>(ref.number));
                                }
                            }
                        }
                        if (parsed.roots.empty()) {
                            continue;
                        }
                        model->springGroups.push_back(std::move(parsed));
                    }
                }
            }

            // Expand groups into linear chains. A node can be listed as a root
            // in a later group (the coat skirt does this); such a node starts a
            // new chain and ends the parent chain, which avoids simulating the
            // same transform twice.
            std::vector<char> isRoot(model->nodes.size(), 0);
            for (const SpringBoneGroup& group : model->springGroups) {
                for (int root : group.roots) {
                    if (root >= 0 && static_cast<std::size_t>(root) < isRoot.size()) {
                        isRoot[static_cast<std::size_t>(root)] = 1;
                    }
                }
            }
            const std::size_t groupCount = model->springGroups.size();
            for (std::size_t g = 0; g < groupCount; ++g) {
                const SpringBoneGroup& group = model->springGroups[g];
                // A root's subtree is a tree, not a line: at a branching node the
                // first child continues the chain and every other child starts a
                // branch chain anchored at the branch point. Each node is written
                // by exactly one chain; the anchor belongs to the parent chain.
                const auto makeChain = [&](int root) {
                    struct Pending {
                        int parentChain;
                        int parentIndex;
                        int node;
                    };
                    std::vector<Pending> queue;
                    queue.push_back({-1, 0, root});
                    constexpr int kMaxChain = 24;
                    constexpr std::size_t kMaxBranchChains = 512;
                    for (std::size_t q = 0; q < queue.size(); ++q) {
                        const Pending item = queue[q];
                        SpringBoneChain chain;
                        chain.group = static_cast<int>(g);
                        chain.colliderGroups = group.colliderGroups;
                        chain.stiffness = group.stiffness;
                        chain.gravityPower = group.gravityPower;
                        chain.dragForce = group.dragForce;
                        chain.hitRadius = group.hitRadius;
                        chain.gravityDir = group.gravityDir;
                        chain.parentChain = item.parentChain;
                        chain.parentIndex = item.parentIndex;
                        if (item.parentChain >= 0) {
                            const auto& parentNodes =
                                model->springChains[static_cast<std::size_t>(item.parentChain)].nodes;
                            chain.nodes.push_back(
                                parentNodes[static_cast<std::size_t>(item.parentIndex)]);
                        }
                        const int myIndex = static_cast<int>(model->springChains.size());
                        chain.nodes.push_back(item.node);
                        int current = item.node;
                        for (int depth = 0; depth < kMaxChain; ++depth) {
                            const Node& node = model->nodes[static_cast<std::size_t>(current)];
                            int next = -1;
                            for (int child : node.children) {
                                if (child < 0 ||
                                    static_cast<std::size_t>(child) >= model->nodes.size()) {
                                    continue;
                                }
                                if (isRoot[static_cast<std::size_t>(child)] && child != root) {
                                    // A different group simulates this subtree.
                                    continue;
                                }
                                if (next < 0) {
                                    next = child;
                                } else if (queue.size() < kMaxBranchChains) {
                                    queue.push_back(
                                        {myIndex, static_cast<int>(chain.nodes.size()) - 1, child});
                                }
                            }
                            if (next < 0) {
                                break;
                            }
                            chain.nodes.push_back(next);
                            current = next;
                        }
                        model->springChains.push_back(std::move(chain));
                    }
                };
                for (int root : group.roots) {
                    if (root >= 0 && static_cast<std::size_t>(root) < model->nodes.size()) {
                        makeChain(root);
                    }
                }
            }
            model->diagnostics.springGroupCount =
                static_cast<int>(model->springGroups.size());
            model->diagnostics.springChainCount =
                static_cast<int>(model->springChains.size());
            model->diagnostics.springBoneCount = model->springBoneCount();
            model->diagnostics.springColliderGroupCount =
                static_cast<int>(model->springColliderGroups.size());
            int colliderCount = 0;
            for (const SpringColliderGroup& group : model->springColliderGroups) {
                colliderCount += static_cast<int>(group.colliders.size());
            }
            model->diagnostics.springColliderCount = colliderCount;
        }
    }

    // ------------------------------------------------------------------
    // Skins
    // ------------------------------------------------------------------
    if (data->skins_count > model->nodes.size() * 4 + 64) {
        result.error = "unreasonable skin count";
        return result;
    }
    model->skins.resize(data->skins_count);
    for (std::size_t i = 0; i < data->skins_count; ++i) {
        const cgltf_skin& source = data->skins[i];
        Skin& skin = model->skins[i];
        skin.name = source.name ? source.name : std::string{};
        if (source.joints_count == 0 || source.joints_count > kMaxSkinJoints) {
            result.error = "skin '" + skin.name + "' has an invalid joint count";
            return result;
        }
        skin.joints.reserve(source.joints_count);
        for (std::size_t j = 0; j < source.joints_count; ++j) {
            const auto joint = nodeIndexOf(nodeIndex, source.joints[j]);
            if (!joint) {
                result.error = "skin references a joint outside the node table";
                return result;
            }
            skin.joints.push_back(*joint);
        }
        if (source.skeleton) {
            const auto skeleton = nodeIndexOf(nodeIndex, source.skeleton);
            skin.skeleton = skeleton ? *skeleton : -1;
        }
        skin.inverseBindMatrices.assign(source.joints_count, Mat4::identity());
        if (source.inverse_bind_matrices) {
            if (!validateAccessor(source.inverse_bind_matrices,
                                  "skin inverseBindMatrices", cgltf_type_mat4,
                                  {cgltf_component_type_r_32f}, result.error)) {
                return result;
            }
            if (source.inverse_bind_matrices->count < source.joints_count) {
                result.error = "skin inverseBindMatrices count is too small";
                return result;
            }
            for (std::size_t j = 0; j < source.joints_count; ++j) {
                float values[16]{};
                if (!cgltf_accessor_read_float(source.inverse_bind_matrices, j,
                                               values, 16)) {
                    result.error = "failed to read an inverse bind matrix";
                    return result;
                }
                for (int k = 0; k < 16; ++k) {
                    skin.inverseBindMatrices[j].m[k] = values[k];
                }
                if (!skin.inverseBindMatrices[j].finite()) {
                    result.error = "skin contains a non-finite inverse bind matrix";
                    return result;
                }
            }
        }
        model->diagnostics.jointCount += static_cast<int>(skin.joints.size());
    }

    // ------------------------------------------------------------------
    // Materials
    // ------------------------------------------------------------------
    model->materials.resize(data->materials_count);
    std::unordered_map<const cgltf_texture*, int> textureIndex;
    textureIndex.reserve(data->textures_count);
    if (data->textures_count > kMaxTextures) {
        result.error = "VRM texture count is unreasonable";
        return result;
    }
    for (std::size_t i = 0; i < data->textures_count; ++i) {
        textureIndex.emplace(&data->textures[i], static_cast<int>(i));
    }
    for (std::size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& source = data->materials[i];
        Material& material = model->materials[i];
        material.name = source.name ? source.name : std::string{};
        if (source.has_pbr_metallic_roughness) {
            const float* factor = source.pbr_metallic_roughness.base_color_factor;
            material.baseColorFactor = {factor[0], factor[1], factor[2], factor[3]};
            if (source.pbr_metallic_roughness.base_color_texture.texture) {
                const auto index = textureIndexOf(
                    textureIndex,
                    source.pbr_metallic_roughness.base_color_texture.texture);
                if (index) {
                    material.baseColorTexture = *index;
                    const auto& view =
                        source.pbr_metallic_roughness.base_color_texture;
                    if (view.has_transform) {
                        material.baseColorUvOffset = {
                            view.transform.offset[0], view.transform.offset[1]};
                        material.baseColorUvScale = {
                            view.transform.scale[0], view.transform.scale[1]};
                        material.baseColorUvRotation = view.transform.rotation;
                        if (std::abs(view.transform.rotation) > 1.0e-5f) {
                            model->warnings.push_back(
                                "material '" + material.name +
                                "' uses a rotated texture transform; rotation is "
                                "approximated as zero");
                        }
                    }
                }
            }
        }
        if (source.emissive_texture.texture) {
            const auto index =
                textureIndexOf(textureIndex, source.emissive_texture.texture);
            if (index) {
                material.emissiveTexture = *index;
            }
        }
        material.emissiveFactor = {source.emissive_factor[0],
                                  source.emissive_factor[1],
                                  source.emissive_factor[2], 1.0f};
        material.doubleSided = source.double_sided != 0;
        material.unlit = source.unlit != 0;
        material.alphaCutoff = source.alpha_cutoff;
        switch (source.alpha_mode) {
        case cgltf_alpha_mode_mask:
            material.alphaMode = AlphaMode::Mask;
            break;
        case cgltf_alpha_mode_blend:
            material.alphaMode = AlphaMode::Blend;
            break;
        case cgltf_alpha_mode_opaque:
        default:
            material.alphaMode = AlphaMode::Opaque;
            break;
        }
    }

    // VRM material properties are outside glTF materials and carry MToon's
    // shade colour/cull mode. Missing entries are not fatal.
    const JsonValue* materialProperties =
        objectField(vrm, "materialProperties");
    if (materialProperties && materialProperties->isArray()) {
        std::unordered_map<std::string, int> byName;
        for (std::size_t i = 0; i < model->materials.size(); ++i) {
            byName.emplace(model->materials[i].name, static_cast<int>(i));
        }
        for (const JsonValue& property : materialProperties->items) {
            const std::string name = stringField(&property, "name");
            const auto found = byName.find(name);
            if (found == byName.end()) {
                continue;
            }
            Material& material = model->materials[static_cast<std::size_t>(found->second)];
            const JsonValue* shader = objectField(&property, "shader");
            if (shader && shader->isString()) {
                material.shader = shader->string;
                material.toon = shader->string == "VRM/MToon";
            }
            const JsonValue* floatProperties =
                objectField(&property, "floatProperties");
            if (const JsonValue* cutoff = objectField(floatProperties, "_Cutoff")) {
                if (cutoff->isNumber() && cutoff->number > 0.0 &&
                    cutoff->number <= 1.0) {
                    material.alphaCutoff = static_cast<float>(cutoff->number);
                }
            }
            if (const JsonValue* cull = objectField(floatProperties, "_CullMode")) {
                if (cull->isNumber() && static_cast<int>(cull->number) == 0) {
                    material.doubleSided = true;
                }
            }
            const JsonValue* vectorProperties =
                objectField(&property, "vectorProperties");
            material.shadeColor =
                vec4Field(vectorProperties, "_ShadeColor", material.shadeColor);
        }
    }

    // ------------------------------------------------------------------
    // Textures: create entries for all glTF textures, decode only the ones
    // actually referenced as base colour maps.
    // ------------------------------------------------------------------
    model->textures.resize(data->textures_count);
    for (std::size_t i = 0; i < data->textures_count; ++i) {
        const cgltf_texture& source = data->textures[i];
        Texture& texture = model->textures[i];
        texture.name = source.name ? source.name : std::string{};
        texture.sourceTexture = static_cast<int>(i);
        if (source.image) {
            const auto it = std::find_if(
                data->images, data->images + data->images_count,
                [&](const cgltf_image& image) { return &image == source.image; });
            texture.imageIndex =
                it != data->images + data->images_count
                    ? static_cast<int>(it - data->images)
                    : -1;
            if (texture.imageIndex < 0) {
                model->warnings.push_back("texture references an invalid image");
            }
        }
        if (source.has_basisu || source.has_webp) {
            model->warnings.push_back(
                "texture uses a compressed container that is not decoded");
        }
    }
    std::unordered_set<int> referencedTextures;
    for (const Material& material : model->materials) {
        if (material.baseColorTexture >= 0) {
            referencedTextures.insert(material.baseColorTexture);
        }
    }
    std::size_t decodedBytes = 0;
    for (int textureIndexValue : referencedTextures) {
        if (textureIndexValue < 0 ||
            static_cast<std::size_t>(textureIndexValue) >= data->textures_count) {
            continue;
        }
        const cgltf_texture& source =
            data->textures[static_cast<std::size_t>(textureIndexValue)];
        Texture& texture = model->textures[static_cast<std::size_t>(textureIndexValue)];
        std::string warning;
        if (decodeTexture(source.image, texture, warning)) {
            if (decodedBytes + texture.rgba.size() > kMaxDecodedBytes) {
                model->warnings.push_back(
                    "decoded texture budget exceeded; using fallback for one map");
                texture.rgba.clear();
                texture.decoded = false;
                continue;
            }
            decodedBytes += texture.rgba.size();
            ++model->diagnostics.decodedTextureCount;
        } else if (!warning.empty()) {
            model->warnings.push_back("texture '" + texture.name + "': " + warning);
        }
    }
    model->diagnostics.estimatedCpuTextureBytes =
        static_cast<int>(std::min<std::size_t>(decodedBytes,
                                              static_cast<std::size_t>(
                                                  std::numeric_limits<int>::max())));

    // ------------------------------------------------------------------
    // Meshes
    // ------------------------------------------------------------------
    if (data->meshes_count > kMaxMeshes) {
        result.error = "VRM mesh count is unreasonable";
        return result;
    }
    model->meshes.resize(data->meshes_count);
    for (std::size_t meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex) {
        const cgltf_mesh& sourceMesh = data->meshes[meshIndex];
        Mesh& mesh = model->meshes[meshIndex];
        mesh.name = sourceMesh.name ? sourceMesh.name : std::string{};
        if (sourceMesh.primitives_count > kMaxPrimitives) {
            result.error = "mesh '" + mesh.name + "' has too many primitives";
            return result;
        }
        mesh.primitives.resize(sourceMesh.primitives_count);
        for (std::size_t primitiveIndex = 0;
             primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex) {
            const cgltf_primitive& source = sourceMesh.primitives[primitiveIndex];
            Primitive& primitive = mesh.primitives[primitiveIndex];
            if (source.type != cgltf_primitive_type_triangles) {
                model->warnings.push_back(
                    "mesh '" + mesh.name +
                    "' has a non-triangle primitive that was skipped");
                continue;
            }
            if (source.material) {
                const auto it = std::find_if(
                    data->materials, data->materials + data->materials_count,
                    [&](const cgltf_material& material) {
                        return &material == source.material;
                    });
                primitive.material =
                    it != data->materials + data->materials_count
                        ? static_cast<int>(it - data->materials)
                        : -1;
                if (primitive.material < 0) {
                    result.error = "primitive references an invalid material";
                    return result;
                }
            }

            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* uvs = nullptr;
            const cgltf_accessor* joints = nullptr;
            const cgltf_accessor* weights = nullptr;
            for (std::size_t a = 0; a < source.attributes_count; ++a) {
                const cgltf_attribute& attribute = source.attributes[a];
                switch (attribute.type) {
                case cgltf_attribute_type_position:
                    positions = attribute.data;
                    break;
                case cgltf_attribute_type_normal:
                    normals = attribute.data;
                    break;
                case cgltf_attribute_type_texcoord:
                    if (attribute.index == 0) {
                        uvs = attribute.data;
                    }
                    break;
                case cgltf_attribute_type_joints:
                    if (attribute.index == 0) {
                        joints = attribute.data;
                    }
                    break;
                case cgltf_attribute_type_weights:
                    if (attribute.index == 0) {
                        weights = attribute.data;
                    }
                    break;
                default:
                    break;
                }
            }
            if (!positions) {
                result.error = "mesh '" + mesh.name + "' primitive has no POSITION";
                return result;
            }
            if (!validateAccessor(positions, "POSITION", cgltf_type_vec3,
                                  {cgltf_component_type_r_32f}, result.error)) {
                return result;
            }
            const std::size_t vertexCount = positions->count;
            if (vertexCount == 0 || vertexCount > kMaxVerticesPerPrimitive) {
                result.error = "mesh '" + mesh.name + "' has an invalid vertex count";
                return result;
            }
            if (normals &&
                !validateAccessor(normals, "NORMAL", cgltf_type_vec3,
                                  {cgltf_component_type_r_32f}, result.error)) {
                return result;
            }
            if (uvs &&
                !validateAccessor(uvs, "TEXCOORD_0", cgltf_type_vec2,
                                  {cgltf_component_type_r_32f,
                                   cgltf_component_type_r_8u,
                                   cgltf_component_type_r_16u},
                                  result.error)) {
                return result;
            }
            if (joints &&
                !validateAccessor(joints, "JOINTS_0", cgltf_type_vec4,
                                  {cgltf_component_type_r_8u,
                                   cgltf_component_type_r_16u},
                                  result.error)) {
                return result;
            }
            if (weights &&
                !validateAccessor(weights, "WEIGHTS_0", cgltf_type_vec4,
                                  {cgltf_component_type_r_32f,
                                   cgltf_component_type_r_8u,
                                   cgltf_component_type_r_16u},
                                  result.error)) {
                return result;
            }
            if (joints && !weights) {
                result.error = "mesh '" + mesh.name +
                               "' has JOINTS_0 without WEIGHTS_0";
                return result;
            }
            const bool hasSkinData = joints != nullptr && weights != nullptr;
            if (hasSkinData && joints->count < vertexCount) {
                result.error = "JOINTS_0 count is smaller than POSITION count";
                return result;
            }
            if (hasSkinData && weights->count < vertexCount) {
                result.error = "WEIGHTS_0 count is smaller than POSITION count";
                return result;
            }
            if (normals && normals->count < vertexCount) {
                result.error = "NORMAL count is smaller than POSITION count";
                return result;
            }
            if (uvs && uvs->count < vertexCount) {
                result.error = "TEXCOORD_0 count is smaller than POSITION count";
                return result;
            }
            primitive.skinned = hasSkinData;
            primitive.material = primitive.material < 0 ? -1 : primitive.material;
            primitive.vertices.resize(vertexCount);
            const Material* material =
                primitive.material >= 0
                    ? &model->materials[static_cast<std::size_t>(primitive.material)]
                    : nullptr;
            const Vec2 uvOffset = material ? material->baseColorUvOffset : Vec2{};
            const Vec2 uvScale = material ? material->baseColorUvScale
                                          : Vec2{1.0f, 1.0f};

            for (std::size_t v = 0; v < vertexCount; ++v) {
                Vertex& vertex = primitive.vertices[v];
                float position[3]{};
                if (!cgltf_accessor_read_float(positions, v, position, 3)) {
                    result.error = "failed to read POSITION data";
                    return result;
                }
                vertex.position = {position[0], position[1], position[2]};
                if (!math::finite(vertex.position)) {
                    result.error = "mesh contains a non-finite vertex position";
                    return result;
                }
                if (normals) {
                    float normal[3]{};
                    if (!cgltf_accessor_read_float(normals, v, normal, 3)) {
                        result.error = "failed to read NORMAL data";
                        return result;
                    }
                    vertex.normal = math::normalize(
                        Vec3{normal[0], normal[1], normal[2]});
                    if (!math::finite(vertex.normal) ||
                        vertex.normal.lengthSquared() < 0.5f) {
                        vertex.normal = {0.0f, 1.0f, 0.0f};
                    }
                }
                if (uvs) {
                    float uv[2]{};
                    if (!cgltf_accessor_read_float(uvs, v, uv, 2)) {
                        result.error = "failed to read TEXCOORD_0 data";
                        return result;
                    }
                    vertex.uv = {uv[0] * uvScale.x + uvOffset.x,
                                 uv[1] * uvScale.y + uvOffset.y};
                    if (!std::isfinite(vertex.uv.x) ||
                        !std::isfinite(vertex.uv.y)) {
                        vertex.uv = {};
                    }
                }
                if (hasSkinData) {
                    cgltf_uint jointValues[4]{};
                    float weightValues[4]{};
                    if (!cgltf_accessor_read_uint(joints, v, jointValues, 4) ||
                        !cgltf_accessor_read_float(weights, v, weightValues, 4)) {
                        result.error = "failed to read skinning data";
                        return result;
                    }
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) {
                        if (jointValues[k] > 0xFFFFu) {
                            result.error = "joint index exceeds 16-bit range";
                            return result;
                        }
                        const float weight = std::isfinite(weightValues[k])
                                                 ? std::max(0.0f, weightValues[k])
                                                 : 0.0f;
                        vertex.joints[static_cast<std::size_t>(k)] =
                            static_cast<std::uint16_t>(jointValues[k]);
                        vertex.weights[static_cast<std::size_t>(k)] = weight;
                        sum += weight;
                    }
                    if (sum > 1.0e-6f) {
                        for (float& weight : vertex.weights) {
                            weight /= sum;
                        }
                    } else {
                        vertex.joints = {0, 0, 0, 0};
                        vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                    }
                } else {
                    vertex.joints = {0, 0, 0, 0};
                    vertex.weights = {1.0f, 0.0f, 0.0f, 0.0f};
                }
            }

            if (source.indices) {
                if (!validateAccessor(source.indices, "indices", cgltf_type_scalar,
                                      {cgltf_component_type_r_8u,
                                       cgltf_component_type_r_16u,
                                       cgltf_component_type_r_32u},
                                      result.error)) {
                    return result;
                }
                const std::size_t indexCount = source.indices->count;
                if (indexCount > kMaxIndicesPerPrimitive) {
                    result.error = "mesh '" + mesh.name + "' has too many indices";
                    return result;
                }
                primitive.indices.resize(indexCount);
                for (std::size_t i = 0; i < indexCount; ++i) {
                    cgltf_uint value = 0;
                    if (!cgltf_accessor_read_uint(source.indices, i, &value, 1)) {
                        result.error = "failed to read an index";
                        return result;
                    }
                    if (value >= vertexCount) {
                        result.error = "mesh index is outside the vertex range";
                        return result;
                    }
                    primitive.indices[i] = static_cast<std::uint32_t>(value);
                }
            } else {
                if (vertexCount > kMaxIndicesPerPrimitive) {
                    result.error = "mesh '" + mesh.name + "' is too large";
                    return result;
                }
                primitive.indices.resize(vertexCount);
                for (std::size_t i = 0; i < vertexCount; ++i) {
                    primitive.indices[i] = static_cast<std::uint32_t>(i);
                }
            }
            if (primitive.indices.size() % 3 != 0) {
                model->warnings.push_back(
                    "mesh '" + mesh.name +
                    "' index count is not a multiple of three; trailing indices "
                    "were dropped");
                primitive.indices.resize(primitive.indices.size() /
                                        3 * 3);
            }
            if (primitive.indices.empty()) {
                model->warnings.push_back("mesh '" + mesh.name +
                                          "' primitive is empty");
            }
            if (!normals) {
                // Reconstruct smooth vertex normals. This makes the renderer
                // robust to glTF files that omit NORMAL without inventing a
                // wrong normal.
                for (std::size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
                    const std::uint32_t ia = primitive.indices[i];
                    const std::uint32_t ib = primitive.indices[i + 1];
                    const std::uint32_t ic = primitive.indices[i + 2];
                    const Vec3 a = primitive.vertices[ia].position;
                    const Vec3 b = primitive.vertices[ib].position;
                    const Vec3 c = primitive.vertices[ic].position;
                    const Vec3 n = math::cross(b - a, c - a);
                    primitive.vertices[ia].normal += n;
                    primitive.vertices[ib].normal += n;
                    primitive.vertices[ic].normal += n;
                }
                for (Vertex& vertex : primitive.vertices) {
                    vertex.normal = math::normalize(vertex.normal);
                    if (vertex.normal.lengthSquared() < 0.5f) {
                        vertex.normal = {0.0f, 1.0f, 0.0f};
                    }
                }
            }
            model->diagnostics.primitiveCount += 1;
            model->diagnostics.triangleCount +=
                static_cast<int>(primitive.indices.size() / 3);
            model->diagnostics.vertexCount += static_cast<int>(vertexCount);
        }
    }

    // Mesh instances and bounds.
    for (std::size_t i = 0; i < model->nodes.size(); ++i) {
        if (model->nodes[i].mesh >= 0) {
            if (static_cast<std::size_t>(model->nodes[i].mesh) >= model->meshes.size()) {
                result.error = "node references a mesh outside the mesh table";
                return result;
            }
            model->meshInstances.push_back(
                MeshInstance{static_cast<int>(i), model->nodes[i].mesh});
        }
    }
    if (model->meshInstances.empty()) {
        result.error = "VRM contains no renderable mesh instance";
        return result;
    }
    for (const MeshInstance& instance : model->meshInstances) {
        const Node& node = model->nodes[static_cast<std::size_t>(instance.node)];
        const Mesh& mesh = model->meshes[static_cast<std::size_t>(instance.mesh)];
        const Mat4 nodeTransform =
            model->bindGlobals[static_cast<std::size_t>(instance.node)];
        const Skin* skin = node.skin >= 0
                               ? &model->skins[static_cast<std::size_t>(node.skin)]
                               : nullptr;
        for (const Primitive& primitive : mesh.primitives) {
            const bool appliesSkin = primitive.skinned && skin;
            for (const Vertex& vertex : primitive.vertices) {
                const Vec3 position = appliesSkin
                                          ? vertex.position
                                          : nodeTransform.transformPoint(vertex.position);
                model->bounds.include(position);
            }
            if (appliesSkin) {
                for (const Vertex& vertex : primitive.vertices) {
                    for (std::size_t k = 0; k < 4; ++k) {
                        if (vertex.weights[k] > 0.0f &&
                            vertex.joints[k] >= skin->joints.size()) {
                            result.error = "skin weight references a joint outside "
                                           "the skin";
                            return result;
                        }
                    }
                }
            }
        }
    }
    if (!model->bounds.valid) {
        result.error = "VRM has no finite geometry bounds";
        return result;
    }
    model->height = std::max(0.05f, model->bounds.size().y);
    model->up = {0.0f, 1.0f, 0.0f};

    const int headNode = model->nodeFor(HumanoidBone::Head);
    const int leftEyeNode = model->nodeFor(HumanoidBone::LeftEye);
    const int rightEyeNode = model->nodeFor(HumanoidBone::RightEye);
    if (headNode >= 0 && leftEyeNode >= 0 && rightEyeNode >= 0) {
        const Vec3 head =
            model->bindGlobals[static_cast<std::size_t>(headNode)].translationPart();
        const Vec3 left =
            model->bindGlobals[static_cast<std::size_t>(leftEyeNode)].translationPart();
        const Vec3 right =
            model->bindGlobals[static_cast<std::size_t>(rightEyeNode)].translationPart();
        Vec3 front = (left + right) * 0.5f - head;
        front.y = 0.0f;
        front = math::normalize(front);
        if (front.lengthSquared() > 0.1f) {
            model->forward = front;
        } else {
            model->forward = {0.0f, 0.0f, 1.0f};
        }
    } else {
        model->forward = {0.0f, 0.0f, 1.0f};
    }
    model->right = math::normalize(math::cross(model->forward, model->up));
    if (model->right.lengthSquared() < 0.1f) {
        model->right = {1.0f, 0.0f, 0.0f};
    }
    model->diagnostics.nodeCount = static_cast<int>(model->nodes.size());
    model->diagnostics.sceneRootCount = static_cast<int>(model->sceneRoots.size());
    model->diagnostics.meshCount = static_cast<int>(model->meshes.size());
    model->diagnostics.materialCount = static_cast<int>(model->materials.size());
    model->diagnostics.textureCount = static_cast<int>(model->textures.size());
    model->diagnostics.skinCount = static_cast<int>(model->skins.size());
    model->diagnostics.loadMilliseconds =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    result.warnings = model->warnings;
    result.model = std::move(model);
    return result;
}

} // namespace bgmedia::vrm
