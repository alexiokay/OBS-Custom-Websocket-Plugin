@echo off
setlocal enabledelayedexpansion

echo ==============================================
echo VortiDeck OBS Plugin Build Script
echo ==============================================

cd /d "%~dp0"

set BUILD_DIR=build_x64
set CONFIG=RelWithDebInfo

echo Checking for existing build directory...

if exist "%BUILD_DIR%" (
    echo ✅ Found existing build directory: %BUILD_DIR%
    echo 🔨 Building plugin ^(incremental build^)...
    
    REM Method 1: Direct build using existing build_x64
    cmake --build %BUILD_DIR% --config %CONFIG%
    
    if !errorlevel! neq 0 (
        echo ⚠️  Incremental build failed, trying clean rebuild...
        rmdir /s /q %BUILD_DIR%
        goto clean_build
    ) else (
        goto build_success
    )
) else (
    echo 📁 Creating new build directory: %BUILD_DIR%
    :clean_build
    mkdir %BUILD_DIR%
    cd %BUILD_DIR%
    
    echo 🔧 Configuring CMake...
    cmake .. -G "Visual Studio 17 2022" -A x64
    
    if !errorlevel! neq 0 (
        echo ❌ CMake configuration failed!
        pause
        exit /b !errorlevel!
    )
    
    echo 🔨 Building plugin...
    cmake --build . --config %CONFIG%
    
    if !errorlevel! neq 0 (
        echo ❌ Build failed!
        pause
        exit /b !errorlevel!
    )
    
    cd ..
)

:build_success
echo ==============================================
echo ✅ BUILD SUCCESSFUL!
echo ==============================================
echo 📦 Plugin location: %BUILD_DIR%\%CONFIG%\vorti-obs-plugin.dll
echo 🎯 Ready to copy to OBS plugins folder!
echo ==============================================

if exist "%BUILD_DIR%\%CONFIG%\vorti-obs-plugin.dll" (
    echo 📊 File size: 
    dir "%BUILD_DIR%\%CONFIG%\vorti-obs-plugin.dll" | find "vorti-obs-plugin.dll"
) else (
    echo ⚠️  Warning: DLL file not found at expected location
)

echo.
echo Press any key to exit...
pause >nul