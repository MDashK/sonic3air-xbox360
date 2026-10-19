@echo off
rem Builds d3dmin with the stock XDK toolchain (cl.exe / link.exe / imagexex.exe) without MSBuild.
rem Needed while the RXDK "2010-01" MSBuild toolset requires VS 2022 17.14+ (this machine has 17.6).
setlocal
set XDK=%RXDK360%
if "%XDK%"=="" set XDK=E:\RXDK-360
set HERE=%~dp0
set OUT=%HERE%bin\stock
set OBJ=%HERE%obj\stock
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

set PATH=%XDK%\bin\win32;%PATH%
set INCLUDE=%XDK%\include\xbox
set LIB=%XDK%\lib\xbox

cl.exe /nologo /c /O2 /MT /GS- /GR- /W3 /D_XBOX /DNDEBUG /D_MBCS /Fo"%OBJ%\main.obj" "%HERE%main.cpp" || exit /b 1
link.exe /nologo /DEBUG /OUT:"%OBJ%\d3dmin.exe" /PDB:"%OBJ%\d3dmin.pdb" "%OBJ%\main.obj" xapilib.lib d3d9.lib xgraphics.lib xboxkrnl.lib || exit /b 1
imagexex.exe /nologo /IN:"%OBJ%\d3dmin.exe" /OUT:"%OUT%\default.xex" || exit /b 1
echo Built %OUT%\default.xex
