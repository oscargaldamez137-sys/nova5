param([string]$Scenes = ',user-20261001,world-latria,rec-m02_00_00_00-c0000_0006')
# (-File passes one string: scenes separated by commas, an empty one = the default prologue save.)
$SceneList = $Scenes -split ','
# PGO training over several scenes (tools/local/PGO.md; by default the release profile's four): the user's save moved aside once, each scene's baseline
# installed by bench-run (KYTY_BENCH_BASELINE; '' = the default prologue save), pgo-train.ps1 per scene, the raw
# profiles merged into _Build\pgo\windows\kyty-<date>.profdata. The user's save is restored at the end.
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
Set-Location $root
if (Get-Process kyty_emulator -ErrorAction SilentlyContinue) { 'an emulator is running; nothing started'; exit 1 }
python tools\local\bench-windows.py prepare
if ($LASTEXITCODE -ne 0) { 'prepare failed'; exit 1 }
$date = Get-Date -Format yyyyMMdd
$raws = @()
try {
	foreach ($scene in $SceneList) {
		$tag = if ($scene) { $scene } else { 'prologue' }
		if ($scene -and !(Test-Path "$root\_Build\bench-baselines\$scene")) { "no baseline save $scene"; continue }
		if ($scene) { $env:KYTY_BENCH_BASELINE = "$root\_Build\bench-baselines\$scene" }
		else { Remove-Item env:KYTY_BENCH_BASELINE -ErrorAction SilentlyContinue }
		$out = "$root\_Build\pgo\windows\kyty-$date-$tag"
		# (The previous scene's emulator may still be exiting: its executable is copied again.)
		for ($i = 0; $i -lt 90 -and (Get-Process kyty_emulator -ErrorAction SilentlyContinue); $i++) { Start-Sleep 1 }
		"== $tag $(Get-Date -Format HH:mm:ss)"
		& "$root\tools\local\windows\pgo-train.ps1" -Out $out -NoPrecompile
		if (Test-Path "$out.profraw") { $raws += "$out.profraw"; "raw: $((Get-Item "$out.profraw").Length) bytes" }
		else { "NO PROFILE for $tag" }
	}
} finally {
	Remove-Item env:KYTY_BENCH_BASELINE -ErrorAction SilentlyContinue
	Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force
	Start-Sleep 2
	python tools\local\bench-windows.py restore
}
$raws = @(Get-ChildItem "$root\_Build\pgo\windows\kyty-$date-*.profraw" | ForEach-Object { $_.FullName })
if ($raws.Count) {
	& 'C:\Program Files\LLVM\bin\llvm-profdata.exe' merge -o "$root\_Build\pgo\windows\kyty-$date.profdata" @raws
	"merged $($raws.Count) scenes: $root\_Build\pgo\windows\kyty-$date.profdata"
}
