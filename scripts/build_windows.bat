@echo off
REM ===========================================================================
REM qt-commander Windows build script
REM
REM Positional args (all required, pass "" for empty ones):
REM   %1 = vcvars bat path        (e.g. C:\...\vcvars64.bat)
REM   %2 = vcvars arch arg        (e.g. amd64, or "" for default)
REM   %3 = qtenv bat path         (e.g. C:\...\qtenv2.bat)
REM   %4 = source root            (project root containing src/)
REM   %5 = build output dir
REM   %6 = install prefix
REM   %7 = build type             (Debug | Release | ...)
REM   %8 = Qt major version       (5 | 6)
REM   %9 = WITH_QML               (ON | OFF)
REM  %10 = CMake generator        ("" to let CMake choose, or e.g. "Ninja")
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
REM shift past first 9 args to reach the optional 10th
shift & shift & shift & shift & shift & shift & shift & shift & shift
set "GENERATOR=%~1"

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

echo.
echo === Setting up MSVC ===
call "%VCVARS%" %VCVARS_ARGS%
if errorlevel 1 exit /b 1

echo.
echo === Setting up Qt ===
call "%QTENV%"
if errorlevel 1 exit /b 1

set "CMAKE_COMMON=-DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_INSTALL_PREFIX=%INSTALL_PREFIX%"

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
echo === Done ===
exit /b 0
