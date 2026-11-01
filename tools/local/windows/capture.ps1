param([Parameter(Mandatory)][string]$Out, [double]$Seconds = 30, [int]$IntervalMs = 200, [int]$Width = 640)
# Screen capture of the emulator's game image for $Seconds: every $IntervalMs the window's client area, scaled to
# $Width pixels wide, as $Out\<unix ms>.jpg (the SLOW Frame lines carry unix_ms; tools/local/hitch-sheet.py puts
# the captures of the slow frames side by side). It never touches the window focus.
Add-Type @"
using System; using System.Runtime.InteropServices;
public class C { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p); }
"@
[C]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$process = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if (!$process) { 'no emulator window'; exit 1 }
$rect = New-Object C+RECT
$origin = New-Object C+POINT
if (![C]::GetClientRect($process.MainWindowHandle, [ref]$rect) -or ![C]::ClientToScreen($process.MainWindowHandle, [ref]$origin)) {
	'no client area'; exit 1
}
$w = $rect.Right; $h = $rect.Bottom
$outH = [int]($h * $Width / $w)
$full = New-Object System.Drawing.Bitmap $w, $h
$grab = [System.Drawing.Graphics]::FromImage($full)
$small = New-Object System.Drawing.Bitmap $Width, $outH
$scale = [System.Drawing.Graphics]::FromImage($small)
$scale.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::Bilinear
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
$quality = New-Object System.Drawing.Imaging.EncoderParameters 1
$quality.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 70L
$end = [DateTimeOffset]::UtcNow.AddSeconds($Seconds)
$count = 0
while ([DateTimeOffset]::UtcNow -lt $end) {
	$started = [DateTimeOffset]::UtcNow
	$grab.CopyFromScreen($origin.X, $origin.Y, 0, 0, $full.Size)
	$stamp = $started.ToUnixTimeMilliseconds()
	$scale.DrawImage($full, 0, 0, $Width, $outH)
	$small.Save((Join-Path $Out "$stamp.jpg"), $codec, $quality)
	$count++
	$left = $IntervalMs - ([DateTimeOffset]::UtcNow - $started).TotalMilliseconds
	if ($left -gt 0) { Start-Sleep -Milliseconds $left }
}
"captures: $count in $Out"
