#include "Ui.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bgmedia::desktop::ui {
namespace {

SDL_FColor fc(SDL_Color color) {
    return SDL_FColor{color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                      color.a / 255.0f};
}

void setColor(SDL_Renderer* renderer, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void appendQuad(std::vector<SDL_Vertex>& vertices, std::vector<int>& indices,
                const SDL_FPoint p0, const SDL_FPoint p1, const SDL_FPoint p2,
                const SDL_FPoint p3, SDL_Color color) {
    const int base = static_cast<int>(vertices.size());
    const SDL_FColor c = fc(color);
    vertices.push_back({p0, c, SDL_FPoint{0, 0}});
    vertices.push_back({p1, c, SDL_FPoint{0, 0}});
    vertices.push_back({p2, c, SDL_FPoint{0, 0}});
    vertices.push_back({p3, c, SDL_FPoint{0, 0}});
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 2);
    indices.push_back(base + 1);
    indices.push_back(base + 3);
}

} // namespace

void fillRect(SDL_Renderer* renderer, const Rect& rect, SDL_Color color) {
    setColor(renderer, color);
    const SDL_FRect r = fr(rect);
    SDL_RenderFillRect(renderer, &r);
}

void strokeRect(SDL_Renderer* renderer, const Rect& rect, SDL_Color color,
                float width) {
    setColor(renderer, color);
    const SDL_FRect r = fr(rect);
    for (float i = 0.0f; i < width; i += 1.0f) {
        SDL_FRect rr = r;
        rr.x -= i;
        rr.y -= i;
        rr.w += i * 2.0f;
        rr.h += i * 2.0f;
        SDL_RenderRect(renderer, &rr);
    }
}

void fillRoundedRect(SDL_Renderer* renderer, const Rect& rect, float radius,
                     SDL_Color color) {
    radius = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (radius <= 0.5f) {
        fillRect(renderer, rect, color);
        return;
    }

    constexpr int kSegments = 6;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(4 * (kSegments + 1) + 4);
    indices.reserve(3 * 4 * kSegments * 2);

    const SDL_FColor c = fc(color);
    auto addVertex = [&](float x, float y) {
        vertices.push_back({SDL_FPoint{x, y}, c, SDL_FPoint{0, 0}});
        return static_cast<int>(vertices.size() - 1);
    };

    const int center = addVertex(rect.centerX(), rect.centerY());
    const float cornersX[4] = {rect.x + radius, rect.right() - radius,
                               rect.right() - radius, rect.x + radius};
    const float cornersY[4] = {rect.y + radius, rect.y + radius,
                               rect.bottom() - radius, rect.bottom() - radius};
    const float startAngles[4] = {3.14159265f, -1.57079633f, 0.0f, 1.57079633f};

    std::vector<int> perimeter;
    perimeter.reserve(4 * (kSegments + 1));
    for (int corner = 0; corner < 4; ++corner) {
        for (int s = 0; s <= kSegments; ++s) {
            const float angle =
                startAngles[corner] + (3.14159265f * 0.5f) *
                                          static_cast<float>(s) / kSegments;
            const int index = addVertex(cornersX[corner] + std::cos(angle) * radius,
                                        cornersY[corner] + std::sin(angle) * radius);
            if (perimeter.empty() || perimeter.back() != index) {
                perimeter.push_back(index);
            }
        }
    }
    if (perimeter.front() == perimeter.back()) {
        perimeter.pop_back();
    }
    for (std::size_t i = 0; i < perimeter.size(); ++i) {
        indices.push_back(center);
        indices.push_back(perimeter[i]);
        indices.push_back(perimeter[(i + 1) % perimeter.size()]);
    }
    SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}

void strokeRoundedRect(SDL_Renderer* renderer, const Rect& rect, float radius,
                       SDL_Color color, float width) {
    radius = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (radius <= 0.5f) {
        strokeRect(renderer, rect, color, width);
        return;
    }
    constexpr int kSegments = 8;
    const float cx[4] = {rect.x + radius, rect.right() - radius,
                         rect.right() - radius, rect.x + radius};
    const float cy[4] = {rect.y + radius, rect.y + radius,
                         rect.bottom() - radius, rect.bottom() - radius};
    const float start[4] = {3.14159265f, -1.57079633f, 0.0f, 1.57079633f};

    if (width > 1.5f) {
        std::vector<SDL_FPoint> points;
        points.reserve(4 * (kSegments + 1) + 1);
        for (int corner = 0; corner < 4; ++corner) {
            for (int s = 0; s <= kSegments; ++s) {
                const float angle = start[corner] +
                                    (3.14159265f * 0.5f) * static_cast<float>(s) /
                                        kSegments;
                points.push_back(SDL_FPoint{cx[corner] + std::cos(angle) * radius,
                                            cy[corner] + std::sin(angle) * radius});
            }
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            const SDL_FPoint& a = points[i];
            const SDL_FPoint& b = points[(i + 1) % points.size()];
            thickLine(renderer, a.x, a.y, b.x, b.y, width, color, false);
        }
        return;
    }

    setColor(renderer, color);
    std::vector<SDL_FPoint> points;
    points.reserve(4 * (kSegments + 1) + 1);
    for (int corner = 0; corner < 4; ++corner) {
        for (int s = 0; s <= kSegments; ++s) {
            const float angle = start[corner] +
                                (3.14159265f * 0.5f) * static_cast<float>(s) /
                                    kSegments;
            points.push_back(SDL_FPoint{cx[corner] + std::cos(angle) * radius,
                                        cy[corner] + std::sin(angle) * radius});
        }
    }
    points.push_back(points.front());
    SDL_RenderLines(renderer, points.data(), static_cast<int>(points.size()));
}

