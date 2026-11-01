param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$output = "$repo/_Build/windows/unified-host"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$llvm = if ($env:KYTY_LLVM_BIN) { $env:KYTY_LLVM_BIN } else { Join-Path $PSScriptRoot '..\..\herramientas\llvm\bin' }
$msvc = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231'
$sdk = 'C:/Program Files (x86)/Windows Kits/10'
$env:INCLUDE = "$msvc/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um"
$env:LIB = "$msvc/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
foreach ($item in @(@('tools/guest-crash-capture.cpp','guest-crash-capture'),@('tests/CrashCaptureChild.cpp','crash-capture-child'))) {
    & "$llvm/clang-cl.exe" /nologo /std:c++20 /EHsc /W4 /Od /Zi /MD /D_CRT_SECURE_NO_WARNINGS "$repo/$($item[0])" "/Fo$output/$($item[1]).obj" "/Fe$output/$($item[1]).exe" -fuse-ld=lld /link "/pdb:$output/$($item[1]).pdb"
    if ($LASTEXITCODE) { throw "Compile failed: $($item[0])" }
}
if ($SkipTests) { return }
foreach ($mode in @('clean','handled','null','bad-stack')) {
    $log = "$output/capture-test-$mode.log"
    $child = "$output/crash-capture-child.exe"
    $modules = "$output/capture-test-$mode.out.log"
    $argsText = "`"$log`" `"$modules`" 0 0 `"$child`" $mode `"space argument`" trailing\ `"embedded\`"quote`""
    $p = Start-Process -FilePath "$output/guest-crash-capture.exe" -ArgumentList $argsText -PassThru -WindowStyle Hidden -RedirectStandardOutput $modules
    $null = $p.Handle
    if (!$p.WaitForExit(20000)) { Stop-Process -Id $p.Id; throw "Capture fixture timed out: $mode (pid $($p.Id))" }
    $text = Get-Content -LiteralPath $log -Raw
    if ($mode -eq 'clean') {
        if ($p.ExitCode -ne 0 -or $text -match 'chance=') { throw 'Clean/quoting fixture failed' }
    } elseif ($mode -eq 'handled') {
        if ($p.ExitCode -ne 0 -or $text -notmatch 'chance=first' -or $text -match 'chance=second') { throw 'Observer changed handled exception delivery' }
        if ($text -notmatch 'second=0 av=1 illegal=0 guard=0') { throw 'Handled exception counters are incorrect' }
    } else {
        if ($p.ExitCode -ne -1073741819 -or $text -notmatch 'chance=second' -or $text -notmatch 'rip=' -or $text -notmatch 'instruction bytes') { throw "Exception fixture failed: $mode, code $($p.ExitCode)" }
        if ($mode -eq 'null' -and $text -notmatch 'chance=first') { throw 'Missing first-chance null capture' }
        if ($text -notmatch 'pc=fixture.bin\+0x' -or $text -match 'name=old-fixture') { throw 'Module map load/unload/reuse failed' }
        if ($text -notmatch 'second=1 av=2 illegal=0 guard=0') { throw 'Fatal exception counters are incorrect' }
    }
    Write-Output "PASS capture $mode (exit $($p.ExitCode))"
}
