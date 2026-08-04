@echo off
REM ===========================================================================
REM qt-commander Windows build script
REM
REM Positional args (all required, pass "" for empty ones):
REM   %1 = vcvars bat path        (msvc; e.g. C:\...\vcvars64.bat)
REM                             or MinGW bin dir (mingw; e.g. C:\...\mingw64\bin)
REM   %2 = vcvars arch arg        (msvc; e.g. amd64, or "" for default)
REM   %3 = qtenv2.bat path        (both toolchains; MinGW Qt kits ship one too)
REM   %4 = source root            (project root containing src/)
REM   %5 = build output dir
REM   %6 = install prefix
REM   %7 = build type             (Debug | Release | ...)
REM   %8 = Qt major version       (5 | 6)
REM   %9 = WITH_QML               (ON | OFF)
REM  %10 = CMake generator        ("" to let CMake choose, or e.g. "Ninja")
REM  %11 = toolchain              ("msvc" default | "mingw")
REM ===========================================================================

setlocal enabledelayedexpansion

set "VCVARS=%~1"
set "VCVARS_ARGS=%~2"
set "QTENV=%~3"
REM Arg 3's directory is the Qt bin dir in both modes (msvc: the dir of
REM the qtenv bat; mingw: the qtenv bat's dir, or the bin dir itself when
REM passed).  Capture it BEFORE the shifts below -- after shifting, %~dp3
REM would refer to a different argument.
set "QT_BIN_DIR=%~dp3"
set "SRC_ROOT=%~4"
set "BUILD_ROOT=%~5"
set "INSTALL_PREFIX=%~6"
set "BUILD_TYPE=%~7"
set "QT_MAJOR=%~8"
set "WITH_QML=%~9"
REM shift past first 9 args to reach the optional 10th/11th
shift & shift & shift & shift & shift & shift & shift & shift & shift
set "GENERATOR=%~1"
shift
set "TOOLCHAIN=%~1"
if "%TOOLCHAIN%"=="" set "TOOLCHAIN=msvc"

echo [build] vcvars  = %VCVARS%
echo [build] arch    = %VCVARS_ARGS%
echo [build] qtenv   = %QTENV%
echo [build] src     = %SRC_ROOT%
echo [build] build   = %BUILD_ROOT%
echo [build] install = %INSTALL_PREFIX%
echo [build] type    = %BUILD_TYPE%
echo [build] qt      = %QT_MAJOR%
echo [build] qml     = %WITH_QML%
echo [build] gen     = %GENERATOR%
echo [build] toolch  = %TOOLCHAIN%

if /i "%TOOLCHAIN%"=="mingw" (
    echo.
    echo === Setting up MinGW ===
    if "%VCVARS%"=="" (
        echo [build] ERROR: MinGW bin dir missing ^(arg 1^)
        exit /b 1
    )
    if not exist "%VCVARS%\g++.exe" (
        echo [build] ERROR: no g++.exe in "%VCVARS%"
        exit /b 1
    )
    set "PATH=%VCVARS%;%PATH%"
    call "%QTENV%"
    if errorlevel 1 exit /b 1
    REM MinGW needs an explicit generator; prefer the Qt-bundled Ninja.
    REM <Qt root>/Tools/Ninja sits 3 levels above the Qt bin dir.
    if "%GENERATOR%"=="" (
        for %%n in ("%QT_BIN_DIR%..\..\..\Tools\Ninja\ninja.exe") do (
            if exist "%%~fn" (
                set "GENERATOR=Ninja"
                REM !PATH! (delayed expansion) keeps the mingw prefix just
                REM prepended above; %PATH% would resolve to the original
                REM value and drop it.
                set "PATH=%%~dpn;!PATH!"
            )
        )
    )
) else (
    echo.
    echo === Setting up MSVC ===
    call "%VCVARS%" %VCVARS_ARGS%
    if errorlevel 1 exit /b 1

    echo.
    echo === Setting up Qt ===
    call "%QTENV%"
    if errorlevel 1 exit /b 1
)