void fillCircle(SDL_Renderer* renderer, float cx, float cy, float radius,
                SDL_Color color) {
    if (radius <= 0.0f) {
        return;
    }
    constexpr int kSegments = 28;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(kSegments + 1);
    indices.reserve(kSegments * 3);
    const SDL_FColor c = fc(color);
    vertices.push_back({SDL_FPoint{cx, cy}, c, SDL_FPoint{0, 0}});
    for (int i = 0; i < kSegments; ++i) {
        const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / kSegments;
        vertices.push_back({SDL_FPoint{cx + std::cos(angle) * radius,
                                       cy + std::sin(angle) * radius},
                            c, SDL_FPoint{0, 0}});
    }
    for (int i = 0; i < kSegments; ++i) {
        indices.push_back(0);
        indices.push_back(1 + i);
        indices.push_back(1 + ((i + 1) % kSegments));
    }
    SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                       static_cast<int>(vertices.size()), indices.data(),
                       static_cast<int>(indices.size()));
}

void ring(SDL_Renderer* renderer, float cx, float cy, float radius, float width,
          SDL_Color color) {
    constexpr int kSegments = 64;
    setColor(renderer, color);
    std::vector<SDL_FPoint> points;
    points.reserve(kSegments + 1);
    for (int i = 0; i <= kSegments; ++i) {
        const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / kSegments;
        points.push_back(SDL_FPoint{cx + std::cos(angle) * radius,
                                    cy + std::sin(angle) * radius});
    }
    for (int i = 0; i < kSegments; ++i) {
        thickLine(renderer, points[static_cast<std::size_t>(i)].x,
                  points[static_cast<std::size_t>(i)].y,
                  points[static_cast<std::size_t>(i + 1)].x,
                  points[static_cast<std::size_t>(i + 1)].y, width, color, false);
    }
}

void thickLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
               float width, SDL_Color color, bool roundCaps) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.001f) {
        fillCircle(renderer, x1, y1, width * 0.5f, color);
        return;
    }
    const float nx = -dy / length * width * 0.5f;
    const float ny = dx / length * width * 0.5f;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    const SDL_FColor c = fc(color);
    vertices.push_back({SDL_FPoint{x1 + nx, y1 + ny}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{x2 + nx, y2 + ny}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{x2 - nx, y2 - ny}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{x1 - nx, y1 - ny}, c, SDL_FPoint{0, 0}});
    indices = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), 4, indices.data(), 6);
    if (roundCaps) {
        fillCircle(renderer, x1, y1, width * 0.5f, color);
        fillCircle(renderer, x2, y2, width * 0.5f, color);
    }
}

void triangle(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
              float x3, float y3, SDL_Color color) {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    const SDL_FColor c = fc(color);
    vertices.push_back({SDL_FPoint{x1, y1}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{x2, y2}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{x3, y3}, c, SDL_FPoint{0, 0}});
    indices = {0, 1, 2};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), 3, indices.data(), 3);
}

void diamond(SDL_Renderer* renderer, float cx, float cy, float radius,
             SDL_Color color) {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    const SDL_FColor c = fc(color);
    vertices.push_back({SDL_FPoint{cx, cy - radius}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{cx + radius, cy}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{cx, cy + radius}, c, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{cx - radius, cy}, c, SDL_FPoint{0, 0}});
    indices = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), 4, indices.data(), 6);
}

void dashedLine(SDL_Renderer* renderer, float x1, float y1, float x2, float y2,
                float dash, float gap, SDL_Color color) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.01f) {
        return;
    }
    const float ux = dx / length;
    const float uy = dy / length;
    setColor(renderer, color);
    for (float at = 0.0f; at < length; at += dash + gap) {
        const float end = std::min(length, at + dash);
        SDL_RenderLine(renderer, x1 + ux * at, y1 + uy * at, x1 + ux * end,
                       y1 + uy * end);
    }
}

void verticalGradient(SDL_Renderer* renderer, const Rect& rect, SDL_Color top,
                      SDL_Color bottom) {
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    const SDL_FColor c0 = fc(top);
    const SDL_FColor c1 = fc(bottom);
    vertices.push_back({SDL_FPoint{rect.x, rect.y}, c0, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{rect.right(), rect.y}, c0, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{rect.right(), rect.bottom()}, c1, SDL_FPoint{0, 0}});
    vertices.push_back({SDL_FPoint{rect.x, rect.bottom()}, c1, SDL_FPoint{0, 0}});
    indices = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), 4, indices.data(), 6);
}

void drawCheck(SDL_Renderer* renderer, float cx, float cy, float size,
               SDL_Color color) {
    thickLine(renderer, cx - size, cy, cx - size * 0.15f, cy + size * 0.7f,
              std::max(1.5f, size * 0.22f), color);
    thickLine(renderer, cx - size * 0.15f, cy + size * 0.7f, cx + size, cy - size * 0.7f,
              std::max(1.5f, size * 0.22f), color);
}

} // namespace bgmedia::desktop::ui
