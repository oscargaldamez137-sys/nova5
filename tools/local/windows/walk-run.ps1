param([string]$Label = 'walk', [string]$Exe = '', [string[]]$Extra = @(), [int]$TurnMs = 250,
      [string]$TurnKey = 'h', [int]$WalkMs = 25000, [string]$SideKey = '', [int]$SideMs = 0,
      [switch]$KeepRunning, [switch]$Reuse, [switch]$Prof, [switch]$Measure, [int]$TurnAtMs = 0,
      [string]$Plan = '', [int]$Vblank = 0, [string]$ProfThread = '', [switch]$PerSecond, [string[]]$Set = @(), [switch]$Census, [switch]$RenderCensus, [switch]$TraceWrites, [switch]$TraceMarks, [int]$TraceDelayMs = 0, [string]$Patch = '', [switch]$NoPrecompile,
      [string]$Game = '')
# -Plan "w:down:0,h:down:5000,h:up:5500,w:up:25000": the whole key sequence from one process
# (keys.ps1), so the turn lands at the same place every run; it replaces -TurnMs/-SideKey.
# The user's low-fps route: from the baseline save's spawn, turn the camera a little to the
# right and walk straight on for 25 s. A live timeline trace covers the walk (every flip, GPU
# span and render-thread idle interval); screenshots every 5 s show where each part was.
#   walk-run.ps1 -Label base                 fresh launch (bench-run.ps1), then the walk
#   walk-run.ps1 -Reuse                      walk in the running game (already at the spawn)
$S = $PSScriptRoot
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
Set-Location $root
$out = "$root\_Build\walk\$Label"
New-Item -ItemType Directory -Force $out | Out-Null
if (!$Reuse) {
	$params = @{ Label = $Label; KeepRunning = $true; NoWalk = $true; ShotOnly = $true }
	if ($Exe) { $params['Exe'] = $Exe }
	if ($Extra.Count) { $params['Extra'] = $Extra }
	if ($Vblank -gt 0) { $params['Vblank'] = $Vblank }
	# -Set KEY=VALUE overrides a switch of the launch config (-Extra cannot: the config wins).
	if ($Set.Count) { $params['Set'] = $Set }
	if ($Patch) { $params['Patch'] = $Patch }
	# -NoPrecompile: first encounters (with -Set KYTY_SHADER_WARMUP=0: nothing compiled before).
	if ($NoPrecompile) { $params['NoPrecompile'] = $true }
	# -Game <folder>: another game version (bench-run.ps1 -Game).
	if ($Game) { $params['Game'] = $Game }
	& "$S\bench-run.ps1" @params | Select-Object -Last 3
}
if (!(Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object { $_.Threads.Count -gt 1 })) { 'emulator not running'; exit 1 }

