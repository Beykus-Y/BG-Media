#include "TextRenderer.hpp"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bgmedia::desktop {
namespace {

constexpr std::uint32_t kReplacementCodepoint = '?';

std::uint32_t decodeUtf8(std::string_view text, std::size_t& index) {
    if (index >= text.size()) {
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(text[index]);
    if (b0 < 0x80) {
        ++index;
        return b0;
    }
    int length = 0;
    std::uint32_t codepoint = 0;
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        codepoint = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        codepoint = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        codepoint = b0 & 0x07;
    } else {
        ++index;
        return kReplacementCodepoint;
    }
    if (index + static_cast<std::size_t>(length) > text.size()) {
        index = text.size();
        return kReplacementCodepoint;
    }
    for (int i = 1; i < length; ++i) {
        const auto b = static_cast<unsigned char>(text[index + static_cast<std::size_t>(i)]);
        if ((b & 0xC0) != 0x80) {
            index += static_cast<std::size_t>(i);
            return kReplacementCodepoint;
        }
        codepoint = (codepoint << 6) | (b & 0x3F);
    }
    index += static_cast<std::size_t>(length);
    return codepoint;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 32 * 1024 * 1024) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        return {};
    }
    return data;
}

std::vector<std::uint32_t> codepoints() {
    std::vector<std::uint32_t> values;
    for (std::uint32_t cp = 32; cp <= 126; ++cp) {
        values.push_back(cp);
    }
    values.push_back(0x00A0); // no-break space
    values.push_back(0x00B0); // degree
    values.push_back(0x00B1); // plus-minus
    values.push_back(0x00B7); // middle dot
    values.push_back(0x00D7); // multiplication
    values.push_back(0x2013); // en dash
    values.push_back(0x2014); // em dash
    values.push_back(0x2018);
    values.push_back(0x2019);
    values.push_back(0x201C);
    values.push_back(0x201D);
    values.push_back(0x2022); // bullet
    values.push_back(0x2026); // ellipsis
    values.push_back(0x2190); // arrows
    values.push_back(0x2191);
    values.push_back(0x2192);
    values.push_back(0x2193);
    values.push_back(0x2212); // minus
    values.push_back(0x266A); // eighth note
    values.push_back(0x0401);
    for (std::uint32_t cp = 0x0410; cp <= 0x044F; ++cp) {
        values.push_back(cp);
    }
    values.push_back(0x0451);
    return values;
}

} // namespace

struct TextRenderer::Impl {
    struct Glyph {
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float bearingX = 0.0f;
        float bearingY = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float advance = 0.0f;
    };

    struct Face {
        SDL_Texture* texture = nullptr;
        int atlasWidth = 0;
        int atlasHeight = 0;
        float ascent = 0.0f;
        float descent = 0.0f;
        float lineHeight = 0.0f;
        float inverseScale = 1.0f;
        std::unordered_map<std::uint32_t, Glyph> glyphs;
    };

    SDL_Renderer* renderer = nullptr;
    float scale = 1.0f;
    std::vector<std::uint8_t> regularData;
    std::vector<std::uint8_t> semiboldData;
    stbtt_fontinfo regular{};
    stbtt_fontinfo semibold{};
    bool regularReady = false;
    bool semiboldReady = false;
    std::unordered_map<int, Face> faces;

    ~Impl() {
        for (auto& [key, face] : faces) {
            (void)key;
            if (face.texture) {
                SDL_DestroyTexture(face.texture);
            }
        }
    }

    [[nodiscard]] const stbtt_fontinfo* font(FontWeight weight) const {
        if (weight == FontWeight::SemiBold && semiboldReady) {
            return &semibold;
        }
        return regularReady ? &regular : nullptr;
    }

    [[nodiscard]] Face* face(int size, FontWeight weight) {
        size = std::clamp(size, 8, 96);
        const int bakedSize = std::clamp(
            static_cast<int>(std::lround(static_cast<float>(size) * scale)), 8,
            192);
        const int key = bakedSize * 2 + (weight == FontWeight::SemiBold ? 1 : 0);
        auto it = faces.find(key);
        if (it != faces.end()) {
            return &it->second;
        }

        const stbtt_fontinfo* info = font(weight);
        if (!info) {
            return nullptr;
        }

        Face baked;
        baked.inverseScale = 1.0f / std::max(0.01f, scale);
        baked.ascent = static_cast<float>(bakedSize) * 0.80f;
        const float fontScale = stbtt_ScaleForPixelHeight(info, static_cast<float>(bakedSize));
        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        stbtt_GetFontVMetrics(info, &ascent, &descent, &lineGap);
        baked.ascent = static_cast<float>(ascent) * fontScale;
        baked.descent = static_cast<float>(descent) * fontScale;
        baked.lineHeight = static_cast<float>(ascent - descent + lineGap) * fontScale;

        const int atlasWidth = bakedSize >= 64 ? 4096 : (bakedSize >= 44 ? 2048 : 1024);
        const int atlasHeight = bakedSize >= 64 ? 4096 : (bakedSize >= 44 ? 2048 : 1024);
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(atlasWidth) * atlasHeight * 4, 0);

