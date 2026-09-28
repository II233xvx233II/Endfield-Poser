#pragma once
#include "game/mmd_player.h"
#include "game/squad.h"
#include "math/mmd_squad.h"

struct MmdSquadSlot {
  bool enabled=true;
  mmd::MotionClip clip;
  std::string file,member,status;
  std::string calibration=u8"待读取校准";
  void *calibrationAnimator=nullptr;
  uint64_t calibrationSerial=0;
  bool calibrated=false;
  Vec3 offset{};
  float yaw=0,scale=1,height=0;
};
struct MmdSquadActor {
  poser_squad::Member member;
  MmdSession saved;
  std::vector<AllBone> bones;
  mmd::RetargetProfile profile;
  mmd::Retargeter mapper;
  std::shared_ptr<SMCActorState> face;
  bool editorFace=false;
  std::map<std::string,MmdMorphMapping> morphs;
  std::shared_ptr<const character_face::Profile> faceProfile;
  uint64_t faceGeneration=0;
  size_t libraryCount=0;
  bool faceHierarchyReady=false;
  float scale=.08f;
};
struct MmdSquadPlayer {
  bool show=false,hotkeys=false,active=false,refresh=true,loading=false;
  bool inPlace=false,stopRequested=false;
  float scale=.08f,height=0;
  mmd::IkMode ikMode=mmd::IkMode::FollowMotion;
  mmd::Timeline timeline;
  mmd::DeferredStart pending;
  mmd::SquadAnchor anchor;
  mmd::SquadIdentity identity;
  mmd::RigDefinition rig;
  std::array<MmdSquadSlot,4> slots;
  std::array<std::unique_ptr<MmdSquadActor>,4> actors;
  poser_squad::Snapshot roster;
  std::future<MmdLoadResult> loader;
  std::string status=u8"读取小队后，按第 1–4 位分配动作";
  uint64_t cameraSession=0;
  void *cameraOwner=nullptr;
  std::shared_ptr<GripReferences> cameraReferences;
  float cameraHeight=0;
  int cameraFollow=0;
  double nextRefresh=0;
};
static MmdSquadPlayer &g_squad=*new MmdSquadPlayer;

