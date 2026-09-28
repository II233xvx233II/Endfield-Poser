#pragma once
// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// 扩展投影（联机第 1 步的宿主侧）：把与角色无关的人形姿态写到小队队员上。
// 队员的保存 / 冻结 / 恢复沿用多人播放器（mmd_squad.h）；姿态格式见
// sdk/poser_extension.h。所有函数都须在持有 g_poseMutex 时调用。
//
// 编码：body = 根世界旋转 * T 姿身体基（x 右 / y 上 / z 前）；
//   每根人形骨 d = inverse(body) * 世界旋转 * inverse(T 姿旋转) * 身体基。
// 解码时换成目标角色自己的身体基和 T 姿，因此两个角色的局部轴不必一致。
#include "game/mmd_squad.h"
#include "poser_extension.h"

#include <cmath>

namespace poser_puppet {

struct Rig {
  void *animator = nullptr, *root = nullptr;
  std::vector<AllBone> bones;
  mmd::RetargetProfile profile;
  Quat basis;        // T-pose body frame in root space
  float hipsHeight = 1;
};
struct Puppet {
  std::unique_ptr<MmdSquadActor> actor;
  Rig rig;
  int owner = 0;
};
struct State {
  std::array<Puppet, 4> puppets;
  Rig source;
  void *sourceFailed = nullptr;
  double sourceRetry = 0, nextRoster = 0;
  std::string sourceError;
  PoserSquadInfo squad{};
  std::string status;
};
// Leaked like g_squad: never destroyed while the process exits.
static State &state = *new State;

// Eyes and jaw belong to the face system; step 1 leaves them untouched.
static bool SkipRole(int role) { return role == 21 || role == 22 || role == 23; }
static int Fail(const std::string &reason) { state.status = reason; return 0; }

static Vec3 ToVec(PoserVec3 v) { return {v.x, v.y, v.z}; }
static Quat ToQuat(PoserQuat q) { return {q.x, q.y, q.z, q.w}; }
static PoserVec3 FromVec(Vec3 v) { return {v.x, v.y, v.z}; }
static PoserQuat FromQuat(Quat q) { return {q.x, q.y, q.z, q.w}; }

static bool Finite(PoserVec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
static bool Usable(PoserQuat q) {
  if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return false;
  float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return n > .25f && n < 4;
}
// Poses may come from the network in later steps: reject anything malformed.
static bool ValidPose(const PoserHumanPose &pose) {
  if (pose.size < sizeof(PoserHumanPose) || (pose.boneMask >> POSER_HUMAN_BONES) != 0) return false;
  if (!Finite(pose.rootPosition) || !Finite(pose.hipsPosition) || !Usable(pose.bodyRotation)) return false;
  if (Len(ToVec(pose.hipsPosition)) > 10) return false;
  for (int r = 0; r < POSER_HUMAN_BONES; ++r)
    if ((pose.boneMask >> r & 1) && !Usable(pose.rotations[r])) return false;
  return true;
}

static void SetLocalRotation(void *transform, Quat rotation) {
  __try {
    if (!g_transform_set_localRotation || !UnityObjAlive(transform)) return;
    void *args[] = {&rotation};
    Invoke(g_transform_set_localRotation, transform, args);
  } __except (1) {
  }
}

static bool BuildRig(const poser_squad::Member &member, Rig &rig, std::string &error) {
  rig = Rig{};
  if (!member.animator || !UnityObjAlive(member.animator)) { error = u8"角色模型尚未就绪"; return false; }
  {
    MmdSquadRigScope view(member);
    RebuildAllBones();
    RebuildHumanBones();
    rig.bones = s_allBones;
    rig.profile = MmdCurrentProfile();
    if (!MmdBindCalibration(rig.profile) && !MmdLoadCalibration(rig.profile)) {
      error = u8"角色无法自动适配，且缺少备用 T 姿校准";
      return false;
    }
  }
  const auto &p = rig.profile;
  if (rig.bones.empty() || p.bones.size() != rig.bones.size() || !p.valid()) {
    error = u8"角色骨架不完整";
    return false;
  }
  auto at = [&](int role) { return p.bones[p.roles[role]].restPos; };
  Vec3 right = Norm(at(14) - at(13)), up = at(10) - at(0);
  up = Norm(up - right * Dot(up, right));
  if (Len(right) < .9f || Len(up) < .9f) { error = u8"无法确定角色朝向"; return false; }
  rig.basis = mmd::Basis(right, up, Cross(right, up));
  rig.hipsHeight = Dot(at(0), up);
  if (!(rig.hipsHeight > .05f)) { error = u8"无法确定角色髋部高度"; return false; }
  rig.animator = member.animator;
  rig.root = rig.bones[0].transform;
  return true;
}
static bool RigAlive(const Rig &rig) {
  if (!rig.animator || !UnityObjAlive(rig.animator) || !UnityObjAlive(rig.root)) return false;
  for (int r = 0; r < POSER_HUMAN_BONES; ++r) {
    int i = rig.profile.roles[r];
    if (i >= 0 && !UnityObjAlive(rig.bones[i].transform)) return false;
  }
  return true;
}

// ---- 源：当前操控角色 ----
static bool EnsureSource() {
  auto &src = state.source;
  if (!g_charAnimator || !UnityObjAlive(g_charAnimator)) return Fail(u8"等待操控角色");
  if (src.animator == g_charAnimator && RigAlive(src)) return true;
  // Rebuilding logs and rereads calibration; do not retry a failure every frame.
  if (state.sourceFailed == g_charAnimator && MmdNow() < state.sourceRetry) return Fail(state.sourceError);
  poser_squad::Member self{g_mainCharEntity, g_charAnimator, g_charAnimComp};
  std::string error;
  try {
    if (BuildRig(self, src, error)) {
      state.sourceFailed = nullptr;
      Log("[PUPPET] source rig ready animator=%p bones=%d", g_charAnimator, (int)src.bones.size());
      return true;
    }
  } catch (const std::exception &e) {
    error = e.what();
  }
  src = Rig{};
  state.sourceFailed = g_charAnimator;
  state.sourceRetry = MmdNow() + 2;
  state.sourceError = u8"操控角色：" + error;
  return Fail(state.sourceError);
}
static int CaptureControlled(PoserHumanPose &out) {
  if (CharacterSwitchInProgress()) return Fail(u8"正在切换角色");
  if (!EnsureSource()) return 0;
  const auto &rig = state.source;
  const Quat body = NormQ(GetBoneWorldRot(rig.root) * rig.basis), inverse = Conj(body);
  const Vec3 origin = GetBoneWorldPos(rig.root);
  memset(&out, 0, sizeof(out));
  out.size = sizeof(out);
  out.rootPosition = FromVec(origin);
  out.bodyRotation = FromQuat(body);
  for (int r = 0; r < POSER_HUMAN_BONES; ++r) {
    int i = rig.profile.roles[r];
    if (i < 0 || SkipRole(r)) continue;
    Quat world = GetBoneWorldRot(rig.bones[i].transform);
    out.rotations[r] = FromQuat(NormQ(inverse * world * Conj(rig.profile.bones[i].restRot) * rig.basis));
    out.boneMask |= 1ull << r;
  }
  Vec3 hips = GetBoneWorldPos(rig.bones[rig.profile.roles[0]].transform) - origin;
  out.hipsPosition = FromVec((inverse * hips) * (1.f / rig.hipsHeight));
  return 1;
}

// ---- 目标：被接管的小队队员 ----
// Only the squad player captures teammates. Single-character MMD touches the
// controlled character alone, so it may drive the source of a mirror.
static bool MmdBusy() { return g_squad.active || g_squad.pending.active; }
static void Release(int slot, const char *reason = nullptr) {
  if (slot < 0 || slot >= 4) return;
  auto &p = state.puppets[slot];
  if (!p.actor) return;
  auto &a = *p.actor;
  const bool alive = !RuntimeClosing() && UnityObjAlive(a.saved.animator) && UnityObjAlive(a.saved.root);
  if (alive && a.saved.active) {
    for (const auto &bone : a.saved.transforms) MmdRawPose(bone.transform, bone.pos, bone.rot);
    MmdRawPose(a.saved.root, a.saved.rootPos, a.saved.rootRot);
    for (const auto &component : a.saved.components) MmdEnable(component.component, component.enabled);
  }
  Log("[PUPPET] released slot=%d owner=%d restored=%d reason=%s", slot + 1, p.owner, (int)alive,
      reason ? reason : "request");
  p = Puppet{}; // Frees the retained GC handles.
  if (reason) state.status = reason;
}
static void ReleaseAll(const char *reason) {
  for (int slot = 0; slot < 4; ++slot) Release(slot, reason);
}
static int Acquire(int slot, int owner) {
  if (slot < 0 || slot >= 4) return Fail(u8"小队位置无效");
  if (!ClothOnMainThread()) return Fail(u8"只能在游戏线程接管队员");
  if (MmdBusy()) return Fail(u8"MMD 多人播放器正在使用小队，暂不能接管队员");
  auto &p = state.puppets[slot];
  if (p.actor) return p.owner == owner ? 1 : Fail(u8"该位置已被其他扩展接管");
  const auto roster = poser_squad::Read();
  if (!roster.valid) return Fail(poser_squad::status);
  if (slot >= roster.count) return Fail(u8"该小队位置是空位");
  const auto member = roster.members[slot];
  if (!member.entity || !member.animator || !UnityObjAlive(member.animator)) return Fail(u8"该位置的模型尚未就绪");
  if (member.animator == g_charAnimator) return Fail(u8"不能接管当前操控角色，请选择其他小队位置");
  for (const auto &other : state.puppets)
    if (other.actor && other.actor->member.animator == member.animator) return Fail(u8"该队员已被接管");
  try {
    p.actor = std::make_unique<MmdSquadActor>();
    p.owner = owner;
    MmdSquadCaptureActor(*p.actor, member);
    // Arm restoration before the first disable so a partial freeze rolls back.
    p.actor->saved.active = true;
    for (const auto &component : p.actor->saved.components) {
      WriteBehaviourEnabled(component.component, false);
      bool enabled = true;
      if (!ReadBehaviourEnabled(component.component, enabled) || enabled)
        throw std::runtime_error(u8"无法冻结队员动画");
    }
    std::string error;
    if (!BuildRig(member, p.rig, error)) throw std::runtime_error(u8"队员：" + error);
  } catch (const std::exception &e) {
    const std::string reason = e.what();
    Release(slot);
    return Fail(reason);
  }
  Log("[PUPPET] acquired slot=%d owner=%d animator=%p bones=%d", slot + 1, owner, member.animator,
      (int)p.rig.bones.size());
  state.status = u8"已接管第 " + std::to_string(slot + 1) + u8" 位";
  return 1;
}
static int Apply(int slot, int owner, const PoserHumanPose &pose, PoserVec3 offset, float yawDegrees) {
  if (slot < 0 || slot >= 4) return Fail(u8"小队位置无效");
  auto &p = state.puppets[slot];
  if (!p.actor) return state.status.empty() ? Fail(u8"该位置未被接管") : 0;
  if (p.owner != owner) return Fail(u8"该位置由其他扩展接管");
  if (!ClothOnMainThread() || CharacterSwitchInProgress()) return Fail(u8"当前不能写入队员姿态");
  if (!ValidPose(pose) || !Finite(offset) || !std::isfinite(yawDegrees)) return Fail(u8"姿态数据无效");
  if (!RigAlive(p.rig)) { Release(slot, u8"队员骨架已变化，投影已恢复"); return 0; }
  const auto &rig = p.rig;
  const Quat source = NormQ(ToQuat(pose.bodyRotation));
  const Quat body = NormQ(Quat::AxisAngle({0, 1, 0}, yawDegrees * 3.14159265f / 180.f) * source);
  const Vec3 origin = ToVec(pose.rootPosition) + source * ToVec(offset);
  if (!MmdSquadWorldPose(p.actor->saved.root, origin, NormQ(body * Conj(rig.basis)))) {
    Release(slot, u8"无法设置队员位置，投影已恢复");
    return 0;
  }
  // Bones are ordered parent-first, so each parent's world rotation is final.
  for (size_t i = 1; i < rig.bones.size(); ++i) {
    const int r = rig.profile.bones[i].role;
    if (r < 0 || r >= POSER_HUMAN_BONES || SkipRole(r) || !(pose.boneMask >> r & 1)) continue;
    const Quat world = body * NormQ(ToQuat(pose.rotations[r])) * Conj(rig.basis) * rig.profile.bones[i].restRot;
    SetLocalRotation(rig.bones[i].transform, NormQ(Conj(GetBoneWorldRot(rig.bones[i].parent)) * world));
  }
  SetBoneWorldPos(rig.bones[rig.profile.roles[0]].transform,
                  origin + body * (ToVec(pose.hipsPosition) * rig.hipsHeight));
  return 1;
}

// ---- 每帧维护：冲突检测、队员校验、保持动画关闭、小队快照 ----
static void RefreshSquad(const poser_squad::Snapshot &roster) {
  auto &info = state.squad;
  info = PoserSquadInfo{};
  info.size = sizeof(info);
  info.valid = roster.valid;
  info.count = roster.valid ? roster.count : 0;
  info.controlledSlot = -1;
  for (int i = 0; i < 4; ++i) {
    const auto &m = roster.members[i];
    info.puppet[i] = state.puppets[i].actor != nullptr;
    if (!roster.valid || i >= roster.count || !m.animator || !UnityObjAlive(m.animator)) continue;
    info.ready[i] = 1;
    if (m.animator == g_charAnimator) info.controlledSlot = i;
    GetBoneName(SafeGetComponentTransform(m.animator), info.names[i], sizeof(info.names[i]));
  }
}
// wanted: an extension is registered. Without one this costs nothing.
static void Tick(bool wanted) {
  if (!ClothOnMainThread()) return;
  bool any = false;
  for (const auto &p : state.puppets) any |= p.actor != nullptr;
  if (any && MmdBusy()) { ReleaseAll(u8"MMD 多人播放开始，投影已恢复"); any = false; }
  const double now = MmdNow();
  if (!any && (!wanted || now < state.nextRoster)) return;
  state.nextRoster = now + .5;
  const auto roster = poser_squad::Read();
  for (int slot = 0; slot < 4; ++slot) {
    auto &p = state.puppets[slot];
    if (!p.actor) continue;
    const auto &a = *p.actor;
    const auto &m = roster.members[slot];
    if (!roster.valid || slot >= roster.count || m.entity != a.member.entity || m.animator != a.member.animator)
      Release(slot, u8"小队顺序或队员实例变化，投影已恢复");
    else if (a.member.animator == g_charAnimator)
      Release(slot, u8"该队员已成为操控角色，投影已恢复");
    else if (!UnityObjAlive(a.saved.animator) || !UnityObjAlive(a.saved.root))
      Release(slot, u8"队员实例已失效，投影已恢复");
    else // The game re-enables its writers; keep them off like the squad player.
      for (const auto &component : a.saved.components) MmdEnable(component.component, false);
  }
  RefreshSquad(roster);
}

} // namespace poser_puppet
