@echo off
REM ===========================================================================
REM qt-commander Windows build script
REM
REM Positional args (all required, pass "" for empty ones):
REM   %1 = vcvars bat path        (msvc; e.g. C:\...\vcvars64.bat)
REM                             or MinGW bin dir (mingw; e.g. C:\...\mingw64\bin)
REM   %2 = vcvars arch arg        (msvc; e.g. amd64, or "" for default)
REM   %3 = qtenv bat path         (msvc; e.g. C:\...\qtenv2.bat)
REM                             or Qt bin dir (mingw; e.g. C:\...\mingw_64\bin)
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
    if "%QTENV%"=="" (
        echo [build] ERROR: Qt bin dir missing ^(arg 3^)
        exit /b 1
    )
    if not exist "%QTENV%\qmake.exe" (
        echo [build] ERROR: no qmake.exe in "%QTENV%"
        exit /b 1
    )
    set "PATH=%VCVARS%;%QTENV%;%PATH%"
    REM MinGW needs an explicit generator; prefer the Qt-bundled Ninja.
    REM <Qt root>/Tools/Ninja sits 3 levels above the Qt bin dir.
    if "%GENERATOR%"=="" (
        for %%n in ("%QTENV%\..\..\..\Tools\Ninja\ninja.exe") do (
            if exist "%%~fn" (
                set "GENERATOR=Ninja"
                REM !PATH! (delayed expansion) keeps the mingw/qt prefix
                REM just prepended above; %PATH% would resolve to the
                REM original value and drop it.
                set "PATH=%%~dpn;!PATH!"
            )
        )
    )
    set "QT_PREFIX=%QTENV%\.."
) else (
    echo.
    echo === Setting up MSVC ===
    call "%VCVARS%" %VCVARS_ARGS%
    if errorlevel 1 exit /b 1

    echo.
    echo === Setting up Qt ===
    call "%QTENV%"
    if errorlevel 1 exit /b 1
    set "QT_PREFIX=%~dp3\.."
)

set "CMAKE_COMMON=-DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_INSTALL_PREFIX=%INSTALL_PREFIX% -DCMAKE_PREFIX_PATH=%QT_PREFIX%"

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
if /i "%TOOLCHAIN%"=="mingw" (
    REM windeployqt against the library copies its Qt closure (and the
    REM MinGW runtime DLLs) next to it.  The injector preloads these into
    REM the target process; without them, LoadLibraryW fails in the
    REM target.  This automates what the MSVC flow does by hand.
    REM Qt6's windeployqt handles DLL inputs; Qt5's only handles exes, so
    REM the Qt5 closure is copied explicitly below.
    if "%QT_MAJOR%"=="6" (
        "%QTENV%\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%INSTALL_PREFIX%\bin\libqt-commander.dll"
        if errorlevel 1 (
            echo [build] WARNING: windeployqt failed -- dependency closure may be incomplete
        )
    ) else (
        for %%d in (Qt5Core.dll Qt5Gui.dll Qt5Widgets.dll Qt5Quick.dll Qt5Qml.dll Qt5QmlModels.dll Qt5Network.dll) do (
            if exist "%QTENV%\%%d" (
                copy /y "%QTENV%\%%d" "%INSTALL_PREFIX%\bin\" >nul
                echo [build] closure %%d
            ) else (
                echo [build] WARNING: %%d not found in "%QTENV%"
            )
        )
    )
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
