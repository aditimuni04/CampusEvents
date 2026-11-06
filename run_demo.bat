@echo off
cd /d "%~dp0"
cmake -S . -B build || exit /b 1
cmake --build build --config Release || exit /b 1
if exist build\Release\campusevents.exe (set EXE=build\Release\campusevents.exe) else (set EXE=build\campusevents.exe)
if "%CAMPUS_ADMIN_KEY%"=="" set CAMPUS_ADMIN_KEY=demo-admin
%EXE% serve --db campusevents.db --demo
