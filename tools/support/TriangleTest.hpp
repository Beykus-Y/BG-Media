#pragma once

// Triangle-triangle contact test for the offline QA tools (header-only, stateless).
//
// Intersection is the interval test of Moller, "A Fast Triangle-Triangle Intersection
// Test" (1997). The coplanar case is reduced to a 2D test over the projection that drops
// the largest normal component. Semantics are CLOSED: touching counts as contact (a shared
// vertex or edge, a point on a face, coplanar triangles that only share an edge). A vertex
// within `epsilon` (metres) of the other triangle's plane counts as on that plane, so a near
// miss smaller than epsilon is reported as touching. Zero-area triangles never intersect.
//
// Depth is approximate and zero for contact without interpenetration. For a face normal n
// of one triangle, the overshoot of the other triangle is the smaller of the largest
// distance its vertices lie behind the plane on the +n side and on the -n side: translating
// the triangle that far along n puts it on one side of the plane. The pair depth is the
// smaller of the two face-normal overshoots, so it is an upper bound of the minimum
// separating translation. It is exact for a flat face pushed straight through another face,
// and it is zero for any pair that shares an edge.

#include "bgmedia/math/Math.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::probe {

// Default plane tolerance in metres (1 micrometre).
inline constexpr float kTriangleEpsilon = 1.0e-6f;

struct TriangleContact {
    bool intersects = false;  // closed test: touching counts
    float depth = 0.0f;       // approximate penetration depth in metres (0 unless overlapping)
};

