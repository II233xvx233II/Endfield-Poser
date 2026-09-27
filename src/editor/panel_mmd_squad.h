#pragma once
#include "game/mmd_squad.h"
#include "editor/panel_mmd.h"

static void DrawMmdSquadPanel() {
  auto &s=g_squad;if(!s.show)return;
  ImGui::SetNextWindowSize(ImVec2(560,650),ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(540,80),PanelPositionCondition());
  if(!ImGui::Begin(u8"MMD 多人播放器",&s.show,g_pinPanels?ImGuiWindowFlags_NoMove:0)) {ImGui::End();return;}
  ImGui::TextWrapped(u8"按小队第 1–4 位分配动作。以点击播放时当前操控角色的位置和朝向为固定原点；隐藏面板后继续播放。");
  ImGui::TextWrapped(u8"首次使用请逐个手动切换队员，在单人面板完成手动 T 姿并确认保存；这里会自动读取校准。合跳前统一冻结参与成员，停止后恢复。");
  ImGui::Checkbox(u8"快捷键控制多人播放器",&s.hotkeys);
  if(s.hotkeys||s.active)DrawMmdHotkeyHints();
  ImGui::BeginDisabled(s.active||s.pending.active||s.loading||g_mmd.session.active||g_mmd.preview||g_mmd.loading);
  if(ImGui::Button(u8"重新读取小队"))s.refresh=true;
  ImGui::SameLine();
  if(ImGui::Button(u8"单人面板当前动作 → 四人")&&!g_mmd.clip.empty()) {
    for(auto &slot:s.slots) {slot.clip=g_mmd.clip;slot.clip.cameras.clear();mmd::Recount(slot.clip);slot.file=g_mmd.file;}
    s.hotkeys=true;MmdSquadDuration();
  }
  ImGui::TextWrapped("%s",poser_squad::status);
  for(int i=0;i<4;++i) {
    auto &slot=s.slots[i];ImGui::PushID(i);ImGui::Separator();
    if(ImGui::Checkbox(u8"参与",&slot.enabled))MmdSquadDuration();ImGui::SameLine();
    ImGui::Text(u8"第 %d 位：%s",i+1,slot.member.empty()?u8"待读取":slot.member.c_str());
    ImGui::TextWrapped("%s",slot.calibration.c_str());
    if(ImGui::Button(u8"选择动作"))MmdSquadLoad(i);
    ImGui::SameLine();if(ImGui::Button(u8"追加表情 / 眼神"))MmdSquadLoad(i,true);
    ImGui::SameLine();ImGui::BeginDisabled(slot.clip.empty());
    if(ImGui::Button(u8"此动作应用到四人"))MmdSquadCopyToAll(i);
    ImGui::EndDisabled();ImGui::SameLine();
    if(ImGui::SmallButton(u8"清除")) {slot.clip={};slot.file.clear();slot.status.clear();MmdSquadDuration();}
    if(!slot.file.empty())ImGui::TextWrapped("%s",slot.file.c_str());
    if(!slot.clip.empty())ImGui::Text(u8"身体 %zu / 表情 %zu",slot.clip.bones.size(),slot.clip.morphs.size());
    ImGui::PopID();
  }
  ImGui::EndDisabled();
  if(s.loading)ImGui::TextDisabled(u8"正在读取动作，失败时保留此前动作");
  ImGui::Separator();
  ImGui::BeginDisabled(s.loading||g_mmd.loading||g_mmd.session.active||g_mmd.preview);
  if(ImGui::Button(s.timeline.state==mmd::PlayState::Playing?u8"暂停全队":u8"播放全队")) {
    s.hotkeys=true;MmdSquadCommand(s.timeline.state==mmd::PlayState::Playing?1:0);
  }
  ImGui::SameLine();if(ImGui::Button(u8"停止并恢复全队")){s.hotkeys=true;MmdSquadCommand(2);}
  ImGui::SameLine();if(ImGui::Button(u8"回到首帧"))MmdSquadSeek(0);
  ImGui::SameLine();if(ImGui::SmallButton("<"))MmdSquadSeek(s.timeline.seconds-1./30);
  ImGui::SameLine();if(ImGui::SmallButton(">"))MmdSquadSeek(s.timeline.seconds+1./30);
  float seconds=float(s.timeline.seconds);ImGui::SetNextItemWidth(-1);
  if(ImGui::SliderFloat("##squad-time",&seconds,0,float(s.timeline.duration),"%.2f s"))MmdSquadSeek(seconds);
  ImGui::Text(u8"全队共用时间轴：帧 %.1f / %.0f",s.timeline.seconds*30,s.timeline.duration*30);
  float speed=float(s.timeline.speed);
  if(ImGui::SliderFloat(u8"播放速度",&speed,.25f,2,"%.2fx")) {s.timeline.tick(MmdNow());s.timeline.speed=speed;}
  ImGui::Checkbox(u8"全队循环",&s.timeline.loop);ImGui::SameLine();ImGui::Checkbox(u8"原地播放",&s.inPlace);
  int ik=int(s.ikMode);if(ImGui::Combo(u8"动作 IK",&ik,u8"跟随各自动作\0强制开启\0强制关闭\0"))s.ikMode=mmd::IkMode(ik);
  ImGui::SliderFloat(u8"全队高度修正",&s.height,-1,1,"%.3f");
  ImGui::BeginDisabled(s.active);
  ImGui::SliderFloat(u8"基础位移比例",&s.scale,.001f,.3f,"%.4f");
  ImGui::EndDisabled();
  if(ImGui::CollapsingHeader(u8"队员站位与动作比例")) {
    ImGui::TextWrapped(u8"偏移单位为米，方向相对共同原点。全部偏移为 0 时重合在原点，适合动作自带站位的编舞。");
    if(ImGui::Button(u8"四人横排"))for(int i=0;i<4;++i)s.slots[i].offset={float(i)-1.5f,0,0};
    ImGui::SameLine();if(ImGui::Button(u8"全部站位归零"))for(auto &slot:s.slots){slot.offset={};slot.yaw=0;}
    for(int i=0;i<4;++i) {auto &slot=s.slots[i];ImGui::PushID(i);ImGui::Text(u8"第 %d 位",i+1);
      ImGui::SliderFloat3(u8"左右 / 上下 / 前后",&slot.offset.x,-10,10,"%.2f m");
      ImGui::SliderFloat(u8"朝向偏移",&slot.yaw,-180,180,"%.1f");
      ImGui::SliderFloat(u8"高度修正",&slot.height,-1,1,"%.3f");
      ImGui::BeginDisabled(s.active);ImGui::SliderFloat(u8"动作位移倍率",&slot.scale,.1f,3,"%.2fx");ImGui::EndDisabled();
      ImGui::PopID();}
  }
  if(ImGui::CollapsingHeader(u8"音乐与镜头")) {
    ImGui::TextWrapped(u8"与单人面板共用音乐和镜头文件；由全队时间轴同步播放。" );
    ImGui::BeginDisabled(s.active||s.loading);
    if(ImGui::Button(u8"选择音乐"))MmdBeginLoad(5);
    ImGui::SameLine();if(ImGui::Button(u8"选择镜头"))MmdBeginLoad(6);
    ImGui::EndDisabled();
    ImGui::Checkbox(u8"播放音乐",&g_mmd.musicEnabled);
    ImGui::SliderFloat(u8"音乐音量",&g_mmd.musicVolume,0,1,"%.2f");
    ImGui::SliderFloat(u8"音乐偏移（秒）",&g_mmd.musicOffset,-30,30,"%.2f");
    if(ImGui::Checkbox(u8"播放 MMD 镜头",&g_mmd.cameraSettings.enabled))MmdSquadDuration();
    ImGui::Combo(u8"镜头跟随",&s.cameraFollow,u8"第 1 位\0第 2 位\0第 3 位\0第 4 位\0");
    float cameraHeight=s.cameraHeight;
    if(g_mmd.cameraSettings.origin==mmd::CameraOrigin::Follow&&s.cameraFollow>=0&&s.cameraFollow<4&&s.actors[s.cameraFollow])
      cameraHeight=mmd::CameraTargetHeight(s.actors[s.cameraFollow]->profile);
    if(DrawMmdCameraSettings(s.timeline.seconds,cameraHeight))MmdSquadDuration();
  }
  ImGui::EndDisabled();
  ImGui::TextWrapped("%s",s.status.c_str());
  if(s.active)for(int i=0;i<4;++i)if(s.actors[i])ImGui::Text(u8"第 %d 位：%s",i+1,s.slots[i].status.c_str());
  ImGui::TextWrapped(u8"短动作到末帧后保持姿态，循环按最长动作统一重开。换人、换队或实例失效时全队停止并恢复。多人使用角色原有衣物物理，暂不叠加单人服装增强。");
  ImGui::End();
}
