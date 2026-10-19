@echo off
rem Compiles the D3D9 drawer shaders to Xenos microcode headers with the XDK's fxc.exe.
rem Re-run after editing drawer.hlsl; the generated headers are kept next to it.
setlocal
set XDK=%RXDK360%
if "%XDK%"=="" set XDK=E:\RXDK-360
set FXC="%XDK%\bin\win32\fxc.exe" /nologo
set HERE=%~dp0
%FXC% /T vs_3_0 /E mainVS     /Vn g_drawerVS         /Fh "%HERE%drawer_vs.h"       "%HERE%drawer.hlsl" || exit /b 1
%FXC% /T ps_3_0 /E colorPS    /Vn g_drawerColorPS    /Fh "%HERE%drawer_color_ps.h"    "%HERE%drawer.hlsl" || exit /b 1
%FXC% /T ps_3_0 /E texturedPS /Vn g_drawerTexturedPS /Fh "%HERE%drawer_textured_ps.h" "%HERE%drawer.hlsl" || exit /b 1
%FXC% /T ps_3_0 /E indexedPS  /Vn g_drawerIndexedPS  /Fh "%HERE%drawer_indexed_ps.h"  "%HERE%drawer.hlsl" || exit /b 1
%FXC% /T ps_3_0 /E upscalePS  /Vn g_drawerUpscalePS  /Fh "%HERE%drawer_upscale_ps.h"  "%HERE%drawer.hlsl" || exit /b 1
echo Shaders compiled
