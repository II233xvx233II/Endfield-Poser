#pragma once
// Copyright (C) 2026 II233xvx233II. Part of Endfield Poser (mirror-extension branch), AGPL-3.0.
// Endfield Poser extension ABI (C, POD only).
//
// An optional DLL in plugin\ finds the host with
//   GetProcAddress(GetModuleHandleW(L"poser.dll"), POSER_EXTENSION_ENTRY)
// and registers callbacks. Nothing here depends on the host's C++ runtime,
// ImGui build or IL2CPP headers, so extensions build independently.
//
// Threading: onFrame runs on the game's main thread, onGui inside the host's
// ImGui frame. The host never runs two callbacks at the same time, so an
// extension may share state between its own callbacks without locking.
// Pose and puppet functions are valid only inside onFrame; ui* functions only
// inside onGui. Calls outside those contexts fail and return 0.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSER_EXTENSION_API_VERSION 1
#define POSER_EXTENSION_ENTRY "PoserGetExtensionApi"
#define POSER_HUMAN_BONES 55
#define POSER_SQUAD_SLOTS 4

typedef struct PoserVec3 { float x, y, z; } PoserVec3;
typedef struct PoserQuat { float x, y, z, w; } PoserQuat;

// Character-independent humanoid pose. The body frame is taken from the
// calibrated T pose: x = character right, y = up, z = forward.
// Bone indices follow Unity HumanBodyBones (0 = Hips ... 54 = UpperChest).
// Suitable as a network payload: it holds no pointers.
typedef struct PoserHumanPose {
  uint32_t size;            // sizeof(PoserHumanPose)
  uint32_t reserved;
  uint64_t boneMask;        // bit n set: rotations[n] is valid
  PoserVec3 rootPosition;   // world position of the character root (feet)
  PoserQuat bodyRotation;   // world rotation of the body frame
  PoserVec3 hipsPosition;   // hips in the body frame, divided by T-pose hips height
  PoserQuat rotations[POSER_HUMAN_BONES]; // body-frame rotation relative to T pose
} PoserHumanPose;

typedef struct PoserSquadInfo {
  uint32_t size;            // sizeof(PoserSquadInfo)
  int32_t valid;            // 0 while loading or outside a playable scene
  int32_t count;            // occupied slots, 1..4
  int32_t controlledSlot;   // slot of the controlled character, -1 if unknown
  int32_t ready[POSER_SQUAD_SLOTS];  // model loaded
  int32_t puppet[POSER_SQUAD_SLOTS]; // currently driven by an extension
  char names[POSER_SQUAD_SLOTS][64]; // UTF-8 model names
} PoserSquadInfo;

typedef void (*PoserCallback)(void *user);

typedef struct PoserExtensionDesc {
  uint32_t size;            // sizeof(PoserExtensionDesc)
  const char *name;         // UTF-8, shown as the panel title; copied by the host
  void *user;
  PoserCallback onFrame;    // optional
  PoserCallback onGui;      // optional
} PoserExtensionDesc;

typedef struct PoserExtensionApi {
  uint32_t version;         // POSER_EXTENSION_API_VERSION of the host
  uint32_t size;            // sizeof(PoserExtensionApi) of the host
  const char *hostVersion;  // e.g. "0.4.73"

  // Any thread. Returns an id > 0, or 0 on failure. Unregistering releases
  // every puppet the extension still owns.
  int32_t (*registerExtension)(const PoserExtensionDesc *desc);
  void (*unregisterExtension)(int32_t id);
  void (*log)(const char *utf8);   // appends to plugin\poser_log.txt
  double (*now)(void);             // seconds, monotonic

  // Squad snapshot, refreshed once per game frame. onFrame or onGui.
  int32_t (*getSquad)(PoserSquadInfo *out);

  // onFrame only. Encodes the controlled character's current pose.
  int32_t (*captureControlled)(PoserHumanPose *out);
  // onFrame only. Takes over a squad member (not the controlled character):
  // saves its state and disables its animation writers.
  int32_t (*acquirePuppet)(int32_t slot);
  // onFrame only. offset is in the pose's body frame (right, up, forward),
  // in meters; yawDegrees turns the puppet around world up.
  // Returns 0 if the puppet was released by the host (see status()).
  int32_t (*applyPuppet)(int32_t slot, const PoserHumanPose *pose,
                         PoserVec3 offset, float yawDegrees);
  // onFrame only. Restores the member to its state before acquirePuppet.
  void (*releasePuppet)(int32_t slot);
  // Reason for the last failure, or the last host-initiated release. UTF-8,
  // valid until the next API call.
  const char *(*status)(void);

  // onGui only. Labels are UTF-8 ImGui labels ("##id" suffixes work).
  void (*uiText)(const char *text);
  void (*uiTextDisabled)(const char *text);
  int32_t (*uiCheckbox)(const char *label, int32_t *value);
  int32_t (*uiButton)(const char *label);
  int32_t (*uiSliderFloat)(const char *label, float *value, float min,
                           float max, const char *format);
  int32_t (*uiCombo)(const char *label, int32_t *current,
                     const char *const *items, int32_t count);
  void (*uiSameLine)(void);
  void (*uiSeparator)(void);
} PoserExtensionApi;

// Returns nullptr when the host cannot serve the requested major version.
typedef const PoserExtensionApi *(*PoserGetExtensionApiFn)(uint32_t version);

#ifdef __cplusplus
}
#endif
