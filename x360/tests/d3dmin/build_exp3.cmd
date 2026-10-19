@echo off
rem Experiment 3: main.cpp compiled by the stock XDK cl.exe, linked by the RXDK clang pipeline
rem (ld.lld + RXDK _start + XexTool). Separates "clang code generation" from "link/startup/libraries".
setlocal
set XDK=%RXDK360%
if "%XDK%"=="" set XDK=E:\RXDK-360
set HERE=%~dp0
set OBJ=%HERE%obj\exp3_ms
if not exist "%OBJ%" mkdir "%OBJ%"

set PATH=%XDK%\bin\win32;%PATH%
set INCLUDE=%XDK%\include\xbox
set LIB=%XDK%\lib\xbox

cl.exe /nologo /c /O2 /MT /GS- /GR- /W3 /D_XBOX /DNDEBUG /D_MBCS /DD3DMIN_EXP3 /Fo"%OBJ%\main.obj" "%HERE%main.cpp" || exit /b 1
lib.exe /nologo /OUT:"%OBJ%\d3dmin_ms.lib" "%OBJ%\main.obj" || exit /b 1
python -I "%HERE%..\..\tools\rxdk_upstream\coff2elf.py" archive "%OBJ%\d3dmin_ms.lib" -o "%OBJ%\d3dmin_ms.a" --ppc64 --runtime "%LIB%\libc.a" --runtime "%LIB%\libcpp.a" || exit /b 1

"E:\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" "%HERE%d3dmin_clang.vcxproj" /t:Rebuild /p:Configuration=Release "/p:Platform=Xbox 360" /p:Exp3=true "/p:ExtraLibs=%OBJ%\d3dmin_ms.a" "/p:OutDir=%HERE%bin\exp3\\" "/p:IntDir=%HERE%obj\exp3\\" /v:minimal /nologo || exit /b 1
echo Built %HERE%bin\exp3\default.xex
