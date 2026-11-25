#pragma once

// Independent editor camera. It must never read or write the player camera, so the
// viewport can orbit a scene while gameplay physics is frozen. The math mirrors the
// engine's left-handed, column-major, 0..1-depth conventions (renderer/matrix_math.h)
// without pulling the whole engine into the viewport host.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace host {

struct EditorCamera {
    float yaw = 0.62f;
    float pitch = -0.18f;
    float distance = 12.0f;
    float target[3] = {0.0f, 1.0f, 0.0f};
    bool orthographic = false;
    float fovY = 1.0f;
};

struct EditorCameraPose {
    float eye[3]{};
    float target[3]{};
    // Camera basis in world space; `forward` points from the eye to the target.
    float forward[3]{};
    float right[3]{};
    float up[3]{};
};

inline void editorCameraClamp(EditorCamera& camera) {
    camera.distance = std::clamp(camera.distance, 0.25f, 100000.0f);
    // Stop just short of the poles: looking exactly along +/-Y makes `right`
    // degenerate and the view flips unpredictably.
    camera.pitch = std::clamp(camera.pitch, -1.5533f, 1.5533f);
    camera.fovY = std::clamp(camera.fovY, 0.1f, 2.8f);
}

inline EditorCameraPose editorCameraPose(const EditorCamera& camera) {
    EditorCameraPose pose;
    pose.target[0] = camera.target[0];
    pose.target[1] = camera.target[1];
    pose.target[2] = camera.target[2];
    const float cp = std::cos(camera.pitch);
    const float sp = std::sin(camera.pitch);
    const float cy = std::cos(camera.yaw);
    const float sy = std::sin(camera.yaw);
    const float offset[3] = {sy * cp * camera.distance, -sp * camera.distance,
                             -cy * cp * camera.distance};
    for (int axis = 0; axis < 3; ++axis) {
        pose.eye[axis] = pose.target[axis] + offset[axis];
        pose.forward[axis] = -offset[axis] / camera.distance;
    }
    // `right = normalize(cross(up, forward))`, `up = cross(forward, right)`.
    const float worldUp[3] = {0.0f, 1.0f, 0.0f};
    pose.right[0] = worldUp[1] * pose.forward[2] - worldUp[2] * pose.forward[1];
    pose.right[1] = worldUp[2] * pose.forward[0] - worldUp[0] * pose.forward[2];
    pose.right[2] = worldUp[0] * pose.forward[1] - worldUp[1] * pose.forward[0];
    const float rightLength = std::sqrt(pose.right[0] * pose.right[0] +
                                        pose.right[1] * pose.right[1] +
                                        pose.right[2] * pose.right[2]);
    if (rightLength > 1.0e-5f) {
        for (float& value : pose.right) value /= rightLength;
    } else {
        pose.right[0] = 1.0f;
        pose.right[1] = 0.0f;
        pose.right[2] = 0.0f;
    }
    pose.up[0] = pose.forward[1] * pose.right[2] - pose.forward[2] * pose.right[1];
    pose.up[1] = pose.forward[2] * pose.right[0] - pose.forward[0] * pose.right[2];
    pose.up[2] = pose.forward[0] * pose.right[1] - pose.forward[1] * pose.right[0];
    return pose;
}

// `dx`/`dy` are pointer pixels; increasing `dy` means the pointer moved down.
inline void editorCameraOrbit(EditorCamera& camera, float dx, float dy,
                              float radiansPerPixel = 0.006f) {
    camera.yaw -= dx * radiansPerPixel;
    camera.pitch = std::clamp(camera.pitch - dy * radiansPerPixel, -1.5533f, 1.5533f);
    editorCameraClamp(camera);
}

inline void editorCameraLook(EditorCamera& camera, float dx, float dy) {
    editorCameraOrbit(camera, dx, dy);
}

inline void editorCameraDolly(EditorCamera& camera, float wheelSteps) {
    if (!std::isfinite(wheelSteps)) return;
    camera.distance *= std::pow(1.14f, -wheelSteps);
    editorCameraClamp(camera);
}

// Screen-space pan: one pixel maps to the world size of one pixel at the target
// distance, so the point under the cursor keeps up with the pointer in both
// projections.
inline void editorCameraPan(EditorCamera& camera, float dx, float dy,
                            float viewportHeight) {
    if (!(viewportHeight > 1.0f)) return;
    const EditorCameraPose pose = editorCameraPose(camera);
    // Both projections show `2 * distance * tan(fov/2)` metres vertically at the
    // target plane, so the same scale keeps the point under the cursor pinned.
    const float visibleHeight = camera.distance * std::tan(camera.fovY * 0.5f) * 2.0f;
    const float perPixel = visibleHeight / viewportHeight;
    for (int axis = 0; axis < 3; ++axis) {
        camera.target[axis] += -pose.right[axis] * dx * perPixel
                             + pose.up[axis] * dy * perPixel;
    }
}

