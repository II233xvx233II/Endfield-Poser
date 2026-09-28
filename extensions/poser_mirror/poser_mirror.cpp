// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// Endfield Poser 外部可选扩展：人物镜像与局域网投影。
//
// 本地镜像（第 1 步）：把当前操控角色的动作复制到一名小队队员上。
// 局域网投影（第 2 步）：一人创建主机，其他人加入；每名其他玩家显示在本机
// 小队的一个空闲、非操控位置上。只同步各自角色的姿态，不涉及游戏服务器通信。
//
// 放进 <游戏目录>\plugin\ 即由代理加载器载入；没有 poser.dll（或版本过旧）时
// 什么也不做。只依赖 sdk/poser_extension.h 的 C ABI。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "net_session.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "poser_extension.h"
#include "pose_buffer.h"

namespace {

using mirror_net::Now;

const PoserExtensionApi *api = nullptr;
// AGPL-3.0 section 13: remote players can reach the source from the panel.
const char kSource[] = "https://github.com/II233xvx233II/Endfield-Poser/tree/mirror-extension";

enum Mode : int32_t { Off, Mirror, Host, Join };

// Touched only from onFrame / onGui, which the host never runs concurrently.
struct App {
  int32_t mode = Off, applied = Off;
  bool connect = false;
  std::string status = u8"未启用";

  // Local mirror
  int32_t slot = 1, mirrorSlot = -1;
  float right = 1.2f, up = 0, forward = 0, yaw = 0, delayMs = 0;
  double mirrorRetry = 0;
  mirror_pose::PoseBuffer history;

