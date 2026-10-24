#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace bgmedia::desktop::ui {

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }

    [[nodiscard]] float right() const { return x + w; }
    [[nodiscard]] float bottom() const { return y + h; }
    [[nodiscard]] float centerX() const { return x + w * 0.5f; }
    [[nodiscard]] float centerY() const { return y + h * 0.5f; }
    [[nodiscard]] Rect inset(float amount) const {
        return {x + amount, y + amount, w - amount * 2.0f, h - amount * 2.0f};
    }
};

[[nodiscard]] inline SDL_FRect fr(const Rect& rect) {
    return SDL_FRect{rect.x, rect.y, rect.w, rect.h};
}

void fillRect(SDL_Renderer* renderer, const Rect& rect, SDL_Color color);
void strokeRect(SDL_Renderer* renderer, const Rect& rect, SDL_Color color,
                float width = 1.0f);
void fillRoundedRect(SDL_Renderer* renderer, const Rect& rect, float radius,
                     SDL_Color color);
void strokeRoundedRect(SDL_Renderer* renderer, const Rect& rect, float radius,
                       SDL_Color color, float width = 1.0f);
void fillCircle(SDL_Renderer* renderer, float cx, float cy, float radius,
                SDL_Color color);
void ring(SDL_Renderer* renderer, float cx, float cy, float radius,
          float width, SDL_Color color);
void thickLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
               float width, SDL_Color color, bool roundCaps = true);
void triangle(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
              float x3, float y3, SDL_Color color);
void diamond(SDL_Renderer* renderer, float cx, float cy, float radius,
             SDL_Color color);
void dashedLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
                float dash, float gap, SDL_Color color);
void verticalGradient(SDL_Renderer* renderer, const Rect& rect, SDL_Color top,
                      SDL_Color bottom);
void drawCheck(SDL_Renderer* renderer, float cx, float cy, float size,
               SDL_Color color);

} // namespace bgmedia::desktop::ui
