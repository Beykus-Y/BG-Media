#pragma once

// Small, dependency-free linear algebra layer shared by the VRM importer,
// humanoid retargeter and the SDL_GPU renderer. It deliberately lives outside
// BG Beat/BG Sync/BG Motion: the musical engine only produces a normalised
// CharacterPose, never matrices or graphics API objects.
//
// Matrices are column-major, matching the memory layout consumed by HLSL/SPIR-V
// constant buffers and by glTF's column-major `matrix` property.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace bgmedia::math {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-6f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] float lengthSquared() const { return x * x + y * y + z * z; }
    [[nodiscard]] float length() const { return std::sqrt(lengthSquared()); }
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator*(float s, Vec3 v) { return v * s; }
inline Vec3 operator/(Vec3 v, float s) {
    return s != 0.0f ? Vec3{v.x / s, v.y / s, v.z / s} : Vec3{};
}
inline Vec3& operator+=(Vec3& a, Vec3 b) {
    a = a + b;
    return a;
}

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline Vec3 normalize(Vec3 v) {
    const float length = v.length();
    return length > kEpsilon ? v / length : Vec3{};
}
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

inline Vec4 operator*(Vec4 v, float s) {
    return {v.x * s, v.y * s, v.z * s, v.w * s};
}
inline Vec4 operator+(Vec4 a, Vec4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline Vec4 operator-(Vec4 a, Vec4 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline Vec4 lerp(Vec4 a, Vec4 b, float t) { return a + (b - a) * t; }

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    [[nodiscard]] static Quat identity() { return {}; }
    [[nodiscard]] float lengthSquared() const {
        return x * x + y * y + z * z + w * w;
    }
    [[nodiscard]] Quat normalized() const {
        const float length = std::sqrt(lengthSquared());
        if (length < kEpsilon) {
            return identity();
        }
        return {x / length, y / length, z / length, w / length};
    }
    [[nodiscard]] Quat conjugate() const { return {-x, -y, -z, w}; }
};

inline Quat operator*(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat& operator*=(Quat& a, Quat b) {
    a = a * b;
    return a;
}
inline float dot(Quat a, Quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
inline Quat normalize(Quat q) { return q.normalized(); }
inline Quat conjugate(Quat q) { return q.conjugate(); }

inline Quat fromAxisAngle(Vec3 axis, float radians) {
    axis = normalize(axis);
    if (axis.lengthSquared() < 0.5f) {
        return Quat::identity();
    }
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

// Shortest-arc rotation taking `from` to `to`. Both vectors do not need to be
// normalised. Opposite vectors pick a stable, non-parallel orthogonal axis so
// retargeting never produces NaN.
inline Quat fromTo(Vec3 from, Vec3 to) {
    from = normalize(from);
    to = normalize(to);
    if (from.lengthSquared() < 0.5f || to.lengthSquared() < 0.5f) {
        return Quat::identity();
    }
    const float d = std::clamp(dot(from, to), -1.0f, 1.0f);
    if (d > 0.999999f) {
        return Quat::identity();
    }
    if (d < -0.999999f) {
        Vec3 axis = cross(from, Vec3{1.0f, 0.0f, 0.0f});
        if (axis.lengthSquared() < 1.0e-4f) {
            axis = cross(from, Vec3{0.0f, 1.0f, 0.0f});
        }
        return fromAxisAngle(axis, kPi);
    }
    const Vec3 axis = cross(from, to);
    return Quat{axis.x, axis.y, axis.z, 1.0f + d}.normalized();
}

inline Vec3 rotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

inline Quat slerp(Quat a, Quat b, float t) {
    a = a.normalized();
    b = b.normalized();
    float cosine = dot(a, b);
    if (cosine < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    if (cosine > 0.9995f) {
        return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}
            .normalized();
    }
    const float theta = std::acos(std::clamp(cosine, -1.0f, 1.0f));
    const float sinTheta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sinTheta;
    const float wb = std::sin(t * theta) / sinTheta;
    return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                a.z * wa + b.z * wb, a.w * wa + b.w * wb}
        .normalized();
}

inline std::array<float, 16> quatToMatrix(Quat q) {
    q = q.normalized();
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    std::array<float, 16> m{};
    m[0] = 1.0f - 2.0f * (yy + zz);
    m[1] = 2.0f * (xy + wz);
    m[2] = 2.0f * (xz - wy);
    m[4] = 2.0f * (xy - wz);
    m[5] = 1.0f - 2.0f * (xx + zz);
    m[6] = 2.0f * (yz + wx);
    m[8] = 2.0f * (xz + wy);
    m[9] = 2.0f * (yz - wx);
    m[10] = 1.0f - 2.0f * (xx + yy);
    m[15] = 1.0f;
    return m;
}

inline Quat matrixToQuat(const std::array<float, 16>& m) {
    const float m00 = m[0];
    const float m11 = m[5];
    const float m22 = m[10];
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m[6] - m[9]) / s;
        q.y = (m[8] - m[2]) / s;
        q.z = (m[1] - m[4]) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m[6] - m[9]) / s;
        q.x = 0.25f * s;
        q.y = (m[4] + m[1]) / s;
        q.z = (m[8] + m[2]) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m[8] - m[2]) / s;
        q.x = (m[4] + m[1]) / s;
        q.y = 0.25f * s;
        q.z = (m[9] + m[6]) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m[1] - m[4]) / s;
        q.x = (m[8] + m[2]) / s;
        q.y = (m[9] + m[6]) / s;
        q.z = 0.25f * s;
    }
    return q.normalized();
}