namespace detail {

struct Point2 {
    float x = 0.0f;
    float y = 0.0f;
};

// Sine of the angle at the first vertex below which a triangle counts as degenerate.
inline constexpr float kDegenerateSine = 1.0e-5f;

// Unit face normal; false when the vertices are collinear or repeated.
inline bool unitNormal(const math::Vec3* t, math::Vec3& normal) {
    const math::Vec3 e1 = t[1] - t[0];
    const math::Vec3 e2 = t[2] - t[0];
    const math::Vec3 c = math::cross(e1, e2);
    const float length = c.length();
    if (!(length > kDegenerateSine * e1.length() * e2.length())) return false;
    normal = c * (1.0f / length);
    return true;
}

// Signed distances of the vertices of `t` to the plane through `origin` with unit normal
// `n`. Distances within `epsilon` are reported as exactly zero (on the plane).
inline void planeDistances(const math::Vec3* t, math::Vec3 origin, math::Vec3 n, float epsilon,
                           float* d) {
    for (int i = 0; i < 3; ++i) {
        const float v = math::dot(t[i] - origin, n);
        d[i] = std::fabs(v) <= epsilon ? 0.0f : v;
    }
}

// Smaller of the two translations along the normal that put all of the vertices on one side.
inline float axisOvershoot(const float* d) {
    float behind = 0.0f;  // farthest vertex on the -n side
    float ahead = 0.0f;   // farthest vertex on the +n side
    for (int i = 0; i < 3; ++i) {
        behind = std::max(behind, -d[i]);
        ahead = std::max(ahead, d[i]);
    }
    return std::min(behind, ahead);
}

// Interval [lo, hi] where a triangle crosses the other plane, measured along the dominant
// axis of the intersection line (Moller 1997). `vv` are the vertex projections on that axis,
// `d` the signed vertex distances to the other plane. Returns false when all three vertices
// lie on the plane (the coplanar case, handled separately).
inline bool lineInterval(const float* vv, const float* d, float& lo, float& hi) {
    float a = 0.0f;
    float b = 0.0f;
    if (d[0] * d[1] > 0.0f) {
        a = vv[2] + (vv[0] - vv[2]) * d[2] / (d[2] - d[0]);
        b = vv[2] + (vv[1] - vv[2]) * d[2] / (d[2] - d[1]);
    } else if (d[0] * d[2] > 0.0f) {
        a = vv[1] + (vv[0] - vv[1]) * d[1] / (d[1] - d[0]);
        b = vv[1] + (vv[2] - vv[1]) * d[1] / (d[1] - d[2]);
    } else if (d[1] * d[2] > 0.0f || d[0] != 0.0f) {
        a = vv[0] + (vv[1] - vv[0]) * d[0] / (d[0] - d[1]);
        b = vv[0] + (vv[2] - vv[0]) * d[0] / (d[0] - d[2]);
    } else if (d[1] != 0.0f) {
        a = vv[1] + (vv[0] - vv[1]) * d[1] / (d[1] - d[0]);
        b = vv[1] + (vv[2] - vv[1]) * d[1] / (d[1] - d[2]);
    } else if (d[2] != 0.0f) {
        a = vv[2] + (vv[0] - vv[2]) * d[2] / (d[2] - d[0]);
        b = vv[2] + (vv[1] - vv[2]) * d[2] / (d[2] - d[1]);
    } else {
        return false;
    }
    lo = std::min(a, b);
    hi = std::max(a, b);
    return true;
}

inline float orient(Point2 p, Point2 q, Point2 r) {
    return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
}

// r lies on the closed segment p-q, given that p, q, r are collinear.
inline bool onSegment(Point2 p, Point2 q, Point2 r) {
    return std::min(p.x, q.x) <= r.x && r.x <= std::max(p.x, q.x) && std::min(p.y, q.y) <= r.y &&
           r.y <= std::max(p.y, q.y);
}

inline bool segmentsTouch(Point2 p1, Point2 p2, Point2 q1, Point2 q2) {
    const float d1 = orient(q1, q2, p1);
    const float d2 = orient(q1, q2, p2);
    const float d3 = orient(p1, p2, q1);
    const float d4 = orient(p1, p2, q2);
    if (((d1 > 0.0f && d2 < 0.0f) || (d1 < 0.0f && d2 > 0.0f)) &&
        ((d3 > 0.0f && d4 < 0.0f) || (d3 < 0.0f && d4 > 0.0f)))
        return true;
    if (d1 == 0.0f && onSegment(q1, q2, p1)) return true;
    if (d2 == 0.0f && onSegment(q1, q2, p2)) return true;
    if (d3 == 0.0f && onSegment(p1, p2, q1)) return true;
    if (d4 == 0.0f && onSegment(p1, p2, q2)) return true;
    return false;
}

// Closed point-in-triangle test (the boundary counts as inside).
inline bool insideClosed(const Point2* t, Point2 p) {
    const float d0 = orient(t[0], t[1], p);
    const float d1 = orient(t[1], t[2], p);
    const float d2 = orient(t[2], t[0], p);
    const bool negative = d0 < 0.0f || d1 < 0.0f || d2 < 0.0f;
    const bool positive = d0 > 0.0f || d1 > 0.0f || d2 > 0.0f;
    return !(negative && positive);
}

// Two coplanar triangles overlap (closed): an edge pair touches, or a vertex of one lies
// inside the other. `n` is the common unit normal.
inline bool coplanarOverlap(const math::Vec3* a, const math::Vec3* b, math::Vec3 n) {
    const float ax = std::fabs(n.x);
    const float ay = std::fabs(n.y);
    const float az = std::fabs(n.z);
    const int drop = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
    const int u = (drop + 1) % 3;
    const int v = (drop + 2) % 3;
    const auto project = [u, v](math::Vec3 p) {
        const float c[3] = {p.x, p.y, p.z};
        return Point2{c[u], c[v]};
    };
    Point2 pa[3];
    Point2 pb[3];
    for (int i = 0; i < 3; ++i) {
        pa[i] = project(a[i]);
        pb[i] = project(b[i]);
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (segmentsTouch(pa[i], pa[(i + 1) % 3], pb[j], pb[(j + 1) % 3])) return true;
    return insideClosed(pa, pb[0]) || insideClosed(pb, pa[0]);
}

} // namespace detail

// Contact of two triangles of three vertices each. `epsilon` is the plane tolerance (metres).
[[nodiscard]] inline TriangleContact triangleContact(const math::Vec3* a, const math::Vec3* b,
                                                     float epsilon = kTriangleEpsilon) {
    TriangleContact out;
    math::Vec3 na;
    math::Vec3 nb;
    if (!detail::unitNormal(a, na) || !detail::unitNormal(b, nb)) return out;

    float da[3];  // vertices of a against the plane of b
    float db[3];  // vertices of b against the plane of a
    detail::planeDistances(a, b[0], nb, epsilon, da);
    detail::planeDistances(b, a[0], na, epsilon, db);
    if (da[0] * da[1] > 0.0f && da[0] * da[2] > 0.0f) return out;
    if (db[0] * db[1] > 0.0f && db[0] * db[2] > 0.0f) return out;

    const bool coplanarA = da[0] == 0.0f && da[1] == 0.0f && da[2] == 0.0f;
    const bool coplanarB = db[0] == 0.0f && db[1] == 0.0f && db[2] == 0.0f;
    if (coplanarA || coplanarB) {
        out.intersects = detail::coplanarOverlap(a, b, na);
        return out;  // coplanar contact has no penetration depth
    }

    const math::Vec3 dir = math::cross(na, nb);
    const float ax = std::fabs(dir.x);
    const float ay = std::fabs(dir.y);
    const float az = std::fabs(dir.z);
    const int axis = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
    const auto component = [axis](math::Vec3 p) { return axis == 0 ? p.x : (axis == 1 ? p.y : p.z); };
    const float va[3] = {component(a[0]), component(a[1]), component(a[2])};
    const float vb[3] = {component(b[0]), component(b[1]), component(b[2])};
    float a0 = 0.0f;
    float a1 = 0.0f;
    float b0 = 0.0f;
    float b1 = 0.0f;
    if (!detail::lineInterval(va, da, a0, a1) || !detail::lineInterval(vb, db, b0, b1)) return out;
    if (a1 < b0 || b1 < a0) return out;  // the two intervals on the line are disjoint

    out.intersects = true;
    // Intervals that only meet at an endpoint are a touch, not a penetration.
    if (std::min(a1, b1) - std::max(a0, b0) <= epsilon) return out;
    out.depth = std::min(detail::axisOvershoot(da), detail::axisOvershoot(db));
    return out;
}

} // namespace bgmedia::probe
