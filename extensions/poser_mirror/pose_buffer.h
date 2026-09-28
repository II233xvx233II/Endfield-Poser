#pragma once
// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// 带时间戳的姿态缓冲与插值。本地镜像和网络接收共用：读出时间比最新样本晚
// 一个“延迟”，用前后两帧插值，抹平丢包和到达抖动。
#include <cmath>
#include <deque>

#include "poser_extension.h"

namespace mirror_pose {

inline PoserVec3 Lerp(PoserVec3 a, PoserVec3 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
// Normalized lerp along the shorter arc; neighbouring samples are close together.
inline PoserQuat Nlerp(PoserQuat a, PoserQuat b, float t) {
  const float sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0 ? -1.f : 1.f;
  PoserQuat q{a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t,
              a.z + (b.z * sign - a.z) * t, a.w + (b.w * sign - a.w) * t};
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (length < 1e-6f) return a;
  return {q.x / length, q.y / length, q.z / length, q.w / length};
}
inline PoserVec3 Rotate(PoserQuat q, PoserVec3 v) {
  // v + 2w(u x v) + 2u x (u x v), u = q.xyz
  const PoserVec3 u{q.x, q.y, q.z};
  const PoserVec3 c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
  const PoserVec3 cc{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
  return {v.x + 2 * (q.w * c.x + cc.x), v.y + 2 * (q.w * c.y + cc.y), v.z + 2 * (q.w * c.z + cc.z)};
}
inline float Distance(PoserVec3 a, PoserVec3 b) {
  const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
  return std::sqrt(x * x + y * y + z * z);
}
inline PoserHumanPose Blend(const PoserHumanPose &a, const PoserHumanPose &b, float t) {
  PoserHumanPose out = a;
  out.boneMask = a.boneMask & b.boneMask;
  out.rootPosition = Lerp(a.rootPosition, b.rootPosition, t);
  out.bodyRotation = Nlerp(a.bodyRotation, b.bodyRotation, t);
  out.hipsPosition = Lerp(a.hipsPosition, b.hipsPosition, t);
  for (int bone = 0; bone < POSER_HUMAN_BONES; ++bone)
    if (out.boneMask >> bone & 1) out.rotations[bone] = Nlerp(a.rotations[bone], b.rotations[bone], t);
  return out;
}

class PoseBuffer {
public:
  void push(double time, const PoserHumanPose &pose) {
    if (!samples_.empty() && time < samples_.back().time) time = samples_.back().time;
    samples_.push_back({time, pose});
    if (samples_.size() > 600) samples_.pop_front(); // ~10 s at 60 fps
  }
  // Drops samples no longer needed to read at `oldest` or later.
  void trim(double oldest) {
    while (samples_.size() > 2 && samples_[1].time < oldest) samples_.pop_front();
  }
  bool sample(double time, PoserHumanPose &out) const {
    if (samples_.empty()) return false;
    if (time <= samples_.front().time) { out = samples_.front().pose; return true; }
    for (size_t i = 1; i < samples_.size(); ++i)
      if (samples_[i].time >= time) {
        const double span = samples_[i].time - samples_[i - 1].time;
        const float t = span > 1e-6 ? float((time - samples_[i - 1].time) / span) : 1.f;
        out = Blend(samples_[i - 1].pose, samples_[i].pose, t);
        return true;
      }
    out = samples_.back().pose; // Hold the last pose while packets are late.
    return true;
  }
  void clear() { samples_.clear(); }
  size_t size() const { return samples_.size(); }

private:
  struct Sample {
    double time;
    PoserHumanPose pose;
  };
  std::deque<Sample> samples_;
};

} // namespace mirror_pose
