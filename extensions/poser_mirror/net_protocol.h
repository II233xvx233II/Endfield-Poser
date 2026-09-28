#pragma once
// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// 局域网投影协议（UDP，小端，x64 Windows 之间）。只含数据结构和校验，不做网络调用。
//
// 星形拓扑：主机编号 1，加入者由主机分配 2–4。加入者只和主机通信；主机把每个
// 加入者的姿态转发给其余人。任何包都可能丢失、重复或乱序，接收端按 seq 去旧。
#include <cmath>
#include <cstdint>
#include <cstring>

#include "poser_extension.h"

namespace mirror_net {

constexpr uint32_t kMagic = 0x524D5045; // "EPMR"
constexpr uint16_t kVersion = 1;
constexpr uint16_t kDefaultPort = 28923;
constexpr int kMaxPlayers = 4;          // host + 3; the squad has 4 slots
constexpr int kNameBytes = 32;

enum Type : uint8_t { Hello = 1, Welcome = 2, Reject = 3, Pose = 4, Bye = 5 };
enum RejectReason : uint8_t { Full = 1, WrongVersion = 2 };

#pragma pack(push, 1)
struct Header {
  uint32_t magic;
  uint16_t version;
  uint8_t type;
  uint8_t sender;   // player id, 0 before Welcome
  uint32_t session; // chosen by the host, 0 in the first Hello
  uint32_t seq;
};
struct HelloPacket {
  Header header;
  char name[kNameBytes];
};
struct WelcomePacket {
  Header header;
  uint8_t assigned;
};
struct RejectPacket {
  Header header;
  uint8_t reason;
};
struct PosePacket {
  Header header;
  char name[kNameBytes];
  PoserHumanPose pose;
};
struct ByePacket {
  Header header;
};
#pragma pack(pop)

static_assert(sizeof(PoserHumanPose) == 936, "pose layout is part of the wire format");
static_assert(sizeof(PosePacket) < 1200, "stay below a typical MTU");

inline Header MakeHeader(Type type, uint8_t sender, uint32_t session, uint32_t seq) {
  return {kMagic, kVersion, uint8_t(type), sender, session, seq};
}
// Returns the packet type, or 0 when the datagram is not ours or is malformed.
inline uint8_t Classify(const void *data, int bytes) {
  if (bytes < int(sizeof(Header))) return 0;
  Header h;
  memcpy(&h, data, sizeof(h));
  if (h.magic != kMagic || h.version != kVersion) return 0;
  switch (h.type) {
  case Hello: return bytes == int(sizeof(HelloPacket)) ? Hello : 0;
  case Welcome: return bytes == int(sizeof(WelcomePacket)) ? Welcome : 0;
  case Reject: return bytes == int(sizeof(RejectPacket)) ? Reject : 0;
  case Pose: return bytes == int(sizeof(PosePacket)) ? Pose : 0;
  case Bye: return bytes == int(sizeof(ByePacket)) ? Bye : 0;
  default: return 0;
  }
}
// Same rules the host applies before writing a puppet; checked again here so
// the host of a session never relays garbage to the others.
inline bool ValidPose(const PoserHumanPose &p) {
  auto finite = [](PoserVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
  auto usable = [](PoserQuat q) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
    const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return n > .25f && n < 4;
  };
  if (p.size != sizeof(PoserHumanPose) || (p.boneMask >> POSER_HUMAN_BONES) != 0) return false;
  if (!finite(p.rootPosition) || !finite(p.hipsPosition) || !usable(p.bodyRotation)) return false;
  for (int bone = 0; bone < POSER_HUMAN_BONES; ++bone)
    if ((p.boneMask >> bone & 1) && !usable(p.rotations[bone])) return false;
  return true;
}
// Copies a UTF-8 name, cutting on a character boundary and always terminating.
inline void CopyName(char (&out)[kNameBytes], const char *in) {
  memset(out, 0, sizeof(out));
  size_t n = in ? strnlen(in, kNameBytes - 1) : 0;
  while (n > 0 && n < strlen(in) && (uint8_t(in[n]) & 0xC0) == 0x80) --n;
  if (n) memcpy(out, in, n);
}
// seq wraps; treat anything within half the range ahead as newer.
inline bool Newer(uint32_t seq, uint32_t last) { return int32_t(seq - last) > 0; }

} // namespace mirror_net
