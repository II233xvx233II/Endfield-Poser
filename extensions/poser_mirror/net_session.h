#pragma once
// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// 局域网会话：一条后台线程负责收发，游戏线程只通过 publish()/sample() 交换数据，
// 不在游戏帧里等待网络。主机转发加入者之间的姿态；只有主机需要放行防火墙入站。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "net_protocol.h"
#include "pose_buffer.h"

#pragma comment(lib, "ws2_32.lib")

namespace mirror_net {

inline double Now() {
  static const double frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) / frequency;
}

constexpr double kSendInterval = 1.0 / 30;
constexpr double kPeerTimeout = 5, kRemoteTimeout = 3, kHelloInterval = 1;

class Session {
public:
  struct View {
    uint8_t id;
    char name[kNameBytes];
    PoserHumanPose pose;
  };

  bool startHost(uint16_t port, const char *name, std::string &error) {
    return start(true, "", port, name, error);
  }
  bool startJoin(const std::string &host, uint16_t port, const char *name, std::string &error) {
    return start(false, host, port, name, error);
  }
  // Waits at most ~0.2 s: the worker wakes at least every 50 ms.
  void stop() {
    if (!thread_) return;
    stop_ = true;
    if (WaitForSingleObject(thread_, 1000) != WAIT_OBJECT_0) {
      // Never block the game; the worker still exits on its own later.
      CloseHandle(thread_);
      thread_ = nullptr;
      return;
    }
    CloseHandle(thread_);
    thread_ = nullptr;
    Lock lock(lock_);
    remotes_.clear();
    players_ = 0;
    status_ = u8"未连接";
  }
  bool running() const { return thread_ != nullptr; }

  void publish(const PoserHumanPose &pose) {
    Lock lock(lock_);
    outgoing_ = pose;
    outgoingTime_ = Now();
  }
  // Remote players' poses at `time` on this machine's clock.
  std::vector<View> sample(double time) {
    std::vector<View> views;
    Lock lock(lock_);
    for (auto &entry : remotes_) {
      auto &remote = entry.second;
      remote.buffer.trim(time - 1);
      View view;
      view.id = entry.first;
      memcpy(view.name, remote.name, sizeof(view.name));
      if (remote.buffer.sample(time, view.pose)) views.push_back(view);
    }
    return views;
  }
  std::string status() {
    Lock lock(lock_);
    return status_;
  }
  int players() {
    Lock lock(lock_);
    return players_;
  }

private:
  struct Lock {
    explicit Lock(SRWLOCK &l) : lock(l) { AcquireSRWLockExclusive(&lock); }
    ~Lock() { ReleaseSRWLockExclusive(&lock); }
    SRWLOCK &lock;
  };
  struct Remote {
    char name[kNameBytes] = {};
    uint32_t seq = 0;
    bool any = false;
    double lastSeen = 0;
    mirror_pose::PoseBuffer buffer;
  };
  struct Peer {
    sockaddr_in addr;
    uint8_t id;
    double lastSeen;
  };

  // ---- shared with the game thread, guarded by lock_ ----
  SRWLOCK lock_ = SRWLOCK_INIT;
  PoserHumanPose outgoing_{};
  double outgoingTime_ = -1e9;
  std::map<uint8_t, Remote> remotes_;
  std::string status_ = u8"未连接";
  int players_ = 0;

  // ---- worker thread only (set before it starts) ----
  HANDLE thread_ = nullptr;
  volatile bool stop_ = false;
  bool host_ = false, joined_ = false;
  std::string hostName_;
  uint16_t port_ = kDefaultPort;
  char name_[kNameBytes] = {};
  SOCKET socket_ = INVALID_SOCKET;
  sockaddr_in hostAddr_{};
  uint32_t session_ = 0, seq_ = 0;
  uint8_t self_ = 0;
  std::vector<Peer> peers_;
  double lastHostSeen_ = 0;

