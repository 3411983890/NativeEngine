@echo off
rem 打开接触驱动动作系统示例（Character rig - steps and turns）
cd /d "%~dp0"
start "" "build\NativeGPU.exe" --scene ".screen\character-rig.scene.json"
