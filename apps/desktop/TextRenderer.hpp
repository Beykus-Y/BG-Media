#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace bgmedia::desktop {

enum class FontWeight {
    Regular,
    SemiBold
};

// Small runtime font engine built on stb_truetype. It bakes ASCII, Cyrillic and
// the few typographic symbols used by the UI into alpha atlases, so the
// application needs no SDL_ttf/FreeType dependency and displays file names in
// both Latin and Cyrillic correctly.
class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    [[nodiscard]] bool init(SDL_Renderer* renderer,
                            const std::filesystem::path& assetsDirectory,
                            std::string* error = nullptr);

    void draw(std::string_view text, float x, float y, int size,
              FontWeight weight, SDL_Color color, float alpha = 1.0f,
              float letterSpacing = 0.0f);

    void drawRight(std::string_view text, float rightX, float y, int size,
                   FontWeight weight, SDL_Color color, float alpha = 1.0f,
                   float letterSpacing = 0.0f);

    void drawCentered(std::string_view text, float centerX, float y, int size,
                      FontWeight weight, SDL_Color color, float alpha = 1.0f,
                      float letterSpacing = 0.0f);

    [[nodiscard]] float measure(std::string_view text, int size,
                                FontWeight weight,
                                float letterSpacing = 0.0f) const;

    [[nodiscard]] float lineHeight(int size, FontWeight weight) const;
    [[nodiscard]] float ascent(int size, FontWeight weight) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bgmedia::desktop
