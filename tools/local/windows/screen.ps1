param([string]$Out, [double]$Scale = 0.5, [switch]$Client, [int]$ClientWidth = 640)
# Focus the emulator window and capture the visible part of the screen (physical pixels),
# scaled down for reading. Prints the window title (frame and fps counters).
# -Client: only the emulator window's client area (the game image), scaled to 640x360, so
# pixel positions do not depend on the display's resolution or where the window is.
# -ClientWidth: another width for it (the window's aspect; 640: 640x360).
Add-Type @"
using System; using System.Runtime.InteropServices;
public class S { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p); }
"@
[S]::SetProcessDPIAware() | Out-Null
$p = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if ($p) {
	if ([S]::IsIconic($p.MainWindowHandle)) { [S]::ShowWindow($p.MainWindowHandle, 9) | Out-Null }
	[S]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 400
}
$x = 0; $y = 0
$w = [S]::GetSystemMetrics(0); $h = [S]::GetSystemMetrics(1)
$outW = [int]($w * $Scale); $outH = [int]($h * $Scale)
if ($Client -and $p) {
	$rect = New-Object S+RECT
	$origin = New-Object S+POINT
	if ([S]::GetClientRect($p.MainWindowHandle, [ref]$rect) -and [S]::ClientToScreen($p.MainWindowHandle, [ref]$origin) -and
	    $rect.Right -gt 0 -and $rect.Bottom -gt 0) {
		$x = $origin.X; $y = $origin.Y; $w = $rect.Right; $h = $rect.Bottom
		$outW = $ClientWidth; $outH = if ($ClientWidth -eq 640) { 360 } else { [int]($ClientWidth * $h / $w) }
	}
}
Add-Type -AssemblyName System.Drawing
$full = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($x, $y, 0, 0, $full.Size)
$small = New-Object System.Drawing.Bitmap $outW, $outH
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
$g2.DrawImage($full, 0, 0, $small.Width, $small.Height)
$small.Save($Out)
if ($p) { $p.Refresh(); "screen=$([S]::GetSystemMetrics(0))x$([S]::GetSystemMetrics(1)) capture=${w}x$h@$x,$y title=$($p.MainWindowTitle)" } else { "emulator not running" }
