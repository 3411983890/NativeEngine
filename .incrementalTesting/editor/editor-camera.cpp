// Native contract test for the editor camera. It links nothing but the header, so it can
// run without D3D12, a window or the engine: the failure it guards against is camera math
// drifting away from the render conventions, which is cheap to test and expensive to see.
#include "../../host/editor_camera.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

void near(float actual, float expected, float tolerance, const char* what) {
    if (std::abs(actual - expected) <= tolerance) return;
    std::fprintf(stderr, "FAIL: %s (actual %.6f, expected %.6f)\n", what,
                 static_cast<double>(actual), static_cast<double>(expected));
    ++failures;
}

float dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

host::EditorCamera baseCamera() {
    host::EditorCamera camera;
    camera.yaw = 0.0f;
    camera.pitch = 0.0f;
    camera.distance = 10.0f;
    camera.target[0] = 0.0f;
    camera.target[1] = 0.0f;
    camera.target[2] = 0.0f;
    return camera;
}

} // namespace

int main() {
    {
        const auto pose = host::editorCameraPose(baseCamera());
        near(pose.eye[0], 0.0f, 1e-5f, "front eye x");
        near(pose.eye[1], 0.0f, 1e-5f, "front eye y");
        near(pose.eye[2], -10.0f, 1e-5f, "front eye z");
        near(pose.forward[2], 1.0f, 1e-5f, "front looks toward +Z");
        near(pose.right[0], 1.0f, 1e-5f, "front right is +X");
        near(pose.up[1], 1.0f, 1e-5f, "front up is +Y");
        near(dot(pose.right, pose.up), 0.0f, 1e-5f, "right and up are orthogonal");
        near(dot(pose.right, pose.forward), 0.0f, 1e-5f, "right and forward are orthogonal");
    }
    {
        host::EditorCamera camera = baseCamera();
        host::editorCameraOrbit(camera, 100.0f, 0.0f);
        check(camera.yaw < 0.0f, "dragging right turns the camera right");
        host::editorCameraOrbit(camera, 0.0f, 100.0f);
        check(camera.pitch < 0.0f, "dragging down raises the camera (negative pitch)");
    }
    {
        host::EditorCamera camera = baseCamera();
        host::editorCameraDolly(camera, 1.0f);
        check(camera.distance < 10.0f, "wheel forward moves closer");
        host::editorCameraDolly(camera, -1.0f);
        near(camera.distance, 10.0f, 1e-4f, "wheel back restores the distance");
    }
    {
        host::EditorCamera camera = baseCamera();
        const float before = camera.target[1];
        host::editorCameraPan(camera, 0.0f, 100.0f, 1000.0f);
        check(camera.target[1] > before, "dragging down moves the target up");
        near(camera.target[2], 0.0f, 1e-5f, "pan stays in the camera plane");
    }
    {
        host::EditorCamera camera = baseCamera();
        host::editorCameraMove(camera, 1.0f, 0.0f, 0.0f, 0.5f, 4.0f);
        near(camera.target[2], 2.0f, 1e-5f, "fly moves along the view direction");
    }
    {
        host::EditorCamera camera = baseCamera();
        const float minimum[3] = {-1.0f, -1.0f, -1.0f};
        const float maximum[3] = {1.0f, 3.0f, 1.0f};
        check(host::editorCameraFocus(camera, minimum, maximum), "focus accepts finite bounds");
        near(camera.target[1], 1.0f, 1e-5f, "focus centers on the bounds");
        check(camera.distance > 6.0f, "focus backs off to frame the bounds");
        check(camera.distance >= 1.5f, "focus never puts the camera inside a point");
        const float pointMinimum[3] = {2.0f, 2.0f, 2.0f};
        const float pointMaximum[3] = {2.0f, 2.0f, 2.0f};
        check(host::editorCameraFocus(camera, pointMinimum, pointMaximum), "focus accepts a point");
        check(camera.distance >= 1.5f, "point focus keeps a minimum distance");
    }
    {
        host::EditorCamera camera = baseCamera();
        host::editorCameraSetView(camera, "top");
        check(camera.orthographic, "top view is orthographic");
        const auto pose = host::editorCameraPose(camera);
        check(pose.eye[1] > pose.target[1], "top view looks down");
        host::editorCameraSetView(camera, "front");
        const auto front = host::editorCameraPose(camera);
        near(front.forward[2], 1.0f, 1e-4f, "front view looks toward +Z");
        host::editorCameraSetView(camera, "perspective");
        check(!camera.orthographic, "perspective view clears orthographic");
    }
    {
        host::EditorCamera camera = baseCamera();
        float perspective[16], orthographic[16];
        host::editorCameraProjection(camera, 16.0f / 9.0f, perspective);
        check(perspective[11] == 1.0f, "perspective has w = z");
        camera.orthographic = true;
        host::editorCameraProjection(camera, 16.0f / 9.0f, orthographic);
        check(orthographic[11] == 0.0f && orthographic[15] == 1.0f, "orthographic has w = 1");
        float viewProjection[16];
        host::editorCameraViewProjection(camera, 16.0f / 9.0f, viewProjection);
        for (float value : viewProjection) check(std::isfinite(value), "view-projection is finite");
    }
    {
        host::EditorCamera camera = baseCamera();
        camera.pitch = 9.0f;
        host::editorCameraClamp(camera);
        check(camera.pitch < 1.5708f, "pitch is clamped below the pole");
        camera.distance = -4.0f;
        host::editorCameraClamp(camera);
        check(camera.distance > 0.0f, "distance stays positive");
    }

    if (failures) {
        std::printf("{\"success\": false, \"failures\": %d}\n", failures);
        return 1;
    }
    std::printf("{\"success\": true, \"checks\": 26}\n");
    return 0;
}
