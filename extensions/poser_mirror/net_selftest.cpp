// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// 局域网协议自测：同一进程内起一个主机和多个加入者，走 127.0.0.1，无需游戏。
// 由 build.bat -Extensions 编译并运行。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "net_session.h"

#include <cstdio>
#include <string>

using namespace mirror_net;

static int failures = 0;
static void Check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}
static PoserHumanPose MakePose(float x) {
  PoserHumanPose p{};
  p.size = sizeof(p);
  p.rootPosition = {x, 0, 0};
  p.bodyRotation = {0, 0, 0, 1};
  p.boneMask = 1;
  p.rotations[0] = {0, 0, 0, 1};
  return p;
}
static void Pump(Session *sessions[], const float xs[], int count, double seconds) {
  const double end = Now() + seconds;
  while (Now() < end) {
    for (int i = 0; i < count; ++i)
      if (sessions[i]) sessions[i]->publish(MakePose(xs[i]), i % POSER_SQUAD_SLOTS, ("chr_" + std::to_string(i) + "(Clone)#3").c_str());
    Sleep(10);
  }
}
static bool Sees(Session &s, uint8_t id, float x) {
  for (const auto &v : s.sample(Now()))
    if (v.id == id && v.pose.rootPosition.x == x) return true;
  return false;
}
static void SendRaw(uint16_t port, const void *data, int bytes) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
  sendto(s, static_cast<const char *>(data), bytes, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
  closesocket(s);
}