// Only used while preparing a rig on the game thread, under g_poseMutex.
// Restore every editor cache even if calibration or allocation throws.
struct MmdSquadRigScope {
  void *animator=g_charAnimator,*component=g_charAnimComp;
  BoneHandle human[kHumanBoneCount];
  Quat restRot[kHumanBoneCount];Vec3 restPos[kHumanBoneCount];
  int count=s_humanBoneCount,revision=s_bonesRev;
  bool rest=s_restCaptured;
  std::vector<AllBone> bones;
  explicit MmdSquadRigScope(const poser_squad::Member &member) {
    memcpy(human,s_humanBones,sizeof(human));memcpy(restRot,s_restRot,sizeof(restRot));memcpy(restPos,s_restPos,sizeof(restPos));
    bones.swap(s_allBones);g_charAnimator=member.animator;g_charAnimComp=member.component;
    s_humanBoneCount=0;s_restCaptured=false;
  }
  ~MmdSquadRigScope() {
    g_charAnimator=animator;g_charAnimComp=component;s_allBones.swap(bones);
    memcpy(s_humanBones,human,sizeof(human));memcpy(s_restRot,restRot,sizeof(restRot));memcpy(s_restPos,restPos,sizeof(restPos));
    s_humanBoneCount=count;s_bonesRev=revision;s_restCaptured=rest;
  }
};
static mmd::SquadIdentity MmdSquadIdentity(const poser_squad::Snapshot &roster) {
  mmd::SquadIdentity id;id.squad=reinterpret_cast<uintptr_t>(roster.squad);
  for(int i=0;i<4;++i) {
    id.entities[i]=reinterpret_cast<uintptr_t>(roster.members[i].entity);
    id.animators[i]=reinterpret_cast<uintptr_t>(roster.members[i].animator);
    id.enabled[i]=g_squad.slots[i].enabled&&!g_squad.slots[i].clip.empty();
  }
  return id;
}
static void MmdSquadRetain(MmdSession &session,void *object) {
  if(!object)return;
  if(!il2cpp_gchandle_new || !il2cpp_gchandle_free)throw std::runtime_error(u8"无法保留角色句柄，未开始多人播放");
  uint32_t h=il2cpp_gchandle_new(object,false);
  if(!h)throw std::runtime_error(u8"角色句柄保留失败");
  session.references->handles.push_back(h);
}
static bool MmdSquadWorldPose(void *root,Vec3 position,Quat rotation) {
  __try {
    if(!UnityObjAlive(root)||!g_transform_set_position||!g_transform_set_rotation)return false;
    void *p[]={&position},*q[]={&rotation};
    Invoke(g_transform_set_position,root,p);Invoke(g_transform_set_rotation,root,q);return true;
  } __except(1) {return false;}
}
static void MmdSquadCollectWriters(MmdSquadActor &actor) {
  // Use exact known writer classes; leave hair, cloth and tail simulation on.
  std::set<void *> writers{actor.member.animator,actor.member.component};
  for(const auto &bone:actor.bones) {
    void *go=Invoke(g_component_get_gameObject,bone.transform);
    void *type=il2cpp_type_get_object(il2cpp_class_get_type(g_componentClass));
    void *args[]={type};void *array=go?Invoke(g_gameObject_GetComponents,go,args):nullptr;
    for(int n=0;n<MmdArrayLength(array);++n) {
      void *c=MmdArrayObject(array,n);if(!c)continue;
      const char *name=il2cpp_class_get_name(il2cpp_object_get_class(c));
      if(name && (!strcmp(name,"BipedIK")||!strcmp(name,"GrounderBipedIK")||
          !strcmp(name,"LookAtComponent")||!strcmp(name,"TransformFollowDamper")||!strcmp(name,"AnimatorMono"))) writers.insert(c);
    }
  }
  for(auto component:writers)if(LiveBehaviour(component)) {
    bool enabled=false;if(!ReadBehaviourEnabled(component,enabled))throw std::runtime_error(u8"无法读取角色动画状态");
    MmdSquadRetain(actor.saved,component);actor.saved.components.push_back({component,enabled});
  }
  bool animatorCaptured=false;
  for(const auto &component:actor.saved.components)animatorCaptured|=component.component==actor.member.animator;
  if(!animatorCaptured)throw std::runtime_error(u8"无法保存队员的动画开关状态，未开始多人准备");
}
static void MmdSquadFaceMap(MmdSquadActor &actor,const mmd::MotionClip &clip) {
  const auto key=character_face::ModelKey(actor.profile.model);
  if(!actor.faceProfile)for(auto &profile:g_mmd.faceLibrary) if(profile->key==key) {actor.faceProfile=profile;break;}
  for(const auto &track:clip.morphs) {
    MmdMorphMapping map;std::string name=track.first;
    if(actor.faceProfile) {
      const auto &all=g_mmd.faceSavedMappings;
      if(all.contains(key)&&all[key].contains(name)) {
        const auto &v=all[key][name];name=v.value("morph",name);map.gain=face_geometry::Clamp(v.value("gain",1.f),0,2);
      }
      map.slider=character_face::FindMorph(*actor.faceProfile,name);
    }
    for(const auto &native:SMCManualCatalog())if(mmd::Name(native.name)==track.first)map.nativeSlider=native.channel;
    const auto &fixed=g_mmd.faceSavedNativeMappings;
    if(fixed.contains(track.first)) {
      map.nativeSlider=fixed[track.first].value("slider",map.nativeSlider);
      map.nativeGain=face_geometry::Clamp(fixed[track.first].value("gain",1.f),0,2);
    }
    if(map.nativeSlider<0||map.nativeSlider>=SMCSliderCount())map.nativeSlider=-1;
    actor.morphs[track.first]=map;
  }
}
static void MmdSquadCapture(int slot,const poser_squad::Member &member) {
  auto &s=g_squad;auto &ptr=s.actors[slot];ptr=std::make_unique<MmdSquadActor>();auto &a=*ptr;
  a.member=member;auto &saved=a.saved;saved.animator=member.animator;saved.root=SafeGetComponentTransform(member.animator);
  if(!UnityObjAlive(saved.root))throw std::runtime_error(u8"队员模型尚未就绪");
  saved.references=std::make_shared<GripReferences>();
  MmdSquadRetain(saved,member.entity);MmdSquadRetain(saved,member.animator);MmdSquadRetain(saved,saved.root);
  saved.rootPos=GetBoneLocalPos(saved.root);saved.rootRot=GetBoneLocalRot(saved.root);
  {
    MmdSquadRigScope view(member);RebuildAllBones();
    a.bones=s_allBones;
  }
  for(const auto &bone:a.bones) {
    MmdSquadRetain(saved,bone.transform);
    saved.transforms.push_back({bone.transform,GetBoneLocalPos(bone.transform),GetBoneLocalRot(bone.transform)});
  }
  MmdSquadCollectWriters(a);
}
// Snapshot every participant before changing anything; arm restoration before
// the first disable so even a partial freeze is rolled back by MmdSquadStop.
static void MmdSquadFreezeCaptured() {
  for(auto &ptr:g_squad.actors)if(ptr) {
    auto &a=*ptr;a.saved.active=true;a.saved.bodyOwned=true;
    for(const auto &component:a.saved.components) {
      WriteBehaviourEnabled(component.component,false);bool enabled=true;
      if(!ReadBehaviourEnabled(component.component,enabled)||enabled)throw std::runtime_error(u8"无法冻结队员动画，已取消多人准备");
    }
    MmdHideSessionProps(a.saved,true);
  }
}
static void MmdSquadLoadActorCalibration(int slot) {
  auto &s=g_squad;auto &a=*s.actors[slot];
  // Calibration loading is permitted only after the entire participating group
  // is frozen, never interleaved with snapshotting a later teammate.
  for(const auto &ptr:s.actors)if(ptr) {
    if(!ptr->saved.active)throw std::runtime_error(u8"小队尚未完成冻结");
    for(const auto &component:ptr->saved.components) {
      bool enabled=true;
      if(!ReadBehaviourEnabled(component.component,enabled)||enabled)throw std::runtime_error(u8"队员动画未保持冻结，已取消多人准备");
    }
  }
  {
    MmdSquadRigScope view(a.member);s_allBones=a.bones;RebuildHumanBones();
    a.profile=MmdCurrentProfile();
    if(!MmdLoadCalibration(a.profile)) {
      s.slots[slot].calibrated=false;s.slots[slot].calibration=u8"缺少有效校准，请完成手动 T 姿并确认保存";
      throw std::runtime_error(u8"第 "+std::to_string(slot+1)+u8" 位缺少有效校准；请手动切到该角色，在单人面板完成 T 姿校准并确认保存。");
    }
  }
  a.mapper.bind(s.rig,s.slots[slot].clip,a.profile,mmd::AdaptedRoles(g_mmd.adaptation),g_mmd.adaptation.tracks);
  a.scale=(g_mmd.reference && g_mmd.autoScale?a.mapper.suggestedScale:s.scale)*s.slots[slot].scale;
  a.editorFace=a.member.animator==g_charAnimator;
  if(a.editorFace) a.face={&s_editorSMC,[](SMCActorState*){}};
  else {
    a.face=std::make_shared<SMCActorState>();a.face->actor=a.member.animator;a.face->root=a.saved.root;a.face->bones=a.bones;
    a.face->revision=slot+1;
  }
  MmdSquadFaceMap(a,s.slots[slot].clip);
  {SMCActorScope face(a.face.get());SMCFaceSelectProfile(a.faceProfile,a.profile.model);}
  s.slots[slot].calibrated=true;s.slots[slot].calibration=u8"已读取保存的校准";
  s.slots[slot].status=u8"骨架与校准就绪";
}
static void MmdSquadStop() {
  auto &s=g_squad;s.pending.cancel();s.stopRequested=false;s.timeline.stop();
  const bool occupied=s.active;s.active=false;
  s.cameraOwner=nullptr;s.cameraReferences.reset();
  // Unregister first: no future callback may select a retiring face context.
  s_squadSMC.fill(nullptr);
  s_editorSquadFrozen=false;
  for(auto &ptr:s.actors)if(ptr) {
    auto &a=*ptr;bool alive=!RuntimeClosing()&&UnityObjAlive(a.saved.animator)&&UnityObjAlive(a.saved.root);
    if(a.face) {
      SMCActorScope scope(a.face.get());SMCMotionPublish({});
      if(a.editorFace) {
        if(!RuntimeClosing())SMCMotionConsume();
        if(!g_frozen)SMCAutomation().release(alive);
      } else {
        ResetSMCState(alive);FreeGripHandle(a.face->retainedCore);a.face->retainedCore=0;
      }
    }
    if(alive&&!RuntimeClosing()&&a.saved.active) {
      for(const auto &bone:a.saved.transforms)MmdRawPose(bone.transform,bone.pos,bone.rot);
      MmdRawPose(a.saved.root,a.saved.rootPos,a.saved.rootRot);
      if(a.member.animator==g_charAnimator)CapturePoseSnapshot();
      for(const auto &component:a.saved.components)MmdEnable(component.component,component.enabled);
      for(const auto &prop:a.saved.props)MmdSetActive(prop.object,prop.active);
    }
    if(a.saved.active&&a.member.animator==g_charAnimator) {
      for(int n=0;n<s_humanBoneCount;++n)for(const auto &bone:a.saved.transforms)
        if(s_humanBones[n].transform==bone.transform) {
          s_humanBones[n].localPos=bone.pos;s_humanBones[n].localRot=bone.rot;break;
        }
    }
    ptr.reset();
  }
  if(occupied) {
    g_mmd.audio.close();mmd_camera::Stop();InterlockedExchange(&g_mmdOwnsPose,0);
    s.status=u8"已停止，四名队员分别恢复播放前状态";
  }
}
static void MmdSquadRefresh() {
  auto &s=g_squad;if(s.refresh)for(auto &slot:s.slots)slot.calibrationAnimator=nullptr;
  s.roster=poser_squad::Read();s.refresh=false;s.nextRefresh=MmdNow()+1;
  for(int i=0;i<4;++i) {
    auto &slot=s.slots[i];char name[160]{};
    if(s.roster.members[i].animator) {
      void *root=SafeGetComponentTransform(s.roster.members[i].animator);GetBoneName(root,name,sizeof(name));
    }
    slot.member=name[0]?name:(s.roster.valid&&i>=s.roster.count?u8"空位":u8"模型未就绪");
    if(slot.calibrationAnimator!=s.roster.members[i].animator) {
      slot.calibrated=false;slot.calibration=u8"正在读取此角色的已保存校准";
    }
    if(!s.roster.members[i].animator) {
      slot.calibrationAnimator=nullptr;slot.calibrated=false;slot.calibration=u8"等待角色模型";
    }
  }
}
static void MmdSquadPollCalibrations() {
  auto &s=g_squad;
  if(!s.show||s.active||!s.roster.valid||g_mmd.session.active||g_mmd.preview)return;
  // Read one changed actor per game frame. No preview, pose writes, or automatic
  // calibration: only the saved profile keyed by this exact model/fingerprint.
  for(int i=0;i<4;++i) {
    auto &slot=s.slots[i];auto member=s.roster.members[i];
    if(!member.animator||!UnityObjAlive(member.animator))continue;
    if(slot.calibrationAnimator==member.animator&&slot.calibrationSerial==s_mmdCalibrationSerial)continue;
    slot.calibrationAnimator=member.animator;slot.calibrationSerial=s_mmdCalibrationSerial;
    try {
      MmdSquadRigScope view(member);RebuildAllBones();RebuildHumanBones();auto profile=MmdCurrentProfile();
      slot.calibrated=MmdLoadCalibration(profile);
      slot.calibration=slot.calibrated?u8"已读取保存的校准":u8"未校准：切到此角色，在单人面板完成手动 T 姿并确认保存";
    } catch(...) {slot.calibrated=false;slot.calibration=u8"校准读取失败，请重新读取小队";}
    break;
  }
}
static void MmdSquadDuration() {
  std::array<double,4> seconds{};std::array<bool,4> enabled{};
  for(int i=0;i<4;++i) {
    enabled[i]=g_squad.slots[i].enabled;
    // Camera tracks belong to the shared camera, not an individual dancer.
    for(auto &track:g_squad.slots[i].clip.bones)if(!track.second.empty())seconds[i]=(std::max)(seconds[i],track.second.back().frame/30.);
    for(auto &track:g_squad.slots[i].clip.morphs)if(!track.second.empty())seconds[i]=(std::max)(seconds[i],track.second.back().frame/30.);
  }
  auto &keys=MmdCameraKeys();g_squad.timeline.duration=mmd::SquadDuration(seconds,enabled,
    g_mmd.cameraSettings.enabled?mmd::CameraDuration(keys,g_mmd.cameraSettings):0);
}
static bool MmdSquadStart() {
  auto &s=g_squad;
  if(s.loading||g_mmd.loading||g_mmd.preview||g_mmd.session.active) {s.pending.cancel();s.status=u8"请先完成导入或停止单人播放 / 校准";return false;}
  if(!ClothOnMainThread()) {if(!s.pending.active)s.pending.play();s.status=u8"等待游戏线程开始多人播放";return false;}
  if(s.active) {s.timeline.play(MmdNow());return true;}
  if(s_cloth.active||s_cloth.releasing) {ClothRequestPlayback(false);if(!s.pending.active)s.pending.play();s.status=u8"等待单人衣物增强恢复";return false;}
  MmdSquadRefresh();
  if(!s.roster.valid||!MmdCharacterReady()) {s.pending.cancel();s.status=poser_squad::status;return false;}
  auto identity=MmdSquadIdentity(s.roster);std::set<uintptr_t> unique;int count=0;
  for(int n=0;n<4;++n)if(identity.enabled[n]) {
    if(!identity.entities[n]||!identity.animators[n]||!unique.insert(identity.animators[n]).second) {
      s.pending.cancel();s.status=u8"第 "+std::to_string(n+1)+u8" 位未就绪或角色重复；等待加载，或取消该位置。";return false;
    }
    ++count;
  }
  if(!count) {s.pending.cancel();s.status=u8"请给至少一个小队位置选择动作";return false;}
  try {
    s_mmdStartRequest.cancel();
    s.rig=g_mmd.rig;s.identity=identity;
    for(int n=0;n<4;++n)if(identity.enabled[n])MmdSquadCapture(n,s.roster.members[n]);
    MmdSquadFreezeCaptured();
    for(int n=0;n<4;++n)if(s.actors[n])MmdSquadLoadActorCalibration(n);
    // The controlled character is the origin even when its slot is disabled.
    mmd::RetargetProfile originProfile;bool foundOrigin=false;
    for(auto &actor:s.actors)if(actor&&actor->member.animator==g_charAnimator) {originProfile=actor->profile;foundOrigin=true;break;}
    if(!foundOrigin) {
      originProfile=MmdCurrentProfile();
      if(!MmdLoadCalibration(originProfile))throw std::runtime_error(u8"作为共同原点的当前角色尚未校准，请先手动 T 姿校准并确认保存");
    }
    mmd::Retargeter originMapper;originMapper.bind(s.rig,g_mmd.clip,originProfile,mmd::AdaptedRoles(g_mmd.adaptation),g_mmd.adaptation.tracks);
    void *origin=GetCharRootTransform();s.anchor={GetBoneWorldPos(origin),NormQ(GetBoneWorldRot(origin)*originMapper.sourceBasis())};
    s.cameraOwner=g_charAnimator;s.cameraHeight=mmd::CameraTargetHeight(originProfile);
    s.cameraReferences=std::make_shared<GripReferences>();
    uint32_t cameraOwnerRef=il2cpp_gchandle_new?il2cpp_gchandle_new(s.cameraOwner,false):0;
    if(!cameraOwnerRef)throw std::runtime_error(u8"无法保留多人镜头的起点角色");
    s.cameraReferences->handles.push_back(cameraOwnerRef);
    for(int n=0;n<4;++n)if(s.actors[n]) {
      auto &a=*s.actors[n];
      if(a.editorFace)s_editorSquadFrozen=true;
      s_squadSMC[n]=a.face.get();
    }
    s.active=true;s.cameraSession=++mmd_camera::nextSession;InterlockedExchange(&g_mmdOwnsPose,1);
    MmdSquadDuration();s.timeline.play(MmdNow());s.status=u8"多人播放中，共用操控角色的起始原点";
    Log("[MMD-SQUAD] started members=%d squad=%p origin=(%.3f %.3f %.3f)",count,s.roster.squad,s.anchor.origin.x,s.anchor.origin.y,s.anchor.origin.z);
    return true;
  } catch(const std::exception &e) {MmdSquadStop();s.status=e.what();return false;}
}
static void MmdSquadApply() {
  auto &s=g_squad;if(!s.active)return;
  const double frame=s.timeline.seconds*30.;
  for(int n=0;n<4;++n)if(s.actors[n]) {
    auto &a=*s.actors[n];auto &slot=s.slots[n];
    if(!UnityObjAlive(a.saved.animator)||!UnityObjAlive(a.saved.root)) {MmdSquadStop();s.status=u8"队员实例已失效，已停止全部动作";return;}
    for(const auto &bone:a.bones)if(!UnityObjAlive(bone.transform)) {MmdSquadStop();s.status=u8"队员骨架已变化，已停止全部动作";return;}
    for(const auto &component:a.saved.components)MmdEnable(component.component,false);
    a.mapper.sample(frame,a.scale,false,0,s.ikMode,g_mmd.amplitude);
    const auto &pose=a.mapper.output;
    const auto placement=s.anchor.place(a.mapper.sourceBasis(),pose.rootOffset,slot.offset,slot.yaw,s.inPlace,s.height+slot.height);
    if(!MmdSquadWorldPose(a.saved.root,placement.position,placement.rotation)) {MmdSquadStop();s.status=u8"无法设置队员位置，已停止";return;}
    for(size_t j=1;j<pose.write.size()&&j<a.bones.size();++j)if(pose.write[j])
      MmdRawPose(a.bones[j].transform,a.profile.bones[j].localPos,pose.localRot[j]);
    // Preserve the editor's snapshot for saving a paused squad pose.
    if(a.member.animator==g_charAnimator)CapturePoseSnapshot();
    {SMCActorScope scope(a.face.get());
      if(a.faceGeneration!=s_faceGeneration||a.faceHierarchyReady!=s_faceHierarchy.ready||a.libraryCount!=g_mmd.faceLibrary.size()) {
        auto previous=a.faceProfile;const auto key=character_face::ModelKey(a.profile.model);
        float best=1e30f;a.faceProfile.reset();
        for(auto &profile:g_mmd.faceLibrary)if(profile->key==key) {
          if(!a.faceProfile)a.faceProfile=profile;
          if(s_faceHierarchy.ready) {
            auto binding=character_face::Bind(*profile,key,s_faceNodes,s_faceHierarchy);
            if(binding.ready&&binding.error<best) {best=binding.error;a.faceProfile=profile;}
          }
        }
        if(previous!=a.faceProfile){a.morphs.clear();MmdSquadFaceMap(a,slot.clip);}
        a.faceGeneration=s_faceGeneration;a.faceHierarchyReady=s_faceHierarchy.ready;a.libraryCount=g_mmd.faceLibrary.size();
      }
      SMCFaceSelectProfile(a.faceProfile,a.profile.model);
      SMCMotionFrame face;face.active=true;face.animator=a.member.animator;face.generation=s_faceGeneration;
      face.profile=a.faceProfile;face.settings=g_mmd.faceSettings;
      for(const auto &track:a.morphs) {
        const auto &map=track.second;float value=mmd::SampleMorph(slot.clip.morphs.at(track.first),frame);
        bool calibrated=map.slider>=0&&s_characterBinding.ready&&map.slider<int(s_characterBinding.usable.size())&&s_characterBinding.usable[map.slider];
        if(calibrated)face.expressions[map.slider]=(std::max)(face.expressions[map.slider],face_geometry::Clamp(value*map.gain,0,1));
        if(map.nativeSlider>=0) {
          int id=map.nativeSlider;face.weights[id]=face_geometry::Clamp(face.weights[id]+value*map.nativeGain,0,1);
          if(!calibrated)face.fallbackWeights[id]=face_geometry::Clamp(face.fallbackWeights[id]+value*map.nativeGain,0,1);
        }
      }
      for(int e=0;e<2;++e) {int j=a.profile.roles[21+e];if(j>=0&&j<int(pose.write.size())&&pose.write[j]) {
        face.eyeDriven[e]=true;face.eyes[e]=a.bones[j].transform;face.eyeRotation[e]=pose.localRot[j];
      }}
      SMCMotionPublish(face);slot.status=SMCSectionReady()?u8"身体 / 表情已就绪":u8"身体已就绪，等待表情系统";
    }
    MmdHideSessionProps(a.saved);
  }
  const auto &keys=MmdCameraKeys();
  if(g_mmd.cameraSettings.enabled&&!keys.empty()&&mmd_camera::ready) {
    const auto &settings=g_mmd.cameraSettings;
    Vec3 delta{},correction{0,s.height,0};void *follow=nullptr;float height=s.cameraHeight;
    const auto *target=s.cameraFollow>=0&&s.cameraFollow<4?s.actors[s.cameraFollow].get():nullptr;
    if(settings.origin==mmd::CameraOrigin::Follow && !target) {
      mmd_camera::Stop();s.status=u8"所选镜头跟随队员未参与，镜头已恢复；可切回固定起点";
    } else {
      if(settings.origin==mmd::CameraOrigin::Follow) {
        follow=target->member.animator;delta=GetBoneWorldPos(target->saved.root)-s.anchor.origin;
        height=mmd::CameraTargetHeight(target->profile);correction.y+=s.slots[s.cameraFollow].height;
      }
      mmd_camera::Publish({true,s.cameraSession,s.cameraOwner,
        mmd::PlaceCamera(mmd::SampleCamera(keys,mmd::CameraFrame(s.timeline.seconds,settings),settings),
          settings,s.anchor.origin,s.anchor.basis,delta,s.scale,height,mmd::CameraSourceHeight(s.rig),correction),follow,0,frame});
    }
  } else mmd_camera::Stop();
  try {g_mmd.audio.sync(s.timeline,true,g_mmd.musicEnabled,g_mmd.musicOffset,g_mmd.musicVolume);}
  catch(const std::exception &e) {g_mmd.musicError=e.what();g_mmd.musicEnabled=false;}
}
static void MmdSquadLoad(int slot,bool append=false,std::filesystem::path path={}) {
  auto &s=g_squad;if(s.active||s.pending.active||s.loading||g_mmd.session.active||g_mmd.preview||g_mmd.loading||slot<0||slot>=4)return;
  s.loading=true;s.hotkeys=true;s_mmdClosing.store(false);HWND owner=g_gameHwnd;
  try {s.loader=std::async(std::launch::async,[slot,append,path,owner] {
    MmdLoadResult result;result.kind=slot+(append?4:0);
    try {
      auto chosen=path;if(chosen.empty()) {
        wchar_t name[32768]{};OPENFILENAMEW file{};file.lStructSize=sizeof(file);file.hwndOwner=owner;
        file.lpstrFilter=L"VMD motion\0*.vmd\0\0";file.lpstrFile=name;file.nMaxFile=32768;
        file.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER|OFN_ENABLEHOOK;file.lpfnHook=MmdDialogHook;
        if(!GetOpenFileNameW(&file)) {s_mmdDialog.store(nullptr);if(CommDlgExtendedError())throw std::runtime_error("File dialog failed");result.cancelled=true;return result;}
        s_mmdDialog.store(nullptr);chosen=name;
      }
      result.file=mmd::Utf8(chosen.wstring());result.clip=mmd::ReadVmdFile(chosen);
      if(result.clip.bones.empty()&&result.clip.morphs.empty())throw std::runtime_error(u8"此文件没有身体 / 表情轨道，镜头请单独选择");
      if(append) {bool eyes=false;for(const auto &track:result.clip.bones)eyes|=mmd::EyeBone(track.first);
        if(result.clip.morphs.empty()&&!eyes)throw std::runtime_error(u8"没有可追加的表情或眼神轨道");}
      result.clip.cameras.clear();mmd::Recount(result.clip);
    }catch(const std::exception &e){result.error=e.what();}
    return result;
  });} catch(const std::exception &e) {s.loading=false;s.status=e.what();}
}
static bool MmdSquadTick() {
  auto &s=g_squad;
  try {
    if(s.stopRequested&&ClothOnMainThread())MmdSquadStop();
    if(s.loading&&s.loader.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      auto r=s.loader.get();s.loading=false;
      if(!r.cancelled) {
        if(!r.error.empty()) {s.status=r.error;Log("[MMD-SQUAD] load error slot=%d file=%s: %s",r.kind%4+1,r.file.c_str(),r.error.c_str());}
        else {auto &slot=s.slots[r.kind%4];if(r.kind>=4)mmd::AppendFace(slot.clip,r.clip);else {slot.clip=std::move(r.clip);slot.file=r.file;}
          slot.status=u8"动作已导入";s.status=u8"按小队位置分配完成，可播放或应用到四人";MmdSquadDuration();}
      }
    }
    if(ClothOnMainThread()) {
      if(!s.active&&(s.refresh||(s.show&&MmdNow()>=s.nextRefresh)))MmdSquadRefresh();
      if(!s.pending.active)MmdSquadPollCalibrations();
      if(s.pending.active) {auto request=s.pending;if(MmdSquadStart()) {request.apply(s.timeline,MmdNow());s.pending.cancel();}}
      if(s.active) {
        auto current=poser_squad::Read();
        if(!current.valid||!s.identity.matches(MmdSquadIdentity(current))) {MmdSquadStop();s.status=u8"小队顺序或队员实例发生变化，已停止并恢复";return false;}
      }
    }
    if(!s.active)return false;
    if(!ClothOnMainThread())return true;
    s.timeline.tick(MmdNow());MmdSquadApply();return s.active;
  }catch(const std::exception &e){MmdSquadStop();s.status=e.what();return false;}
}
static void MmdSquadSeek(double seconds) {
  auto &s=g_squad;
  if(!s.active) {s.pending.seek(seconds);s.hotkeys=true;return;}
  s.pending.seek(seconds);
}
static bool MmdSquadCommand(int command) {
  auto &s=g_squad;
  if(g_mmd.session.active||g_mmd.preview||s_mmdStartRequest.active)return false;
  if(!s.hotkeys&&!s.active&&!s.pending.active)return false;
  bool content=false;for(const auto &slot:s.slots)content|=slot.enabled&&!slot.clip.empty();
  if(!s.active&&!s.pending.active&&!content) {s.hotkeys=false;return false;}
  if(command==2) {s.pending.cancel();s.stopRequested=true;}
  else if(command==0) {s.hotkeys=true;s.pending.play();}
  else if(command==1) {if(s.pending.active)s.pending.pause();if(s.active)s.timeline.pause(MmdNow());}
  else if(command==3)MmdSquadSeek(0);
  return true;
}
static void MmdSquadCopyToAll(int source) {
  if(g_squad.active||g_squad.pending.active||g_squad.loading||source<0||source>=4)return;
  const auto clip=g_squad.slots[source].clip;const auto file=g_squad.slots[source].file;
  for(int n=0;n<4;++n) {g_squad.slots[n].clip=clip;g_squad.slots[n].file=file;}
  MmdSquadDuration();g_squad.status=u8"同一动作已应用到小队第 1–4 位";
}
static void MmdSquadInstall() {
  g_mmdSquadBridge={[](){return g_squad.active;},MmdSquadStop,MmdSquadTick,MmdSquadCommand,
    [](){return g_squad.active||g_squad.pending.active||g_squad.loading;},
    [](){if(!g_squad.active&&!g_squad.pending.active)g_squad.hotkeys=false;},
    [](){return !g_mmd.session.active&&!g_mmd.preview&&!s_mmdStartRequest.active &&
      (g_squad.hotkeys||g_squad.active||g_squad.pending.active);}};
}
