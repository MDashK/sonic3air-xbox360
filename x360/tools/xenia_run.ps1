# Runs an .xex in this project's Xenia Canary for a while, saves a screenshot of the Xenia window,
# then closes Xenia. Used to check the port without manual interaction.
#
#   powershell -File xenia_run.ps1 -Xex <path\default.xex> [-Seconds 60] [-Screenshot <png path>]
param(
	[Parameter(Mandatory = $true)][string]$Xex,
	[int]$Seconds = 60,
	[string]$Screenshot = ""
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Window
{
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
	[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
"@

$xeniaDir = Join-Path $PSScriptRoot "..\..\..\xenia_canary_windows"
$process = Start-Process -FilePath (Join-Path $xeniaDir "xenia_canary.exe") -ArgumentList "`"$Xex`"" -WorkingDirectory $xeniaDir -PassThru
Start-Sleep -Seconds $Seconds

if ($Screenshot -ne "" -and -not $process.HasExited) {
	$process.Refresh()
	$handle = $process.MainWindowHandle
	if ($handle -ne [IntPtr]::Zero) {
		[Win32Window]::SetForegroundWindow($handle) | Out-Null
		Start-Sleep -Milliseconds 500
		$rect = New-Object Win32Window+RECT
		[Win32Window]::GetWindowRect($handle, [ref]$rect) | Out-Null
		$width = $rect.Right - $rect.Left
		$height = $rect.Bottom - $rect.Top
		$bitmap = New-Object System.Drawing.Bitmap $width, $height
		$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
		# PrintWindow with PW_RENDERFULLCONTENT (2) also captures D3D12 content when the desktop can't be read with CopyFromScreen
		$hdc = $graphics.GetHdc()
		[Win32Window]::PrintWindow($handle, $hdc, 2) | Out-Null
		$graphics.ReleaseHdc($hdc)
		$bitmap.Save($Screenshot, [System.Drawing.Imaging.ImageFormat]::Png)
		$graphics.Dispose()
		$bitmap.Dispose()
		"Screenshot saved: $Screenshot ($width x $height)"
	} else {
		"No Xenia window found for the screenshot"
	}
}

$exited = $process.HasExited
if (-not $exited) { $process | Stop-Process -Force }
"Xenia exited by itself: $exited"
