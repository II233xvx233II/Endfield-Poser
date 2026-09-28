// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// Endfield Poser 外部可选扩展：人物镜像（联机投影第 1 步）。
//
// 每帧读取当前操控角色的姿态，写到指定的小队队员上，让队员像“投影”一样
// 在身旁同步动作。姿态先经过一个带时间戳的缓冲再插值取出，“模拟延迟”即为
// 第 2 步网络传输预留的抖动缓冲：届时把本地采集换成网络接收即可。
//
// 放进 <游戏目录>\plugin\ 即由代理加载器载入；没有 poser.dll（或版本过旧）时
// 什么也不做。只依赖 sdk/poser_extension.h 的 C ABI。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

#include "poser_extension.h"

namespace {

const PoserExtensionApi *api = nullptr;

struct Sample {
  double time = 0;
  PoserHumanPose pose{};
};
// Touched only from onFrame / onGui, which the host never runs concurrently.
struct Mirror {
  int32_t enabled = 0;
  int32_t slot = 1;     // wanted squad slot, 0-based
  int32_t active = -1;  // slot currently taken over
  float right = 1.2f, up = 0, forward = 0, yaw = 0, delayMs = 0;
  double retryAt = 0;
  std::deque<Sample> history;
  std::string status = u8"未启用";
};
Mirror mirror;

PoserVec3 Lerp(PoserVec3 a, PoserVec3 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
// Normalized lerp along the shorter arc; consecutive frames are close together.
PoserQuat Nlerp(PoserQuat a, PoserQuat b, float t) {
  const float sign = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0 ? -1.f : 1.f;
  PoserQuat q{a.x + (b.x * sign - a.x) * t, a.y + (b.y * sign - a.y) * t,
              a.z + (b.z * sign - a.z) * t, a.w + (b.w * sign - a.w) * t};
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (length < 1e-6f) return a;
  return {q.x / length, q.y / length, q.z / length, q.w / length};
}
PoserHumanPose Blend(const PoserHumanPose &a, const PoserHumanPose &b, float t) {
  PoserHumanPose out = a;
  out.boneMask = a.boneMask & b.boneMask;
  out.rootPosition = Lerp(a.rootPosition, b.rootPosition, t);
  out.bodyRotation = Nlerp(a.bodyRotation, b.bodyRotation, t);
  out.hipsPosition = Lerp(a.hipsPosition, b.hipsPosition, t);
  for (int bone = 0; bone < POSER_HUMAN_BONES; ++bone)
    if (out.boneMask >> bone & 1) out.rotations[bone] = Nlerp(a.rotations[bone], b.rotations[bone], t);
  return out;
}
bool SampleAt(double time, PoserHumanPose &out) {
  const auto &h = mirror.history;
  if (h.empty()) return false;
  if (time <= h.front().time) { out = h.front().pose; return true; }
  for (size_t i = 1; i < h.size(); ++i)
    if (h[i].time >= time) {
      const double span = h[i].time - h[i - 1].time;
      const float t = span > 1e-6 ? float((time - h[i - 1].time) / span) : 1.f;
      out = Blend(h[i - 1].pose, h[i].pose, t);
      return true;
    }
  out = h.back().pose;
  return true;
}
void Trim(double now) {
  // Keep the pair that brackets the delayed read time, plus half a second.
  const double oldest = now - mirror.delayMs / 1000.0 - .5;
  while (mirror.history.size() > 2 && mirror.history[1].time < oldest) mirror.history.pop_front();
}

void Stop(const char *status) {
  if (mirror.active >= 0) api->releasePuppet(mirror.active);
  mirror.active = -1;
  mirror.history.clear();
  if (status) mirror.status = status;
}

void OnFrame(void *) {
  auto &m = mirror;
  const double now = api->now();
  if (!m.enabled) {
    if (m.active >= 0) Stop(u8"未启用");
    return;
  }
  if (m.active >= 0 && m.active != m.slot) Stop(nullptr);
  if (m.active < 0) {
    if (now < m.retryAt) return;
    if (!api->acquirePuppet(m.slot)) {
      m.status = api->status();
      m.retryAt = now + 1;
      return;
    }
    m.active = m.slot;
    m.history.clear();
  }
  Sample sample;
  sample.time = now;
  sample.pose.size = sizeof(PoserHumanPose);
  if (api->captureControlled(&sample.pose)) m.history.push_back(sample);
  else m.status = api->status();
  Trim(now);
  PoserHumanPose pose{};
  if (!SampleAt(now - m.delayMs / 1000.0, pose)) return;
  if (!api->applyPuppet(m.active, &pose, PoserVec3{m.right, m.up, m.forward}, m.yaw)) {
    m.status = api->status();
    m.active = -1; // The host may already have handed the member back.
    m.history.clear();
    m.retryAt = now + 1;
    return;
  }
  char text[96];
  snprintf(text, sizeof(text), u8"镜像中：第 %d 位（缓冲 %d 帧）", m.active + 1, int(m.history.size()));
  m.status = text;
}

void OnGui(void *) {
  auto &m = mirror;
  api->uiCheckbox(u8"启用人物镜像", &m.enabled);
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
  api->uiCombo(u8"投影到##slot", &m.slot, items, POSER_SQUAD_SLOTS);
  api->uiSliderFloat(u8"右侧距离 (米)", &m.right, -3, 3, "%.2f");
  api->uiSliderFloat(u8"前方距离 (米)", &m.forward, -3, 3, "%.2f");
  api->uiSliderFloat(u8"高度 (米)", &m.up, -1, 1, "%.2f");
  api->uiSliderFloat(u8"朝向偏移 (度)", &m.yaw, -180, 180, "%.0f");
  api->uiSliderFloat(u8"模拟延迟 (毫秒)", &m.delayMs, 0, 2000, "%.0f");
  if (api->uiButton(u8"恢复默认")) {
    m.right = 1.2f;
    m.up = m.forward = m.yaw = m.delayMs = 0;
  }
  api->uiTextDisabled(m.status.c_str());
  api->uiTextDisabled(u8"多人 MMD 播放、切换操控角色或小队变化时，会自动恢复该队员。");
}

DWORD WINAPI Connect(LPVOID) {
  // poser.dll may load after this DLL; wait up to two minutes for it.
  for (int attempt = 0; attempt < 240; ++attempt, Sleep(500)) {
    HMODULE host = nullptr;
    // Pin the host so the function table stays valid for the process lifetime.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"poser.dll", &host)) continue;
    auto get = reinterpret_cast<PoserGetExtensionApiFn>(GetProcAddress(host, POSER_EXTENSION_ENTRY));
    const PoserExtensionApi *found = get ? get(POSER_EXTENSION_API_VERSION) : nullptr;
    if (!found || found->size < sizeof(PoserExtensionApi)) {
      OutputDebugStringW(L"poser_mirror: poser.dll has no compatible extension API\n");
      return 0;
    }
    api = found;
    PoserExtensionDesc desc{sizeof(desc), u8"人物镜像（联机投影 · 第 1 步）", nullptr, OnFrame, OnGui};
    api->log(api->registerExtension(&desc) ? "poser_mirror registered" : "poser_mirror registration failed");
    return 0;
  }
  return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    if (HANDLE thread = CreateThread(nullptr, 0, Connect, nullptr, 0, nullptr)) CloseHandle(thread);
  }
  return TRUE;
}