        struct Packed {
            std::uint32_t cp;
            int x;
            int y;
            int w;
            int h;
            int x0;
            int y0;
            float advance;
        };
        std::vector<Packed> packed;
        packed.reserve(160);

        int penX = 2;
        int penY = 2;
        int rowHeight = 0;

        for (std::uint32_t cp : codepoints()) {
            int glyphIndex = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
            std::uint32_t useCp = cp;
            if (glyphIndex == 0 && cp != ' ' && cp != 0x00A0) {
                useCp = kReplacementCodepoint;
                glyphIndex = stbtt_FindGlyphIndex(info, static_cast<int>(useCp));
                if (glyphIndex == 0) {
                    continue;
                }
            }

            int advanceRaw = 0;
            int bearing = 0;
            stbtt_GetCodepointHMetrics(info, static_cast<int>(useCp), &advanceRaw, &bearing);
            const float advance = static_cast<float>(advanceRaw) * fontScale;

            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetCodepointBitmapBox(info, static_cast<int>(useCp), fontScale, fontScale,
                                        &x0, &y0, &x1, &y1);
            const int w = std::max(0, x1 - x0);
            const int h = std::max(0, y1 - y0);
            if (w == 0 || h == 0) {
                Glyph glyph;
                glyph.advance = advance;
                baked.glyphs[cp] = glyph;
                continue;
            }

            if (penX + w + 2 > atlasWidth) {
                penX = 2;
                penY += rowHeight + 2;
                rowHeight = 0;
            }
            if (penY + h + 2 > atlasHeight) {
                // This is only reachable for unusually verbose fonts; skip
                // additional glyphs rather than corrupting the atlas.
                continue;
            }

            std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(w) * h);
            stbtt_MakeCodepointBitmap(info, bitmap.data(), w, h, w, fontScale, fontScale,
                                      static_cast<int>(useCp));
            for (int gy = 0; gy < h; ++gy) {
                for (int gx = 0; gx < w; ++gx) {
                    const std::uint8_t alpha =
                        bitmap[static_cast<std::size_t>(gy) * w + gx];
                    const std::size_t at =
                        (static_cast<std::size_t>(penY + gy) * atlasWidth +
                         static_cast<std::size_t>(penX + gx)) *
                        4;
                    pixels[at] = 255;
                    pixels[at + 1] = 255;
                    pixels[at + 2] = 255;
                    pixels[at + 3] = alpha;
                }
            }

            packed.push_back({cp, penX, penY, w, h, x0, y0, advance});
            penX += w + 2;
            rowHeight = std::max(rowHeight, h);
        }

        int usedHeight = std::max(16, penY + rowHeight + 2);
        usedHeight = std::min(atlasHeight, usedHeight);
        baked.atlasWidth = atlasWidth;
        baked.atlasHeight = usedHeight;

        baked.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC, atlasWidth,
                                          usedHeight);
        if (!baked.texture) {
            return nullptr;
        }
        SDL_SetTextureBlendMode(baked.texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(baked.texture, SDL_SCALEMODE_LINEAR);
        SDL_UpdateTexture(baked.texture, nullptr, pixels.data(), atlasWidth * 4);

        for (const Packed& item : packed) {
            Glyph glyph;
            glyph.u0 = static_cast<float>(item.x) / atlasWidth;
            glyph.v0 = static_cast<float>(item.y) / usedHeight;
            glyph.u1 = static_cast<float>(item.x + item.w) / atlasWidth;
            glyph.v1 = static_cast<float>(item.y + item.h) / usedHeight;
            glyph.bearingX = static_cast<float>(item.x0);
            glyph.bearingY = static_cast<float>(item.y0);
            glyph.width = static_cast<float>(item.w);
            glyph.height = static_cast<float>(item.h);
            glyph.advance = item.advance;
            baked.glyphs[item.cp] = glyph;
        }

        auto [inserted, ok] = faces.emplace(key, std::move(baked));
        return ok ? &inserted->second : nullptr;
    }

    [[nodiscard]] const Glyph* glyph(const Face& face, std::uint32_t cp) const {
        auto it = face.glyphs.find(cp);
        if (it != face.glyphs.end()) {
            return &it->second;
        }
        it = face.glyphs.find(kReplacementCodepoint);
        return it != face.glyphs.end() ? &it->second : nullptr;
    }

    [[nodiscard]] const Face* faceRead(int size, FontWeight weight) const {
        size = std::clamp(size, 8, 96);
        const int bakedSize = std::clamp(
            static_cast<int>(std::lround(static_cast<float>(size) * scale)), 8,
            192);
        const int key = bakedSize * 2 + (weight == FontWeight::SemiBold ? 1 : 0);
        auto it = faces.find(key);
        return it != faces.end() ? &it->second : nullptr;
    }
};

