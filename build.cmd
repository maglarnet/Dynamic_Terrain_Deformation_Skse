@echo off
setlocal EnableDelayedExpansion

set "ROOT=%~dp0"
set "BUILD=%ROOT%build\release"

set "VCVARS="
for %%E in (BuildTools Community Professional Enterprise) do (
    for %%P in ("%ProgramFiles(x86)%" "%ProgramFiles%") do (
        if not defined VCVARS (
            if exist "%%~P\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS=%%~P\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
            )
        )
    )
)

if not defined VCVARS (
    echo ERROR: no Visual Studio 2022 vcvars64.bat found.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: vcvars64 failed.
    exit /b 1
)

if /i "%~1"=="repair" (
    echo Rebuilding the CMake cache...
    if exist "%BUILD%\CMakeCache.txt" del /q "%BUILD%\CMakeCache.txt"
    if exist "%BUILD%\CMakeFiles" rmdir /s /q "%BUILD%\CMakeFiles"
)

if exist "%BUILD%\CMakeCache.txt" (
    findstr /c:"CMAKE_TOOLCHAIN_FILE:" "%BUILD%\CMakeCache.txt" >nul || (
        echo Cache is missing the vcpkg toolchain - rebuilding it.
        del /q "%BUILD%\CMakeCache.txt"
        if exist "%BUILD%\CMakeFiles" rmdir /s /q "%BUILD%\CMakeFiles"
    )
)

if not exist "%BUILD%\CMakeCache.txt" (
    cmake --preset release || exit /b 1
)

cmake --build "%BUILD%" || exit /b 1

if exist "%BUILD%\ShaderGen.exe" (
    echo.
    "%BUILD%\ShaderGen.exe" > "%BUILD%\shader-check.log" 2>&1
    if errorlevel 1 (
        echo.
        echo ERROR: the shader harness reported failures. Run it directly to see them:
        echo   %BUILD%\ShaderGen.exe
        type "%BUILD%\shader-check.log"
        exit /b 1
    )
    echo Shaders: ALL PASS
)

if exist "%BUILD%\StampSurfaceTest.exe" (
    "%BUILD%\StampSurfaceTest.exe" || exit /b 1
)

if exist "%BUILD%\TerrainDepthInteropTest.exe" (
    "%BUILD%\TerrainDepthInteropTest.exe" || exit /b 1
)

echo.
echo Done.
endlocal
exit /b 0
