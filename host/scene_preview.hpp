#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor_camera.hpp"

namespace host {

// Embedding surface for the unified native viewport. Paths are resolved and constrained
// by the viewport process before they reach this runtime. The implementation is the same
// Game/Renderer/effect/audio path used by host/main.cpp's external runner.
struct ScenePreviewOptions {
    std::string mode = "scene";
    std::filesystem::path projectRoot;
    std::filesystem::path scene;
    std::filesystem::path textures = "assets/textures";
    std::filesystem::path character = "build/animation/mannequin/character.json";
    std::filesystem::path effectLibrary = "build/effects/library.json";
    std::string asset;
    std::string material;
    std::string clip;
    std::string effect;
    // Scene preview starts in the editor state: physics and gameplay do not advance.
    std::string play = "edit";
    bool audio = false;
};

struct ScenePreviewStats {
    std::uint64_t frames = 0;
    std::uint64_t worldInstances = 0;
    std::uint64_t residentInstances = 0;
    std::uint64_t overlayRevision = 0;
    std::uint32_t vegetationLayers = 0;
    std::string digest;
    std::string play = "edit";
    bool paused = false;
    bool cameraOrthographic = false;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.0f;
    float cameraDistance = 0.0f;
};

struct ScenePreviewCameraState {
    EditorCamera camera;
    bool valid = false;
};

bool scenePreviewOpen(HWND window, unsigned width, unsigned height,
                      const ScenePreviewOptions& options, std::string& error);
bool scenePreviewResize(unsigned width, unsigned height, std::string& error);
bool scenePreviewFrame(float deltaSeconds, std::string& error);
bool scenePreviewCapture(const std::string& path, std::string& error);
void scenePreviewClose();
ScenePreviewStats scenePreviewStats();

// Play mode: "edit" freezes gameplay/physics, "simulate" advances physics without
// player input, "run" uses the game camera and advances the whole game. Switching
// back to edit restores the authored scene state, so simulation results are never
// written back implicitly.
bool scenePreviewSetPlay(const std::string& play, bool paused, std::string& error);
bool scenePreviewStep(float deltaSeconds, std::string& error);

// Named projection: perspective, top, front or side.
bool scenePreviewSetView(const std::string& view, std::string& error);
// Preview-only layered debug view: "off", "mask[:0-3]", "height", "albedo[:0-3]".
bool scenePreviewSetLayerDebug(const std::string& spec, std::string& error);

// Camera navigation happens in the native viewport (it owns the mouse). Each call
// is relative to the previous state; the page only needs focus and projection.
void scenePreviewCameraOrbit(float dxPixels, float dyPixels);
void scenePreviewCameraPan(float dxPixels, float dyPixels);
void scenePreviewCameraDolly(float wheelSteps);
void scenePreviewCameraMove(float forward, float right, float up, float deltaSeconds,
                            float metresPerSecond);
bool scenePreviewFocus(const std::string& entityId, std::string& error);
ScenePreviewCameraState scenePreviewCamera();

// Draft overlay: a transform that is visible in the native viewport but not yet
// written to the scene file. `active=false` hides the entity without deleting it.
// The override is expressed as absolute position/scale; compound entities keep their
// internal part offsets. It never touches physics or the scene document.
bool scenePreviewSetEntityOverride(const std::string& entityId, const float position[3],
                                   const float scale[3], bool active,
                                   std::string& error);
void scenePreviewClearOverrides();

} // namespace host