TextRenderer::TextRenderer() : impl_(std::make_unique<Impl>()) {}

TextRenderer::~TextRenderer() = default;

bool TextRenderer::init(SDL_Renderer* renderer,
                        const std::filesystem::path& assetsDirectory,
                        std::string* error) {
    impl_->renderer = renderer;
    const std::filesystem::path regular = assetsDirectory / "fonts" / "Inter-Regular.ttf";
    const std::filesystem::path semibold = assetsDirectory / "fonts" / "Inter-SemiBold.ttf";
    impl_->regularData = readFile(regular);
    impl_->semiboldData = readFile(semibold);
    if (impl_->regularData.empty()) {
        if (error) {
            *error = "cannot read UI font: " + regular.string();
        }
        return false;
    }
    if (!stbtt_InitFont(&impl_->regular, impl_->regularData.data(),
                        stbtt_GetFontOffsetForIndex(impl_->regularData.data(), 0))) {
        if (error) {
            *error = "invalid UI font: " + regular.string();
        }
        return false;
    }
    impl_->regularReady = true;

    if (!impl_->semiboldData.empty() &&
        stbtt_InitFont(&impl_->semibold, impl_->semiboldData.data(),
                       stbtt_GetFontOffsetForIndex(impl_->semiboldData.data(), 0))) {
        impl_->semiboldReady = true;
    }
    return true;
}

void TextRenderer::draw(std::string_view text, float x, float y, int size,
                        FontWeight weight, SDL_Color color, float alpha,
                        float letterSpacing) {
    Impl::Face* face = impl_->face(size, weight);
    if (!face || !face->texture) {
        return;
    }
    SDL_SetTextureColorMod(face->texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(face->texture, static_cast<Uint8>(
        std::clamp(alpha * static_cast<float>(color.a) / 255.0f, 0.0f, 1.0f) * 255.0f));

    const float baseline = y + face->ascent * face->inverseScale;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::uint32_t cp = decodeUtf8(text, index);
        const Impl::Glyph* glyph = impl_->glyph(*face, cp);
        if (!glyph) {
            continue;
        }
        if (glyph->width > 0.0f && glyph->height > 0.0f) {
            const SDL_FRect src{glyph->u0 * face->atlasWidth,
                                glyph->v0 * face->atlasHeight,
                                glyph->width, glyph->height};
            const SDL_FRect dst{
                x + glyph->bearingX * face->inverseScale,
                baseline + glyph->bearingY * face->inverseScale,
                glyph->width * face->inverseScale,
                glyph->height * face->inverseScale};
            SDL_RenderTexture(impl_->renderer, face->texture, &src, &dst);
        }
        x += glyph->advance * face->inverseScale + letterSpacing;
    }
}

void TextRenderer::drawRight(std::string_view text, float rightX, float y, int size,
                             FontWeight weight, SDL_Color color, float alpha,
                             float letterSpacing) {
    draw(text, rightX - measure(text, size, weight, letterSpacing), y, size, weight,
         color, alpha, letterSpacing);
}

void TextRenderer::drawCentered(std::string_view text, float centerX, float y, int size,
                                FontWeight weight, SDL_Color color, float alpha,
                                float letterSpacing) {
    draw(text,
         centerX - measure(text, size, weight, letterSpacing) * 0.5f, y, size, weight,
         color, alpha, letterSpacing);
}

float TextRenderer::measure(std::string_view text, int size, FontWeight weight,
                            float letterSpacing) const {
    const Impl::Face* face = impl_->face(size, weight);
    if (!face) {
        return 0.0f;
    }
    float width = 0.0f;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::uint32_t cp = decodeUtf8(text, index);
        const Impl::Glyph* glyph = impl_->glyph(*face, cp);
        if (!glyph) {
            continue;
        }
        width += glyph->advance * face->inverseScale + letterSpacing;
    }
    return width;
}

float TextRenderer::lineHeight(int size, FontWeight weight) const {
    const Impl::Face* face = impl_->faceRead(size, weight);
    return face ? face->lineHeight * face->inverseScale
                : static_cast<float>(size) * 1.2f;
}

float TextRenderer::ascent(int size, FontWeight weight) const {
    const Impl::Face* face = impl_->faceRead(size, weight);
    return face ? face->ascent * face->inverseScale
                : static_cast<float>(size) * 0.8f;
}

void TextRenderer::setScale(float scale) {
    impl_->scale = std::clamp(scale, 0.75f, 3.0f);
}

float TextRenderer::scale() const { return impl_->scale; }

} // namespace bgmedia::desktop