struct Mat4 {
    std::array<float, 16> m{};

    [[nodiscard]] static Mat4 identity() {
        Mat4 result;
        result.m[0] = 1.0f;
        result.m[5] = 1.0f;
        result.m[10] = 1.0f;
        result.m[15] = 1.0f;
        return result;
    }

    [[nodiscard]] static Mat4 fromQuat(Quat q) {
        Mat4 result;
        result.m = quatToMatrix(q);
        return result;
    }

    [[nodiscard]] static Mat4 translation(Vec3 t) {
        Mat4 result = identity();
        result.m[12] = t.x;
        result.m[13] = t.y;
        result.m[14] = t.z;
        return result;
    }

    [[nodiscard]] static Mat4 scale(Vec3 s) {
        Mat4 result = identity();
        result.m[0] = s.x;
        result.m[5] = s.y;
        result.m[10] = s.z;
        return result;
    }

    [[nodiscard]] static Mat4 fromTRS(Vec3 translation, Quat rotation, Vec3 scale) {
        Mat4 result = fromQuat(rotation);
        result.m[0] *= scale.x;
        result.m[1] *= scale.x;
        result.m[2] *= scale.x;
        result.m[4] *= scale.y;
        result.m[5] *= scale.y;
        result.m[6] *= scale.y;
        result.m[8] *= scale.z;
        result.m[9] *= scale.z;
        result.m[10] *= scale.z;
        result.m[12] = translation.x;
        result.m[13] = translation.y;
        result.m[14] = translation.z;
        return result;
    }

    [[nodiscard]] Vec3 translationPart() const { return {m[12], m[13], m[14]}; }
    [[nodiscard]] Vec3 column(std::size_t index) const {
        const std::size_t base = index * 4;
        return index < 4 ? Vec3{m[base], m[base + 1], m[base + 2]} : Vec3{};
    }
    [[nodiscard]] Vec3 axisX() const { return column(0); }
    [[nodiscard]] Vec3 axisY() const { return column(1); }
    [[nodiscard]] Vec3 axisZ() const { return column(2); }

