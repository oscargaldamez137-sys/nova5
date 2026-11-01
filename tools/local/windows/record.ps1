param([Parameter(Mandatory)][string]$Out, [double]$Seconds = 30, [int]$Fps = 30, [int]$Width = 1280, [int]$Height = 720)
# Video of the emulator window for $Seconds (Windows Graphics Capture of the kyty_emulator window, no cursor, the
# hardware H.264 encoder through Media Foundation) into $Out (.mkv). Frame timestamps are wall-clock Unix seconds
# (the SLOW Frame lines carry unix_ms; tools/local/hitch-sheet.py takes the video) and are also drawn in the corner.
# It never touches the window focus. Needs ffmpeg 8+ (gfxcapture): winget install Gyan.FFmpeg.
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (!$ffmpeg) { $ffmpeg = "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ffmpeg.exe" }
if (!(Test-Path $ffmpeg)) {
	$ffmpeg = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue |
		Select-Object -First 1 -ExpandProperty FullName
}
if (!$ffmpeg) { 'no ffmpeg'; exit 1 }
$filter = "gfxcapture=window_exe=kyty_emulator\.exe:capture_cursor=0:max_framerate=${Fps}:width=${Width}:height=${Height}:" +
	"resize_mode=scale_aspect,hwdownload,format=bgra,setpts=RTCTIME/(TB*1000000),trim=duration=$Seconds," +
	"drawtext=fontfile='C\:/Windows/Fonts/consola.ttf':text='%{pts\:flt}':x=8:y=8:fontsize=20:fontcolor=yellow:" +
	"box=1:boxcolor=black@0.6,format=nv12"
& $ffmpeg -y -hide_banner -loglevel error -filter_complex $filter -c:v h264_mf -hw_encoding 1 -b:v 6M $Out 2>&1 | ForEach-Object { "$_" }
"video: $Out"
