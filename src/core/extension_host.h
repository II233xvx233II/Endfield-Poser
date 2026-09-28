#pragma once
// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// 外部可选扩展的宿主：导出 PoserGetExtensionApi，按 sdk/poser_extension.h 的 C ABI
// 提供注册、每帧回调、面板回调和投影接口。扩展 DLL 由代理加载器从 plugin\ 载入，
// 本插件不主动加载任何扩展；没有扩展注册时这里不产生任何开销。
#include "poser_extension.h"
#include "game/puppet.h"

namespace poser_ext {

enum Context { Idle, InFrame, InGui };
struct Entry {
  int32_t id = 0;
  std::string name;
  void *user = nullptr;
  PoserCallback onFrame = nullptr, onGui = nullptr;
};
struct Host {
  std::vector<Entry> entries;
  int32_t nextId = 1, current = 0;
  Context context = Idle;
  DWORD thread = 0; // Thread running the current callback.
};
static Host &host = *new Host; // Leaked: extension DLLs are never unloaded.

static bool CallGuarded(PoserCallback fn, void *user) {
  host.thread = GetCurrentThreadId();
  __try {
    fn(user);
    return true;
  } __except (1) {
    return false;
  }
}
// A faulting extension keeps its registration (so the panel shows it) but
// loses its callbacks; its puppets are restored on the next frame.
static void Disable(int32_t id, const char *where) {
  for (auto &e : host.entries)
    if (e.id == id) {
      Log("[EXT] %s crashed in %s; callbacks disabled", e.name.c_str(), where);
      e.onFrame = e.onGui = nullptr;
      e.name += u8"（已停用：运行出错）";
    }
}
static bool Owned(int32_t id) {
  for (const auto &e : host.entries)
    if (e.id == id) return e.onFrame || e.onGui;
  return false;
}

// Before MmdTick: yield squad members to MMD before its start captures them.
static void BeforeMmd() { poser_puppet::Tick(!host.entries.empty()); }
// After MmdTick, on the game thread: extensions write their poses last.
static void AfterMmd() {
  if (!ClothOnMainThread()) return;
  for (int slot = 0; slot < 4; ++slot) {
    const auto &p = poser_puppet::state.puppets[slot];
    if (p.actor && !Owned(p.owner)) poser_puppet::Release(slot, u8"扩展已注销或停用，投影已恢复");
  }
  const auto list = host.entries;
  for (const auto &e : list) {
    if (!e.onFrame) continue;
    host.context = InFrame;
    host.current = e.id;
    const bool ok = CallGuarded(e.onFrame, e.user);
    host.context = Idle;
    host.current = 0;
    if (!ok) Disable(e.id, "onFrame");
  }
}
// Inside the main panel window.
static void DrawGui() {
  if (host.entries.empty() || !ImGui::CollapsingHeader(u8"扩展插件")) return;
  const auto list = host.entries;
  for (const auto &e : list) {
    ImGui::PushID(e.id);
    if (!e.onGui) {
      ImGui::TextDisabled("%s", e.name.c_str());
    } else if (ImGui::TreeNodeEx(e.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      host.context = InGui;
      host.current = e.id;
      const bool ok = CallGuarded(e.onGui, e.user);
      host.context = Idle;
      host.current = 0;
      ImGui::TreePop();
      if (!ok) Disable(e.id, "onGui");
    }
    ImGui::PopID();
  }
}

// ---- API table ----
static int32_t ApiRegister(const PoserExtensionDesc *desc) {
  if (!desc || desc->size < sizeof(PoserExtensionDesc) || !desc->name || !desc->name[0]) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_poseMutex);
  Entry e;
  e.id = host.nextId++;
  e.name.assign(desc->name, strnlen(desc->name, 64));
  e.user = desc->user;
  e.onFrame = desc->onFrame;
  e.onGui = desc->onGui;
  host.entries.push_back(e);
  Log("[EXT] registered id=%d name=%s", e.id, e.name.c_str());
  return e.id;
}
static void ApiUnregister(int32_t id) {
  std::lock_guard<std::recursive_mutex> lock(g_poseMutex);
  // Puppets are restored by AfterMmd on the game thread, never here: this may
  // be called from a thread that is not attached to IL2CPP.
  for (size_t i = 0; i < host.entries.size(); ++i)
    if (host.entries[i].id == id) {
      Log("[EXT] unregistered id=%d name=%s", id, host.entries[i].name.c_str());
      host.entries.erase(host.entries.begin() + i);
      return;
    }
}
static void ApiLog(const char *text) {
  if (text) Log("[EXT] %.1000s", text);
}
static double ApiNow() { return MmdNow(); }
static int32_t ApiGetSquad(PoserSquadInfo *out) {
  if (!out || out->size < sizeof(PoserSquadInfo)) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_poseMutex);
  *out = poser_puppet::state.squad;
  out->size = sizeof(PoserSquadInfo);
  return out->valid;
}
// Another thread may observe a callback in flight; only the calling thread counts.
static bool InContext(Context context) {
  return host.context == context && host.thread == GetCurrentThreadId();
}
static bool InFrameCall() {
  if (InContext(InFrame)) return true;
  poser_puppet::state.status = u8"只能在 onFrame 回调中调用";
  return false;
}
static int32_t ApiCapture(PoserHumanPose *out) {
  if (!InFrameCall()) return 0;
  if (!out || out->size < sizeof(PoserHumanPose)) return poser_puppet::Fail(u8"姿态结构体大小无效");
  try {
    return poser_puppet::CaptureControlled(*out);
  } catch (const std::exception &e) {
    return poser_puppet::Fail(e.what());
  }
}
static int32_t ApiAcquire(int32_t slot) {
  if (!InFrameCall()) return 0;
  try {
    return poser_puppet::Acquire(slot, host.current);
  } catch (const std::exception &e) {
    return poser_puppet::Fail(e.what());
  }
}
static int32_t ApiApply(int32_t slot, const PoserHumanPose *pose, PoserVec3 offset, float yawDegrees) {
  if (!InFrameCall()) return 0;
  if (!pose) return poser_puppet::Fail(u8"姿态为空");
  try {
    return poser_puppet::Apply(slot, host.current, *pose, offset, yawDegrees);
  } catch (const std::exception &e) {
    poser_puppet::Release(slot, e.what());
    return 0;
  }
}
static void ApiRelease(int32_t slot) {
  if (!InFrameCall() || slot < 0 || slot >= 4) return;
  if (poser_puppet::state.puppets[slot].owner == host.current) poser_puppet::Release(slot);
}
static const char *ApiStatus() {
  static thread_local std::string copy; // Stable even if the host updates status later.
  std::lock_guard<std::recursive_mutex> lock(g_poseMutex);
  copy = poser_puppet::state.status;
  return copy.c_str();
}

