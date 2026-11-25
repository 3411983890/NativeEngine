@echo off
rem Launch the rig acceptance scene: flat floor, four box steps, a turn
rem platform and a blocking wall. Built for checking foot planting, in-place
rem turning and step-up IK by hand.
cd /d "%~dp0.."
if not exist "build\NativeGPU.exe" (
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1
    if errorlevel 1 (
        echo Build failed.
        pause
        exit /b 1
    )
)
echo Controls: W/A/S/D move, Shift run, Space jump, E interact,
echo           left-drag or arrow keys turn the camera, F5 reset, F7/F12 save/load.
echo Close the window to write reports\animation\rig-session.json.
build\NativeGPU.exe --scene .screen/character-rig.scene.json --textures assets/builtin/textures --report reports/animation/rig-session.json
