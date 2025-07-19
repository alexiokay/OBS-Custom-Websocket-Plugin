@echo off
echo Building mDNS Discovery Test...

REM Create build directory
if not exist build mkdir build
cd build

REM Configure with CMake
cmake .. -G "Visual Studio 17 2022" -A x64
if %ERRORLEVEL% neq 0 (
    echo CMake configuration failed!
    pause
    exit /b 1
)

REM Build the project
cmake --build . --config Release
if %ERRORLEVEL% neq 0 (
    echo Build failed!
    pause
    exit /b 1
)

echo Build completed successfully!
echo.
echo Running mDNS Discovery Test...
echo.

REM Run the executable
Release\mdns_discovery_test.exe

pause