    [[nodiscard]] Vec3 transformPoint(Vec3 p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    [[nodiscard]] Vec3 transformVector(Vec3 v) const {
        return {m[0] * v.x + m[4] * v.y + m[8] * v.z,
                m[1] * v.x + m[5] * v.y + m[9] * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z};
    }

    [[nodiscard]] Quat rotationQuat() const {
        const Vec3 x = normalize(axisX());
        const Vec3 y = normalize(axisY());
        const Vec3 z = normalize(axisZ());
        std::array<float, 16> r{};
        r[0] = x.x;
        r[1] = x.y;
        r[2] = x.z;
        r[4] = y.x;
        r[5] = y.y;
        r[6] = y.z;
        r[8] = z.x;
        r[9] = z.y;
        r[10] = z.z;
        r[15] = 1.0f;
        return matrixToQuat(r);
    }

    [[nodiscard]] Mat4 inverseAffine() const {
        const float a = m[0];
        const float b = m[1];
        const float c = m[2];
        const float d = m[4];
        const float e = m[5];
        const float f = m[6];
        const float g = m[8];
        const float h = m[9];
        const float i = m[10];
        const float det = a * (e * i - f * h) - d * (b * i - c * h) +
                          g * (b * f - c * e);
        if (!std::isfinite(det) || std::abs(det) < 1.0e-9f) {
            return identity();
        }
        const float invDet = 1.0f / det;
        Mat4 out = identity();
        out.m[0] = (e * i - f * h) * invDet;
        out.m[1] = (c * h - b * i) * invDet;
        out.m[2] = (b * f - c * e) * invDet;
        out.m[4] = (f * g - d * i) * invDet;
        out.m[5] = (a * i - c * g) * invDet;
        out.m[6] = (c * d - a * f) * invDet;
        out.m[8] = (d * h - e * g) * invDet;
        out.m[9] = (b * g - a * h) * invDet;
        out.m[10] = (a * e - b * d) * invDet;
        const Vec3 t = translationPart();
        out.m[12] = -(out.m[0] * t.x + out.m[4] * t.y + out.m[8] * t.z);
        out.m[13] = -(out.m[1] * t.x + out.m[5] * t.y + out.m[9] * t.z);
        out.m[14] = -(out.m[2] * t.x + out.m[6] * t.y + out.m[10] * t.z);
        return out;
    }

    [[nodiscard]] Mat4 operator*(const Mat4& rhs) const {
        Mat4 out;
        for (std::size_t col = 0; col < 4; ++col) {
            for (std::size_t row = 0; row < 4; ++row) {
                float value = 0.0f;
                for (std::size_t k = 0; k < 4; ++k) {
                    value += m[k * 4 + row] * rhs.m[col * 4 + k];
                }
                out.m[col * 4 + row] = value;
            }
        }
        return out;
    }

    [[nodiscard]] bool finite() const {
        for (float value : m) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }
};

inline Mat4 perspectiveFovLH(float fovYRadians, float aspect, float nearPlane,
                              float farPlane) {
    Mat4 out;
    if (aspect <= 0.0f || farPlane <= nearPlane || nearPlane <= 0.0f) {
        return Mat4::identity();
    }
    const float yScale = 1.0f / std::tan(fovYRadians * 0.5f);
    const float xScale = yScale / aspect;
    out.m[0] = xScale;
    out.m[5] = yScale;
    out.m[10] = farPlane / (farPlane - nearPlane);
    out.m[11] = 1.0f;
    out.m[14] = -(nearPlane * farPlane) / (farPlane - nearPlane);
    return out;
}

inline Mat4 lookAtLH(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 forward = normalize(target - eye);
    if (forward.lengthSquared() < 0.5f) {
        return Mat4::identity();
    }
    Vec3 right = normalize(cross(up, forward));
    if (right.lengthSquared() < 0.5f) {
        right = normalize(cross(Vec3{0.0f, 0.0f, 1.0f}, forward));
    }
    const Vec3 correctedUp = cross(forward, right);
    Mat4 out = Mat4::identity();
    out.m[0] = right.x;
    out.m[4] = right.y;
    out.m[8] = right.z;
    out.m[12] = -dot(right, eye);
    out.m[1] = correctedUp.x;
    out.m[5] = correctedUp.y;
    out.m[9] = correctedUp.z;
    out.m[13] = -dot(correctedUp, eye);
    out.m[2] = forward.x;
    out.m[6] = forward.y;
    out.m[10] = forward.z;
    out.m[14] = -dot(forward, eye);
    return out;
}

inline bool decomposeTRS(const Mat4& matrix, Vec3* translation, Quat* rotation,
                         Vec3* scale) {
    if (!matrix.finite()) {
        return false;
    }
    const Vec3 sx = matrix.axisX();
    const Vec3 sy = matrix.axisY();
    const Vec3 sz = matrix.axisZ();
    const float lengthX = sx.length();
    const float lengthY = sy.length();
    const float lengthZ = sz.length();
    if (lengthX < 1.0e-6f || lengthY < 1.0e-6f || lengthZ < 1.0e-6f) {
        return false;
    }
    if (translation) {
        *translation = matrix.translationPart();
    }
    if (rotation) {
        std::array<float, 16> normalized{};
        normalized[0] = sx.x / lengthX;
        normalized[1] = sx.y / lengthX;
        normalized[2] = sx.z / lengthX;
        normalized[4] = sy.x / lengthY;
        normalized[5] = sy.y / lengthY;
        normalized[6] = sy.z / lengthY;
        normalized[8] = sz.x / lengthZ;
        normalized[9] = sz.y / lengthZ;
        normalized[10] = sz.z / lengthZ;
        normalized[15] = 1.0f;
        *rotation = matrixToQuat(normalized);
    }
    if (scale) {
        *scale = {lengthX, lengthY, lengthZ};
    }
    return true;
}

} // namespace bgmedia::math