  void setStatus(const std::string &text) {
    Lock lock(lock_);
    status_ = text;
  }
  bool start(bool host, const std::string &hostName, uint16_t port, const char *name, std::string &error) {
    stop();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = u8"网络初始化失败"; return false; }
    host_ = host;
    hostName_ = hostName;
    port_ = port;
    CopyName(name_, name);
    stop_ = false;
    joined_ = false;
    peers_.clear();
    {
      Lock lock(lock_);
      remotes_.clear();
      players_ = host ? 1 : 0;
      status_ = host ? u8"正在创建主机…" : u8"正在连接主机…";
    }
    thread_ = CreateThread(nullptr, 0, Main, this, 0, nullptr);
    if (!thread_) { WSACleanup(); error = u8"无法创建网络线程"; return false; }
    return true;
  }
  static DWORD WINAPI Main(LPVOID self) {
    static_cast<Session *>(self)->run();
    WSACleanup();
    return 0;
  }

  bool open(std::string &error) {
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) { error = u8"无法创建套接字"; return false; }
    u_long nonBlocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(host_ ? port_ : 0);
    if (bind(socket_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
      error = host_ ? u8"端口 " + std::to_string(port_) + u8" 已被占用" : u8"无法绑定本地端口";
      return false;
    }
    if (host_) {
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      session_ = uint32_t(t.QuadPart ^ (t.QuadPart >> 32) ^ GetCurrentProcessId()) | 1;
      self_ = 1;
      return true;
    }
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(hostName_.c_str(), std::to_string(port_).c_str(), &hints, &found) != 0 || !found) {
      error = u8"无法解析主机地址：" + hostName_;
      return false;
    }
    memcpy(&hostAddr_, found->ai_addr, sizeof(hostAddr_));
    freeaddrinfo(found);
    return true;
  }
  static std::string LocalAddresses() {
    char host[256] = {};
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    std::string text;
    if (gethostname(host, sizeof(host)) != 0 || getaddrinfo(host, nullptr, &hints, &found) != 0) return text;
    for (auto *a = found; a; a = a->ai_next) {
      char ip[INET_ADDRSTRLEN] = {};
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ai_addr)->sin_addr, ip, sizeof(ip));
      if (!text.empty()) text += " / ";
      text += ip;
    }
    freeaddrinfo(found);
    return text;
  }
  void run() {
    std::string error;
    if (!open(error)) {
      if (socket_ != INVALID_SOCKET) closesocket(socket_);
      socket_ = INVALID_SOCKET;
      setStatus(error);
      return;
    }
    const std::string addresses = host_ ? LocalAddresses() : "";
    if (host_)
      setStatus(u8"主机已开启，端口 " + std::to_string(port_) + u8"；本机地址：" +
                (addresses.empty() ? u8"未知" : addresses));
    double nextSend = 0, nextHello = 0, nextKeepAlive = 0;
    while (!stop_) {
      double now = Now();
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(socket_, &readable);
      const double wake = (std::min)(nextSend, host_ || joined_ ? nextSend : nextHello);
      const double wait = (std::max)(0.0, (std::min)(wake - now, 0.05));
      timeval timeout{0, long(wait * 1e6)};
      if (select(0, &readable, nullptr, nullptr, &timeout) > 0) drain();
      now = Now();
      expire(now);
      if (!host_ && !joined_ && now >= nextHello) {
        sendHello();
        nextHello = now + kHelloInterval;
      }
      if (now >= nextSend) {
        nextSend = now + kSendInterval;
        if (!sendPose(now) && now >= nextKeepAlive) {
          keepAlive(); // Stay connected while this side has no pose to send.
          nextKeepAlive = now + 1;
        }
      }
    }
    if (host_) for (const auto &peer : peers_) sendBye(peer.addr);
    else if (joined_) sendBye(hostAddr_);
    closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }

  // ---- sending ----
  void sendTo(const sockaddr_in &to, const void *data, int bytes) {
    sendto(socket_, static_cast<const char *>(data), bytes, 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
  }
  void sendHello() {
    HelloPacket p{};
    p.header = MakeHeader(Hello, self_, session_, ++seq_);
    memcpy(p.name, name_, sizeof(p.name));
    sendTo(hostAddr_, &p, sizeof(p));
  }
  void sendWelcome(const sockaddr_in &to, uint8_t id) {
    WelcomePacket p{};
    p.header = MakeHeader(Welcome, 1, session_, ++seq_);
    p.assigned = id;
    sendTo(to, &p, sizeof(p));
  }
  void sendBye(const sockaddr_in &to) {
    ByePacket p{};
    p.header = MakeHeader(Bye, self_, session_, ++seq_);
    sendTo(to, &p, sizeof(p));
  }
  bool sendPose(double now) {
    PosePacket p{};
    {
      Lock lock(lock_);
      if (now - outgoingTime_ > 1) return false; // Stale: the game stopped publishing.
      p.pose = outgoing_;
    }
    if (!host_ && !joined_) return false;
    p.header = MakeHeader(Pose, self_, session_, ++seq_);
    memcpy(p.name, name_, sizeof(p.name));
    if (host_) for (const auto &peer : peers_) sendTo(peer.addr, &p, sizeof(p));
    else sendTo(hostAddr_, &p, sizeof(p));
    return true;
  }
  void keepAlive() {
    if (host_) for (const auto &peer : peers_) sendWelcome(peer.addr, peer.id);
    else if (joined_) sendHello();
  }

  // ---- receiving ----
  static bool Same(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
  }
  Peer *findPeer(const sockaddr_in &from) {
    for (auto &peer : peers_)
      if (Same(peer.addr, from)) return &peer;
    return nullptr;
  }
  void drain() {
    alignas(8) char buffer[2048];
    for (;;) {
      sockaddr_in from{};
      int fromBytes = sizeof(from);
      const int bytes = recvfrom(socket_, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&from), &fromBytes);
      if (bytes == SOCKET_ERROR) {
        // WSAECONNRESET reports an earlier send to a closed port; keep reading.
        if (WSAGetLastError() == WSAECONNRESET) continue;
        return;
      }
      if (fromBytes != sizeof(from) || from.sin_family != AF_INET) continue;
      const uint8_t type = Classify(buffer, bytes);
      if (!type) continue;
      Header header;
      memcpy(&header, buffer, sizeof(header));
      if (host_) hostReceive(type, header, buffer, from);
      else clientReceive(type, header, buffer, from);
    }
  }
  void hostReceive(uint8_t type, const Header &header, const char *data, const sockaddr_in &from) {
    const double now = Now();
    Peer *peer = findPeer(from);
    if (type == Hello) {
      if (!peer) {
        uint8_t id = 0;
        for (uint8_t candidate = 2; candidate <= kMaxPlayers && !id; ++candidate) {
          bool used = false;
          for (const auto &p : peers_) used |= p.id == candidate;
          if (!used) id = candidate;
        }
        if (!id) {
          RejectPacket reject{};
          reject.header = MakeHeader(Reject, 1, session_, ++seq_);
          reject.reason = Full;
          sendTo(from, &reject, sizeof(reject));
          return;
        }
        peers_.push_back({from, id, now});
        peer = &peers_.back();
        updatePlayers();
      }
      peer->lastSeen = now;
      sendWelcome(from, peer->id);
      return;
    }
    if (!peer || header.session != session_ || header.sender != peer->id) return;
    peer->lastSeen = now;
    if (type == Bye) {
      removePeer(peer->id);
      return;
    }
    if (type != Pose) return;
    PosePacket packet;
    memcpy(&packet, data, sizeof(packet));
    if (!ValidPose(packet.pose)) return;
    if (!store(packet, now)) return;
    // Relay to everyone else; the packet keeps its original sender id.
    for (const auto &other : peers_)
      if (other.id != peer->id) sendTo(other.addr, &packet, sizeof(packet));
  }
  void clientReceive(uint8_t type, const Header &header, const char *data, const sockaddr_in &from) {
    if (!Same(from, hostAddr_)) return; // Only the host talks to a joiner.
    const double now = Now();
    if (type == Welcome) {
      WelcomePacket packet;
      memcpy(&packet, data, sizeof(packet));
      if (packet.assigned < 2 || packet.assigned > kMaxPlayers) return;
      if (!joined_ || session_ != header.session) {
        session_ = header.session;
        self_ = packet.assigned;
        joined_ = true;
        setStatus(u8"已加入主机，玩家编号 " + std::to_string(self_));
      }
      lastHostSeen_ = now;
      return;
    }
    if (type == Reject) {
      RejectPacket packet;
      memcpy(&packet, data, sizeof(packet));
      setStatus(packet.reason == Full ? u8"主机已满（最多 4 人），稍后自动重试" : u8"协议版本不一致");
      return;
    }
    if (!joined_ || header.session != session_) return;
    lastHostSeen_ = now;
    if (type == Bye && header.sender == 1) {
      dropHost(u8"主机已关闭，正在等待重新开启…");
      return;
    }
    if (type != Pose || header.sender == self_ || header.sender < 1 || header.sender > kMaxPlayers) return;
    PosePacket packet;
    memcpy(&packet, data, sizeof(packet));
    if (ValidPose(packet.pose)) store(packet, now);
  }
  // Returns false for duplicates and out-of-order packets.
  bool store(const PosePacket &packet, double now) {
    Lock lock(lock_);
    auto &remote = remotes_[packet.header.sender];
    if (remote.any && !Newer(packet.header.seq, remote.seq)) return false;
    remote.any = true;
    remote.seq = packet.header.seq;
    remote.lastSeen = now;
    CopyName(remote.name, packet.name);
    remote.buffer.push(now, packet.pose);
    return true;
  }
  void removePeer(uint8_t id) {
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Peer &p) { return p.id == id; }), peers_.end());
    {
      Lock lock(lock_);
      remotes_.erase(id);
    }
    updatePlayers();
  }
  void updatePlayers() {
    Lock lock(lock_);
    players_ = 1 + int(peers_.size());
  }
  void dropHost(const char *reason) {
    joined_ = false;
    session_ = 0;
    self_ = 0;
    Lock lock(lock_);
    remotes_.clear();
    players_ = 0;
    status_ = reason;
  }
  void expire(double now) {
    if (host_) {
      std::vector<uint8_t> gone;
      for (const auto &peer : peers_)
        if (now - peer.lastSeen > kPeerTimeout) gone.push_back(peer.id);
      for (auto id : gone) removePeer(id);
    } else if (joined_ && now - lastHostSeen_ > kPeerTimeout) {
      dropHost(u8"与主机失去连接，正在重连…");
    }
    Lock lock(lock_);
    for (auto it = remotes_.begin(); it != remotes_.end();)
      it = now - it->second.lastSeen > kRemoteTimeout ? remotes_.erase(it) : std::next(it);
    if (!host_ && joined_) players_ = 1 + int(remotes_.size());
  }
};

} // namespace mirror_net
