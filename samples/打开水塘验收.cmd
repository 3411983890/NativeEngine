@echo off
rem Interactive pond acceptance: wading, contact foam, splash droplets and the
rem ripple shape/variety work. Uses the effects host build because the splash stage
rem needs the GPU effect runtime; the default build\NativeGPU.exe also works but has
rem no splash stage.
setlocal
cd /d "%~dp0.."
set "PROJECT=%USERPROFILE%\Documents\nativeEngineProject\InteractivePond"
if not exist "%PROJECT%\.screen\main.scene.json" (
    echo InteractivePond project not found: %PROJECT%
    pause
    exit /b 1
)
if not exist "build\pond-host\NativeGPU.exe" (
    echo Building the water/effects host...
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-effects-host.ps1 -OutputDirectory build\pond-host
    if errorlevel 1 (
        echo Build failed.
        pause
        exit /b 1
    )
)
echo Controls: W/A/S/D move, Shift run, Space jump, E interact,
echo           left-drag or arrow keys turn the camera, F5 reset, F7/F12 save/load.
echo Walk into the pond: the ripples must not spread as squares, and consecutive
echo steps must differ in size and strength. Close the window to write a report.
build\pond-host\NativeGPU.exe --scene "%PROJECT%\.screen\main.scene.json" --textures "%PROJECT%\assets\textures" --splash water_splash --validation --report "%PROJECT%\reports\water-realism\session.json"
endlocal