  // LAN
  char name[mirror_net::kNameBytes] = {};
  char address[64] = {};
  char port[8] = {};
  int32_t placement = 0; // 0 world position, 1 beside me
  float bufferMs = 100;
  mirror_net::Session session;
  std::map<uint8_t, int> assigned; // remote player id -> squad slot
  std::array<double, POSER_SQUAD_SLOTS> retryAt{};
  std::vector<std::string> players; // panel lines, rebuilt every frame
};
App &app = *new App; // Leaked: never torn down under the loader lock.

void StopMirror() {
  if (app.mirrorSlot >= 0) api->releasePuppet(app.mirrorSlot);
  app.mirrorSlot = -1;
  app.history.clear();
}
void StopNet() {
  for (const auto &entry : app.assigned) api->releasePuppet(entry.second);
  app.assigned.clear();
  app.players.clear();
  app.session.stop();
}
void Connect() {
  app.connect = false;
  StopNet();
  const int port = atoi(app.port);
  if (port < 1024 || port > 65535) { app.status = u8"端口需在 1024–65535 之间"; return; }
  if (!app.name[0]) mirror_net::CopyName(app.name, u8"玩家");
  if (app.mode == Join && !app.address[0]) { app.status = u8"输入主机 IP 后点击“连接”"; return; }
  std::string error;
  const bool ok = app.mode == Host ? app.session.startHost(uint16_t(port), app.name, error)
                                   : app.session.startJoin(app.address, uint16_t(port), app.name, error);
  if (!ok) app.status = error;
}
void SwitchMode() {
  StopMirror();
  StopNet();
  app.applied = app.mode;
  app.status = app.mode == Mirror ? u8"本地镜像" : u8"未启用";
  if (app.mode == Host || (app.mode == Join && app.address[0])) Connect();
  else if (app.mode == Join) app.status = u8"输入主机 IP 后点击“连接”";
}

// ---- 本地镜像（第 1 步） ----
void RunMirror(double now) {
  auto &m = app;
  if (m.mirrorSlot >= 0 && m.mirrorSlot != m.slot) StopMirror();
  if (m.mirrorSlot < 0) {
    if (now < m.mirrorRetry) return;
    if (!api->acquirePuppet(m.slot)) {
      m.status = api->status();
      m.mirrorRetry = now + 1;
      return;
    }
    m.mirrorSlot = m.slot;
    m.history.clear();
  }
  PoserHumanPose pose{};
  pose.size = sizeof(pose);
  if (api->captureControlled(&pose)) m.history.push(now, pose);
  else m.status = api->status();
  const double readAt = now - m.delayMs / 1000.0;
  m.history.trim(readAt - .5);
  if (!m.history.sample(readAt, pose)) return;
  if (!api->applyPuppet(m.mirrorSlot, &pose, PoserVec3{m.right, m.up, m.forward}, m.yaw)) {
    m.status = api->status();
    m.mirrorSlot = -1; // The host may already have handed the member back.
    m.history.clear();
    m.mirrorRetry = now + 1;
    return;
  }
  char text[96];
  snprintf(text, sizeof(text), u8"镜像中：第 %d 位（缓冲 %d 帧）", m.mirrorSlot + 1, int(m.history.size()));
  m.status = text;
}

// ---- 局域网投影（第 2 步） ----
void RunNet(double now) {
  PoserHumanPose own{};
  own.size = sizeof(own);
  const bool haveOwn = api->captureControlled(&own) != 0;
  if (haveOwn) app.session.publish(own);
  auto views = app.session.sample(now - app.bufferMs / 1000.0);
  std::sort(views.begin(), views.end(), [](const auto &a, const auto &b) { return a.id < b.id; });

  // Each other player takes the next free slot that is not the controlled one.
  PoserSquadInfo squad{};
  squad.size = sizeof(squad);
  api->getSquad(&squad);
  std::vector<int> free;
  for (int s = 0; s < POSER_SQUAD_SLOTS; ++s)
    if (squad.valid && squad.ready[s] && s != squad.controlledSlot) free.push_back(s);
  std::map<uint8_t, int> desired;
  for (size_t k = 0; k < views.size() && k < free.size(); ++k) desired[views[k].id] = free[k];
  for (auto it = app.assigned.begin(); it != app.assigned.end();) {
    const auto want = desired.find(it->first);
    if (want == desired.end() || want->second != it->second) {
      api->releasePuppet(it->second);
      it = app.assigned.erase(it);
    } else {
      ++it;
    }
  }

  app.players.clear();
  char line[192];
  for (size_t k = 0; k < views.size(); ++k) {
    const auto &view = views[k];
    const auto want = desired.find(view.id);
    if (want == desired.end()) {
      snprintf(line, sizeof(line), u8"#%d %s：小队没有空位，未显示", view.id, view.name);
      app.players.push_back(line);
      continue;
    }
    const int slot = want->second;
    if (!app.assigned.count(view.id)) {
      if (now < app.retryAt[slot]) continue;
      if (!api->acquirePuppet(slot)) {
        app.retryAt[slot] = now + 1;
        snprintf(line, sizeof(line), u8"#%d %s：%s", view.id, view.name, api->status());
        app.players.push_back(line);
        continue;
      }
      app.assigned[view.id] = slot;
    }
    PoserHumanPose pose = view.pose;
    if (app.placement == 1 && haveOwn) {
      const PoserVec3 side = mirror_pose::Rotate(own.bodyRotation, PoserVec3{1.2f * float(k + 1), 0, 0});
      pose.rootPosition = {own.rootPosition.x + side.x, own.rootPosition.y + side.y, own.rootPosition.z + side.z};
    }
    if (!api->applyPuppet(slot, &pose, PoserVec3{0, 0, 0}, 0)) {
      app.assigned.erase(view.id);
      app.retryAt[slot] = now + 1;
      snprintf(line, sizeof(line), u8"#%d %s：%s", view.id, view.name, api->status());
      app.players.push_back(line);
      continue;
    }
    if (haveOwn && app.placement == 0)
      snprintf(line, sizeof(line), u8"#%d %s → 第 %d 位，距离 %.0f 米", view.id, view.name, slot + 1,
               mirror_pose::Distance(own.rootPosition, view.pose.rootPosition));
    else
      snprintf(line, sizeof(line), u8"#%d %s → 第 %d 位", view.id, view.name, slot + 1);
    app.players.push_back(line);
  }
  // Keep a local message (bad port, missing IP) until a session is started.
  if (app.session.running()) app.status = app.session.status();
}

void OnFrame(void *) {
  if (app.mode != app.applied) SwitchMode();
  if (app.connect && (app.mode == Host || app.mode == Join)) Connect();
  const double now = Now();
  if (app.mode == Mirror) RunMirror(now);
  else if (app.mode == Host || app.mode == Join) RunNet(now);
}

void DrawMirror() {
  PoserSquadInfo squad{};
  squad.size = sizeof(squad);
  api->getSquad(&squad);
  char labels[POSER_SQUAD_SLOTS][112];
  const char *items[POSER_SQUAD_SLOTS];
  for (int i = 0; i < POSER_SQUAD_SLOTS; ++i) {
    snprintf(labels[i], sizeof(labels[i]), u8"第 %d 位 · %s%s", i + 1,
             squad.ready[i] ? squad.names[i] : u8"空位 / 未就绪",
             i == squad.controlledSlot ? u8"（操控中）" : "");
    items[i] = labels[i];
  }
  api->uiCombo(u8"投影到##slot", &app.slot, items, POSER_SQUAD_SLOTS);
  api->uiSliderFloat(u8"右侧距离 (米)", &app.right, -3, 3, "%.2f");
  api->uiSliderFloat(u8"前方距离 (米)", &app.forward, -3, 3, "%.2f");
  api->uiSliderFloat(u8"高度 (米)", &app.up, -1, 1, "%.2f");
  api->uiSliderFloat(u8"朝向偏移 (度)", &app.yaw, -180, 180, "%.0f");
  api->uiSliderFloat(u8"模拟延迟 (毫秒)", &app.delayMs, 0, 2000, "%.0f");
  if (api->uiButton(u8"恢复默认")) {
    app.right = 1.2f;
    app.up = app.forward = app.yaw = app.delayMs = 0;
  }
}
void DrawNet() {
  api->uiInputText(u8"玩家名", app.name, sizeof(app.name));
  if (app.mode == Join) api->uiInputText(u8"主机 IP", app.address, sizeof(app.address));
  api->uiInputText(u8"端口", app.port, sizeof(app.port));
  if (api->uiButton(app.session.running() ? u8"重新连接（应用上面的修改）" : u8"连接")) app.connect = true;
  static const char *const placements[] = {u8"世界坐标（需在同一地图）", u8"跟在我身边"};
  api->uiCombo(u8"显示位置", &app.placement, placements, 2);
  api->uiSliderFloat(u8"缓冲延迟 (毫秒)", &app.bufferMs, 0, 500, "%.0f");
  api->uiSeparator();
  if (app.players.empty()) api->uiTextDisabled(u8"暂无其他玩家");
  for (const auto &line : app.players) api->uiText(line.c_str());
  if (app.mode == Host)
    api->uiTextDisabled(u8"其他人在同一局域网内输入上面的本机地址加入。首次开启时，"
                        u8"请在 Windows 防火墙弹窗中允许“专用网络”访问。");
  api->uiTextDisabled(u8"每名其他玩家显示在小队的一个空闲、非操控位置上（最多 3 人）。");
}
void OnGui(void *) {
  static const char *const modes[] = {u8"关闭", u8"本地镜像", u8"局域网 · 创建主机", u8"局域网 · 加入主机"};
  api->uiCombo(u8"模式", &app.mode, modes, 4);
  if (app.mode == Mirror) DrawMirror();
  else if (app.mode == Host || app.mode == Join) DrawNet();
  api->uiTextDisabled(app.status.c_str());
  api->uiTextDisabled(u8"多人 MMD 播放、切换操控角色或小队变化时，被接管的队员会自动恢复。");
  api->uiSeparator();
  api->uiTextDisabled(u8"本扩展按 AGPL-3.0 开源，源码：");
  api->uiTextDisabled(kSource);
}

DWORD WINAPI ConnectHost(LPVOID) {
  // poser.dll may load after this DLL; wait up to two minutes for it.
  for (int attempt = 0; attempt < 240; ++attempt, Sleep(500)) {
    HMODULE host = nullptr;
    // Pin the host so the function table stays valid for the process lifetime.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"poser.dll", &host)) continue;
    auto get = reinterpret_cast<PoserGetExtensionApiFn>(GetProcAddress(host, POSER_EXTENSION_ENTRY));
    const PoserExtensionApi *found = get ? get(POSER_EXTENSION_API_VERSION) : nullptr;
    // Requires every field this build uses, including uiInputText.
    if (!found || found->size < sizeof(PoserExtensionApi)) {
      OutputDebugStringW(L"poser_mirror: poser.dll has no compatible extension API\n");
      return 0;
    }
    api = found;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    snprintf(app.name, sizeof(app.name), u8"玩家%04d", int(t.QuadPart % 10000));
    snprintf(app.port, sizeof(app.port), "%u", unsigned(mirror_net::kDefaultPort));
    PoserExtensionDesc desc{sizeof(desc), u8"人物镜像 / 局域网投影", nullptr, OnFrame, OnGui};
    api->log(api->registerExtension(&desc) ? "poser_mirror registered" : "poser_mirror registration failed");
    return 0;
  }
  return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    if (HANDLE thread = CreateThread(nullptr, 0, ConnectHost, nullptr, 0, nullptr)) CloseHandle(thread);
  }
  return TRUE;
}