static bool InGuiCall() { return InContext(InGui); }
static void UiText(const char *text) {
  if (InGuiCall() && text) ImGui::TextUnformatted(text);
}
static void UiTextDisabled(const char *text) {
  if (InGuiCall() && text) ImGui::TextDisabled("%s", text);
}
static int32_t UiCheckbox(const char *label, int32_t *value) {
  if (!InGuiCall() || !label || !value) return 0;
  bool checked = *value != 0;
  const bool changed = ImGui::Checkbox(label, &checked);
  *value = checked;
  return changed;
}
static int32_t UiButton(const char *label) { return InGuiCall() && label && ImGui::Button(label); }
static int32_t UiSliderFloat(const char *label, float *value, float min, float max, const char *format) {
  return InGuiCall() && label && value && ImGui::SliderFloat(label, value, min, max, format ? format : "%.3f");
}
static int32_t UiCombo(const char *label, int32_t *current, const char *const *items, int32_t count) {
  if (!InGuiCall() || !label || !current || !items || count <= 0) return 0;
  int selected = *current;
  const bool changed = ImGui::Combo(label, &selected, items, count);
  *current = selected;
  return changed;
}
static void UiSameLine() {
  if (InGuiCall()) ImGui::SameLine();
}
static void UiSeparator() {
  if (InGuiCall()) ImGui::Separator();
}

} // namespace poser_ext

extern "C" __declspec(dllexport) const PoserExtensionApi *PoserGetExtensionApi(uint32_t version) {
  static const PoserExtensionApi api = {
      POSER_EXTENSION_API_VERSION, sizeof(PoserExtensionApi), POSER_VERSION,
      poser_ext::ApiRegister, poser_ext::ApiUnregister, poser_ext::ApiLog, poser_ext::ApiNow,
      poser_ext::ApiGetSquad, poser_ext::ApiCapture, poser_ext::ApiAcquire, poser_ext::ApiApply,
      poser_ext::ApiRelease, poser_ext::ApiStatus,
      poser_ext::UiText, poser_ext::UiTextDisabled, poser_ext::UiCheckbox, poser_ext::UiButton,
      poser_ext::UiSliderFloat, poser_ext::UiCombo, poser_ext::UiSameLine, poser_ext::UiSeparator};
  return version == POSER_EXTENSION_API_VERSION ? &api : nullptr;
}