set "QT_PREFIX=%QT_BIN_DIR%\.."

REM MinGW: pin the compiler explicitly -- several gcc builds may sit on
REM PATH (e.g. a Strawberry Perl toolchain), and CMake would pick the
REM first one instead of the requested toolchain.
set "COMPILER_ARGS="
if /i "%TOOLCHAIN%"=="mingw" (
    set "COMPILER_ARGS=-DCMAKE_C_COMPILER=%VCVARS%\gcc.exe -DCMAKE_CXX_COMPILER=%VCVARS%\g++.exe"
)

set "CMAKE_COMMON=-DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_INSTALL_PREFIX=%INSTALL_PREFIX% -DCMAKE_PREFIX_PATH=%QT_PREFIX% %COMPILER_ARGS%"

set "GEN_ARG="
if not "%GENERATOR%"=="" set "GEN_ARG=-G %GENERATOR%"

echo.
echo === Building qt-injector ===
cmake -S "%SRC_ROOT%\src\injector" -B "%BUILD_ROOT%\injector" %CMAKE_COMMON% %GEN_ARG%
if errorlevel 1 exit /b 1
cmake --build "%BUILD_ROOT%\injector" --config %BUILD_TYPE%
if errorlevel 1 exit /b 1
cmake --install "%BUILD_ROOT%\injector" --config %BUILD_TYPE%
if errorlevel 1 exit /b 1

echo.
echo === Building libqt-commander ===
cmake -S "%SRC_ROOT%\src\library" -B "%BUILD_ROOT%\library" %CMAKE_COMMON% %GEN_ARG% -DQT_MAJOR_VERSION=%QT_MAJOR% -DBUILD_SERVER=OFF -DWITH_QML=%WITH_QML%
if errorlevel 1 exit /b 1
cmake --build "%BUILD_ROOT%\library" --config %BUILD_TYPE%
if errorlevel 1 exit /b 1
cmake --install "%BUILD_ROOT%\library" --config %BUILD_TYPE%
if errorlevel 1 exit /b 1

echo.
echo === Deploying dependency closure ===
REM The injector preloads the library's Qt closure into the target; the
REM closure DLLs must sit next to the library or LoadLibraryW fails there.
REM Qt6's windeployqt handles DLL inputs; Qt5's only handles exes, so the
REM Qt5 closure is copied explicitly.  %~dp3 = the Qt bin dir in both
REM modes (msvc: dir of qtenv2.bat; mingw: the Qt bin dir itself).
if "%QT_MAJOR%"=="6" (
    "%QT_BIN_DIR%windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%INSTALL_PREFIX%\bin\libqt-commander.dll"
    if errorlevel 1 (
        echo [build] WARNING: windeployqt failed -- dependency closure may be incomplete
    )
) else (
    for %%d in (Qt5Core.dll Qt5Gui.dll Qt5Widgets.dll Qt5Quick.dll Qt5Qml.dll Qt5QmlModels.dll Qt5Network.dll) do (
        if exist "%QT_BIN_DIR%%%d" (
            copy /y "%QT_BIN_DIR%%%d" "%INSTALL_PREFIX%\bin\" >nul
            echo [build] closure %%d
        ) else (
            echo [build] WARNING: %%d not found in "%QT_BIN_DIR%"
        )
    )
)
if /i "%TOOLCHAIN%"=="mingw" (
    REM windeployqt (Qt6) ships the Qt kit's own (older) compiler runtime;
    REM the executables were built with %VCVARS%, so its runtime must win
    REM (libstdc++ is backward-compatible: older-ABI DLLs load a newer
    REM libstdc++-6.dll, but not vice versa).
    for %%d in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
        if exist "%VCVARS%\%%d" (
            copy /y "%VCVARS%\%%d" "%INSTALL_PREFIX%\bin\" >nul
            echo [build] runtime %%d from compiler
        )
    )
)

echo.
echo === Done ===
exit /b 0
