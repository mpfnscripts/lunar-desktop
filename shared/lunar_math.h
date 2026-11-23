// Small quaternion/pose helpers shared by the PC and Quest code.
#pragma once
#include <math.h>
#include "lunar_protocol.h"

static inline LQuat lqMul(LQuat a, LQuat b) {
    LQuat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
static inline LQuat lqConj(LQuat q) { LQuat r = {-q.x, -q.y, -q.z, q.w}; return r; }
static inline LQuat lqIdentity() { LQuat r = {0, 0, 0, 1}; return r; }
static inline LQuat lqNormalize(LQuat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-8f) return lqIdentity();
    LQuat r = {q.x / n, q.y / n, q.z / n, q.w / n};
    return r;
}
static inline LVec3 lqRotate(LQuat q, LVec3 v) {
    // v' = q * v * q^-1
    LVec3 u = {q.x, q.y, q.z};
    float s = q.w;
    float dot_uv = u.x * v.x + u.y * v.y + u.z * v.z;
    float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    LVec3 cross = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    LVec3 r;
    r.x = 2.0f * dot_uv * u.x + (s * s - dot_uu) * v.x + 2.0f * s * cross.x;
    r.y = 2.0f * dot_uv * u.y + (s * s - dot_uu) * v.y + 2.0f * s * cross.y;
    r.z = 2.0f * dot_uv * u.z + (s * s - dot_uu) * v.z + 2.0f * s * cross.z;
    return r;
}
static inline LVec3 lvAdd(LVec3 a, LVec3 b) { LVec3 r = {a.x + b.x, a.y + b.y, a.z + b.z}; return r; }
static inline LVec3 lvSub(LVec3 a, LVec3 b) { LVec3 r = {a.x - b.x, a.y - b.y, a.z - b.z}; return r; }
static inline LVec3 lvScale(LVec3 a, float s) { LVec3 r = {a.x * s, a.y * s, a.z * s}; return r; }
static inline float lvDot(LVec3 a, LVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float lvLen(LVec3 a) { return sqrtf(lvDot(a, a)); }

// a * b : apply b in a's space
static inline LPose lpMul(LPose a, LPose b) {
    LPose r;
    r.orientation = lqNormalize(lqMul(a.orientation, b.orientation));
    r.position = lvAdd(a.position, lqRotate(a.orientation, b.position));
    return r;
}
static inline LPose lpInverse(LPose p) {
    LPose r;
    r.orientation = lqConj(p.orientation);
    r.position = lqRotate(r.orientation, lvScale(p.position, -1.0f));
    return r;
}
static inline LPose lpIdentity() {
    LPose r;
    r.orientation = lqIdentity();
    r.position.x = r.position.y = r.position.z = 0;
    return r;
}
static inline LQuat lqFromEulerDeg(float pitchX, float yawY, float rollZ) {
    float cx = cosf(pitchX * 0.5f * 3.14159265f / 180.f), sx = sinf(pitchX * 0.5f * 3.14159265f / 180.f);
    float cy = cosf(yawY * 0.5f * 3.14159265f / 180.f), sy = sinf(yawY * 0.5f * 3.14159265f / 180.f);
    float cz = cosf(rollZ * 0.5f * 3.14159265f / 180.f), sz = sinf(rollZ * 0.5f * 3.14159265f / 180.f);
    LQuat qx = {sx, 0, 0, cx}, qy = {0, sy, 0, cy}, qz = {0, 0, sz, cz};
    return lqMul(lqMul(qy, qx), qz);
}
// Rotation matrix (row-major 3x3) from quaternion
static inline void lqToMat3(LQuat q, float m[3][3]) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0][0] = 1 - 2 * (yy + zz); m[0][1] = 2 * (xy - wz);     m[0][2] = 2 * (xz + wy);
    m[1][0] = 2 * (xy + wz);     m[1][1] = 1 - 2 * (xx + zz); m[1][2] = 2 * (yz - wx);
    m[2][0] = 2 * (xz - wy);     m[2][1] = 2 * (yz + wx);     m[2][2] = 1 - 2 * (xx + yy);
}
static inline LQuat lqFromMat3(const float m[3][3]) {
    LQuat q;
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2;
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2;
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2;
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return lqNormalize(q);
}
