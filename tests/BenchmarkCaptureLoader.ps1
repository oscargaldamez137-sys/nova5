# Synthetic decoder benchmark only; never starts an emulator or game.
$ErrorActionPreference = 'Stop'
$dir = Join-Path (Split-Path $PSScriptRoot) '_Build/windows/unified-host'
$testExe = "$dir/unified-guest-cpu-tests.exe"
$observer = "$dir/guest-crash-capture.exe"
if (!(Test-Path $testExe) -or !(Test-Path $observer)) { throw 'Run BuildUnifiedHost.ps1 and BuildCrashCapture.ps1 first' }
$oldHeap = [Environment]::GetEnvironmentVariable('_NO_DEBUG_HEAP','Process')
try {
    foreach ($mode in @('direct','debug-heap','capture-default')) {
        [Environment]::SetEnvironmentVariable('_NO_DEBUG_HEAP',$(if ($mode -eq 'debug-heap') {'0'} else {$null}),'Process')
        $out = "$dir/patch-benchmark-$mode.out.log"
        $exe = $testExe
        $arguments = '--patch-benchmark'
        if ($mode -ne 'direct') {
            $exe = $observer
            $arguments = "`"$dir/patch-benchmark-$mode.crash.log`" `"$out`" 0 0 `"$testExe`" --patch-benchmark"
        }
        $p = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $out
        $null = $p.Handle
        if (!$p.WaitForExit(50000)) { Stop-Process -Id $p.Id; throw "Benchmark timeout: $mode" }
        if ($p.ExitCode) { throw "Benchmark failed: $mode, exit $($p.ExitCode)" }
        $result = Get-Content -LiteralPath $out | Where-Object { $_ -match '^\[PATCH_BENCHMARK\]' }
        if (!$result) { throw "Missing result: $mode" }
        Write-Output "$mode $result"
    }
} finally { [Environment]::SetEnvironmentVariable('_NO_DEBUG_HEAP',$oldHeap,'Process') }
