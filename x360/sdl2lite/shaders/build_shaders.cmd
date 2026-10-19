@echo off
rem Compiles the SDL2-lite presentation shaders to Xenos microcode headers with the XDK's fxc.exe.
rem Re-run after editing present.hlsl; the generated headers are committed alongside it.
setlocal
set XDK=%RXDK360%
if "%XDK%"=="" set XDK=E:\RXDK-360
set HERE=%~dp0
"%XDK%\bin\win32\fxc.exe" /nologo /T vs_3_0 /E mainVS /Vn g_presentVertexShader /Fh "%HERE%present_vs.h" "%HERE%present.hlsl" || exit /b 1
"%XDK%\bin\win32\fxc.exe" /nologo /T ps_3_0 /E mainPS /Vn g_presentPixelShader /Fh "%HERE%present_ps.h" "%HERE%present.hlsl" || exit /b 1
echo Shaders compiled
