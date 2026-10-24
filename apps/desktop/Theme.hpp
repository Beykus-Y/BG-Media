#pragma once

#include <SDL3/SDL.h>

#include <algorithm>

namespace bgmedia::desktop::theme {

inline constexpr SDL_Color background{7, 9, 14, 255};
inline constexpr SDL_Color backgroundSoft{10, 13, 19, 255};
inline constexpr SDL_Color panel{13, 17, 25, 255};
inline constexpr SDL_Color panelRaised{17, 22, 32, 255};
inline constexpr SDL_Color panelHover{22, 28, 40, 255};
inline constexpr SDL_Color border{31, 40, 55, 255};
inline constexpr SDL_Color borderBright{48, 62, 84, 255};
inline constexpr SDL_Color text{232, 238, 248, 255};
inline constexpr SDL_Color muted{141, 154, 176, 255};
inline constexpr SDL_Color dim{87, 101, 122, 255};
inline constexpr SDL_Color accent{83, 231, 196, 255};
inline constexpr SDL_Color accentSoft{29, 82, 74, 255};
inline constexpr SDL_Color violet{142, 131, 255, 255};
inline constexpr SDL_Color pink{255, 91, 157, 255};
inline constexpr SDL_Color amber{255, 183, 77, 255};
inline constexpr SDL_Color red{255, 94, 105, 255};
inline constexpr SDL_Color white{255, 255, 255, 255};

inline constexpr float canvasWidth = 1440.0f;
inline constexpr float canvasHeight = 900.0f;

inline SDL_Color alpha(SDL_Color color, float a) {
    color.a = static_cast<Uint8>(a * 255.0f);
    return color;
}

inline SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return SDL_Color{
        static_cast<Uint8>(a.r + (b.r - a.r) * t),
        static_cast<Uint8>(a.g + (b.g - a.g) * t),
        static_cast<Uint8>(a.b + (b.b - a.b) * t),
        static_cast<Uint8>(a.a + (b.a - a.a) * t),
    };
}

} // namespace bgmedia::desktop::theme