int main() {
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  const uint16_t port = 38923;
  std::string error;

  // Pure checks.
  PoserHumanPose bad = MakePose(0);
  bad.rotations[0] = {0, 0, 0, 0};
  Check(!ValidPose(bad), "zero quaternion rejected");
  bad = MakePose(0);
  bad.boneMask = 1ull << 60;
  Check(!ValidPose(bad), "mask beyond 55 bones rejected");
  bad = MakePose(std::nanf(""));
  Check(!ValidPose(bad), "NaN position rejected");
  Check(Newer(1, 0xFFFFFFFFu) && !Newer(5, 5) && !Newer(4, 5), "sequence wraparound");
  char name[kNameBytes];
  CopyName(name, u8"一二三四五六七八九十一二三"); // 39 bytes of 3-byte characters
  Check(strlen(name) == 30 && name[31] == 0, "name cut on a UTF-8 boundary");
  char model[kModelBytes];
  NormalizeModel(model, "chr_0003_endminf (Clone)#12");
  Check(!strcmp(model, "chr_0003_endminf"), "model name normalized like Poser");

  Session host, a, b, c, d;
  Check(host.startHost(port, "host", error), "host starts");
  Check(a.startJoin("127.0.0.1", port, "a", error), "joiner a starts");
  Check(b.startJoin("127.0.0.1", port, "b", error), "joiner b starts");
  Session *all[] = {&host, &a, &b};
  const float xs[] = {1, 2, 3};
  Pump(all, xs, 3, 1.5);
  Check(host.players() == 3, "host counts three players");
  Check(Sees(host, 2, 2) && Sees(host, 3, 3), "host receives both joiners");
  Check(Sees(a, 1, 1) && Sees(a, 3, 3), "joiner a receives host and relayed b");
  Check(Sees(b, 1, 1) && Sees(b, 2, 2), "joiner b receives host and relayed a");
  bool carried = false;
  for (const auto &v : b.sample(Now()))
    if (v.id == 2) carried = v.slot == 1 && !strcmp(v.model, "chr_1") && v.received > 0;
  Check(carried, "relayed packets keep the sender's slot and character");
  Check(a.sent() > 0 && host.sent() > 0, "send counters advance");
  Check(host.status().find(u8"主机已开启") != std::string::npos, "host status reports addresses");

  // Junk and stale packets must be ignored without disturbing the session.
  char junk[64] = "not a poser packet";
  SendRaw(port, junk, sizeof(junk));
  PosePacket forged{};
  forged.header = MakeHeader(Pose, 2, 12345, 999999);
  forged.pose = MakePose(99);
  SendRaw(port, &forged, sizeof(forged)); // Wrong address and session.
  Pump(all, xs, 3, .3);
  Check(Sees(host, 2, 2) && !Sees(host, 2, 99), "junk and forged packets ignored");

  // Room is full after four players.
  Check(c.startJoin("127.0.0.1", port, "c", error), "joiner c starts");
  Check(d.startJoin("127.0.0.1", port, "d", error), "joiner d starts");
  Session *five[] = {&host, &a, &b, &c, &d};
  const float xs5[] = {1, 2, 3, 4, 5};
  Pump(five, xs5, 5, 1.5);
  Check(host.players() == 4, "host caps the room at four");
  Check(d.status().find(u8"已满") != std::string::npos || c.status().find(u8"已满") != std::string::npos,
        "fifth player is told the room is full");

  // A joiner leaving says Bye; the host drops it at once.
  c.stop();
  d.stop();
  b.stop();
  Session *two[] = {&host, &a};
  Pump(two, xs, 2, .5);
  Check(host.players() == 2, "host drops a joiner that left");
  Pump(two, xs, 2, 3.5);
  Check(!Sees(a, 3, 3), "joiner forgets a player that left");

  // An old-version joiner is told why instead of waiting forever.
  {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    HelloPacket old{};
    old.header = MakeHeader(Hello, 0, 0, 1);
    old.header.version = kVersion - 1;
    sendto(s, reinterpret_cast<const char *>(&old), sizeof(old), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
    DWORD wait = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&wait), sizeof(wait));
    RejectPacket reply{};
    const int got = recv(s, reinterpret_cast<char *>(&reply), sizeof(reply), 0);
    Check(got == int(sizeof(reply)) && reply.header.type == Reject && reply.reason == WrongVersion &&
              reply.header.version == kVersion,
          "host rejects an old-version Hello with its own version");
    closesocket(s);
  }
  {
    // A joiner facing an old-version host sees the mismatch.
    SOCKET fake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port + 2);
    inet_pton(AF_INET, "127.0.0.1", &local.sin_addr);
    bind(fake, reinterpret_cast<sockaddr *>(&local), sizeof(local));
    Session e;
    e.startJoin("127.0.0.1", uint16_t(port + 2), "e", error);
    DWORD wait = 2000;
    setsockopt(fake, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&wait), sizeof(wait));
    char hello[256];
    sockaddr_in from{};
    int fromBytes = sizeof(from);
    if (recvfrom(fake, hello, sizeof(hello), 0, reinterpret_cast<sockaddr *>(&from), &fromBytes) > 0) {
      RejectPacket reject{};
      reject.header = MakeHeader(Reject, 1, 0, 1);
      reject.header.version = kVersion + 1;
      reject.reason = WrongVersion;
      sendto(fake, reinterpret_cast<const char *>(&reject), sizeof(reject), 0, reinterpret_cast<sockaddr *>(&from), fromBytes);
    }
    Sleep(300);
    Check(e.status().find(u8"版本不一致") != std::string::npos, "joiner reports a host on another version");
    e.stop();
    closesocket(fake);
  }
  {
    // Nobody listening: after a few Hellos the joiner explains what to check.
    Session f;
    f.startJoin("127.0.0.1", uint16_t(port + 4), "f", error);
    Sleep(3500);
    Check(f.status().find(u8"没有回应") != std::string::npos, "joiner explains a silent host");
    f.stop();
  }

  // Host shutting down: the joiner notices and waits for it to return.
  host.stop();
  Sleep(300);
  Check(a.status().find(u8"主机已关闭") != std::string::npos, "joiner notices the host closing");
  a.stop();

  printf(failures ? "net_selftest: %d FAILED\n" : "net_selftest: all passed\n", failures);
  WSACleanup();
  return failures ? 1 : 0;
}