inline void editorCameraMove(EditorCamera& camera, float forward, float right, float up,
                             float deltaSeconds, float metresPerSecond) {
    if (!(deltaSeconds > 0.0f) || !std::isfinite(deltaSeconds)) return;
    const EditorCameraPose pose = editorCameraPose(camera);
    const float step = metresPerSecond * deltaSeconds;
    for (int axis = 0; axis < 3; ++axis) {
        camera.target[axis] += pose.forward[axis] * forward * step
                             + pose.right[axis] * right * step
                             + pose.up[axis] * up * step;
    }
}

inline bool editorCameraFocus(EditorCamera& camera, const float minimum[3],
                              const float maximum[3]) {
    if (!minimum || !maximum) return false;
    float center[3]{};
    for (int axis = 0; axis < 3; ++axis) {
        center[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
        if (!std::isfinite(center[axis])) return false;
    }
    float radius = 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        radius = std::max(radius, (maximum[axis] - minimum[axis]) * 0.5f);
    }
    for (float& value : camera.target) value = 0.0f;
    for (int axis = 0; axis < 3; ++axis) camera.target[axis] = center[axis];
    camera.distance = std::max(1.5f, radius * 3.2f);
    editorCameraClamp(camera);
    return true;
}

// Named views. Perspective keeps the current orientation so switching back from an
// orthographic view does not snap the user somewhere else.
inline void editorCameraSetView(EditorCamera& camera, const std::string& view) {
    if (view == "top") {
        camera.orthographic = true;
        camera.yaw = 0.0f;
        camera.pitch = -1.5533f;
    } else if (view == "front") {
        camera.orthographic = true;
        camera.yaw = 0.0f;
        camera.pitch = 0.0f;
    } else if (view == "side") {
        camera.orthographic = true;
        camera.yaw = 1.5707963f;
        camera.pitch = 0.0f;
    } else {
        camera.orthographic = false;
    }
    editorCameraClamp(camera);
}

inline void editorCameraProjection(const EditorCamera& camera, float aspect,
                                   float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    aspect = std::max(aspect, 0.001f);
    const float nearPlane = std::max(0.02f, std::min(camera.distance * 0.01f, 10.0f));
    const float farPlane = std::max(1000.0f, camera.distance * 200.0f + 4000.0f);
    if (camera.orthographic) {
        const float halfHeight =
            std::max(0.05f, camera.distance * std::tan(camera.fovY * 0.5f));
        const float halfWidth = halfHeight * aspect;
        out[0] = 1.0f / halfWidth;
        out[5] = 1.0f / halfHeight;
        out[10] = 1.0f / (farPlane - nearPlane);
        out[14] = -nearPlane / (farPlane - nearPlane);
        out[15] = 1.0f;
    } else {
        out[0] = (1.0f / std::tan(camera.fovY * 0.5f)) / aspect;
        out[5] = 1.0f / std::tan(camera.fovY * 0.5f);
        out[10] = farPlane / (farPlane - nearPlane);
        out[11] = 1.0f;
        out[14] = -nearPlane * farPlane / (farPlane - nearPlane);
    }
}

inline void editorCameraViewProjection(const EditorCamera& camera, float aspect,
                                       float out[16]) {
    float projection[16], view[16];
    editorCameraProjection(camera, aspect, projection);
    const EditorCameraPose pose = editorCameraPose(camera);
    // lookAtLH, same layout as engine::lookAtLH.
    std::memset(view, 0, sizeof(view));
    view[0] = pose.right[0];   view[4] = pose.right[1];   view[8] = pose.right[2];
    view[1] = pose.up[0];      view[5] = pose.up[1];      view[9] = pose.up[2];
    view[2] = pose.forward[0]; view[6] = pose.forward[1]; view[10] = pose.forward[2];
    view[12] = -(pose.right[0] * pose.eye[0] + pose.right[1] * pose.eye[1] +
                 pose.right[2] * pose.eye[2]);
    view[13] = -(pose.up[0] * pose.eye[0] + pose.up[1] * pose.eye[1] +
                 pose.up[2] * pose.eye[2]);
    view[14] = -(pose.forward[0] * pose.eye[0] + pose.forward[1] * pose.eye[1] +
                 pose.forward[2] * pose.eye[2]);
    view[15] = 1.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += projection[k * 4 + row] * view[column * 4 + k];
            out[column * 4 + row] = sum;
        }
    }
}

} // namespace host