# -Prof: sample the render thread during the walk instead (profreport.exe, KYTY_PROF_RANGE).
# -ProfThread <tid|auto>: sample that thread instead; auto = the busiest thread without a name
# (the guest's main thread).
$trace = if ($Prof) { "$out\prof.bin" } elseif ($Measure) { $Label } else { "$out\trace.bin" }
# Profiles are symbolized against the build that ran: keep its executable and linker map.
if ($Prof -or $ProfThread) {
	Copy-Item "$root\_Build\windows\kyty_emulator.exe" "$out\kyty_emulator.exe" -ErrorAction SilentlyContinue
	Copy-Item "$root\_Build\windows\kyty_emulator_clang_lld_link.map" "$out\map.map" -ErrorAction SilentlyContinue
}
$verb  = if ($Prof) { 'prof' } elseif ($Measure) { 'measure' } elseif ($TraceWrites) { 'tracew' } elseif ($TraceMarks) { 'tracem' } else { 'trace' }
# -TraceWrites: `tracew` (GPU write ticks too) for live-trace.py producers; -TraceMarks: `tracem`
# (a GPU timestamp after every draw and dispatch) for live-trace.py marks.
if ($ProfThread) {
	$tid = $ProfThread
	if ($tid -eq 'auto') {
		$busiest = powershell -NoProfile -File "$S\threads.ps1" -Seconds 2 -Top 40 | Select-Object -Skip 1 |
			Where-Object { $_ -match '^\s*([\d.]+)%\s+(\d+)\s*$' } | Select-Object -First 1
		if ($busiest -notmatch '^\s*([\d.]+)%\s+(\d+)\s*$') { 'no unnamed busy thread'; exit 1 }
		$tid = $Matches[2]
		"sampling thread $tid ($($Matches[1])% of a core)"
	} elseif ($tid -like 'name:*') {
		# -ProfThread name:Kyty.Record: the busiest thread with that description.
		$wanted = $tid.Substring(5)
		$named = powershell -NoProfile -File "$S\threads.ps1" -Seconds 2 -Top 200 | Select-Object -Skip 1 |
			Where-Object { $_ -match '^\s*([\d.]+)%\s+(\d+)\s+(.+?)\s*$' -and $Matches[3] -eq $wanted } | Select-Object -First 1
		if ($named -notmatch '^\s*([\d.]+)%\s+(\d+)\s+(.+?)\s*$') { "no thread named $wanted"; exit 1 }
		$tid = $Matches[2]
		"sampling thread $tid $wanted ($($Matches[1])% of a core)"
	}
	$trace = "$out\prof-t$tid.bin"; $verb = "proft $tid"
}
$seconds = [math]::Ceiling(($TurnMs + $WalkMs) / 1000.0) + 2
if ($Plan) {
	$last = ($Plan -split ',' | ForEach-Object { [int](($_ -split ':')[2]) } | Measure-Object -Maximum).Maximum
	$seconds = [math]::Ceiling($last / 1000.0) + 2
	$WalkMs = $last
}
# -Measure -PerSecond: one measure per second of the walk (labels s0, s1, ...), for counters of
# the worst seconds rather than a whole-walk average.
$command = "$verb $seconds $($trace -replace '\\', '/')"
if ($Measure -and $PerSecond) { $command = (0..($seconds - 1) | ForEach-Object { "measure 1 s$_" }) -join ' ; ' }
# -Census: time-related HLE calls and large backing reads per caller over the walk (time-census.h).
if ($Census) { $command = "timecensus $seconds" }
# -RenderCensus: render-thread time per call kind and shader over the walk (live-census.h);
# the entries go to $out\census.txt for tools\local\live-census-report.py.
if ($RenderCensus) { $command = "census $seconds" }
# -TraceDelayMs: start the capture that long after the walk starts (a `tracew` buffer holds
# about 4 s of the walk; the fixed walk-length duration still ends it after the walk).
$job = Start-Job -ScriptBlock {
	param($root, $command, $delay)
	Set-Location $root
	if ($delay -gt 0) { Start-Sleep -Milliseconds $delay }
	python tools\local\bench-windows.py live $command | Select-String 'LIVE_TRACE|LIVE_PROF|LIVE_MEASURE|LIVE_COUNTERS|LIVE_TIMECENSUS|LIVE_CENSUS' | ForEach-Object { $_.Line }
} -ArgumentList $root, $command, $TraceDelayMs
Start-Sleep -Milliseconds 800
if ($Plan) {
	$walk = Start-Process powershell -ArgumentList '-NoProfile', '-File', "$S\keys.ps1", '-Plan', $Plan -PassThru -WindowStyle Hidden -RedirectStandardOutput "$out\keys.txt"
	$TurnMs = 0; $SideMs = 0
} else {
# -TurnAtMs: the turn happens while walking, that long after the walk starts (out of the tunnel).
if ($TurnMs -gt 0 -and $TurnAtMs -le 0) { powershell -NoProfile -File "$S\key.ps1" -Key $TurnKey -HoldMs $TurnMs | Out-Null }
if ($TurnMs -gt 0 -and $TurnAtMs -gt 0) {
	Start-Process powershell -ArgumentList '-NoProfile', '-Command', "Start-Sleep -Milliseconds $TurnAtMs; & '$S\key.ps1' -Key $TurnKey -HoldMs $TurnMs" -WindowStyle Hidden | Out-Null
}
$walk = Start-Process powershell -ArgumentList '-NoProfile', '-File', "$S\key.ps1", '-Key', 'w', '-HoldMs', "$WalkMs" -PassThru -WindowStyle Hidden
}
# -SideKey d -SideMs 3000: also hold a strafe key for the first part of the walk (diagonal).
if ($SideKey -and $SideMs -gt 0) {
	Start-Sleep -Milliseconds 150
	Start-Process powershell -ArgumentList '-NoProfile', '-File', "$S\key.ps1", '-Key', $SideKey, '-HoldMs', "$SideMs" -WindowStyle Hidden | Out-Null
}
for ($t = 5; $t -le [int]($WalkMs / 1000); $t += 5) {
	Start-Sleep -Seconds 5
	powershell -NoProfile -File "$S\screen.ps1" -Out "$out\at-$t.png" -Scale 0.25 | Select-Object -Last 1
}
$walk.WaitForExit()
if (Test-Path "$out\keys.txt") { Get-Content "$out\keys.txt" | Select-Object -Last 1 }
$line = Receive-Job $job -Wait
Remove-Job $job
if ($RenderCensus) { $line | Set-Content -Path "$out\census.txt" -Encoding ascii; "census: $out\census.txt" } else { $line }
if ($line -match 'tsc_hz=(\d+)') { Set-Content -Path "$trace.hz" -Value $Matches[1] -Encoding ascii }
if (!$Prof -and !$Measure -and !$ProfThread -and !$Census -and !$RenderCensus) { python tools\local\walk-report.py $trace }
if (!$KeepRunning) { Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object { $_.Threads.Count -gt 1 } | Stop-Process -Force }
