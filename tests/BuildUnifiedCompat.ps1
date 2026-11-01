# Host-only shader/umtx regression suites. Reuses the existing emulator objects
# and dependencies; writes only under unified-compat, never replaces the emulator.
param([switch]$SkipShader, [switch]$BaselineShader, [switch]$PadOnly, [switch]$PageOnly)
$ErrorActionPreference = 'Stop'
if ($PadOnly -and $BaselineShader) { throw '-PadOnly and -BaselineShader are mutually exclusive' }
if ($PageOnly -and ($PadOnly -or $BaselineShader -or $SkipShader)) { throw '-PageOnly cannot be combined with other suite switches' }
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo '_Build/windows'
$output = Join-Path $build 'unified-compat'
New-Item -ItemType Directory -Force $output | Out-Null
$llvm = if ($env:KYTY_LLVM_BIN) { $env:KYTY_LLVM_BIN } else { Join-Path $PSScriptRoot '..\..\herramientas\llvm\bin' }
$msvc = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231'
$sdk = 'C:/Program Files (x86)/Windows Kits/10'
$env:INCLUDE = "$msvc/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um;$sdk/Include/10.0.26100.0/winrt"
$env:LIB = "$msvc/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
$manifest = [IO.File]::ReadAllText("$build/build.ninja").Replace('\','/')
function Block($target) {
    $m = [regex]::Match($manifest, '(?m)^build '+[regex]::Escape($target)+':[^\r\n]*\r?\n(?:  [^\r\n]*\r?\n)*')
    if (!$m.Success) { throw "Missing target $target" }; $m.Value
}
function Variable($b,$n) { [regex]::Match($b,'(?m)^  '+$n+' = ([^\r\n]*)').Groups[1].Value }
function Compile($source,$target) {
    $b = Block $target
    $obj = "$output/" + ($source -replace '[/\\]','_') + '.obj'
    $flags = (Variable $b 'FLAGS') -replace '-flto=thin','' -replace '-fprofile-instr-use=\S+',''
    [IO.File]::WriteAllText("$obj.rsp", "$(Variable $b 'DEFINES') $(Variable $b 'INCLUDES') $flags /c `"$repo/$source`" /Fo`"$obj`"")
    $ErrorActionPreference = 'Continue'
    & "$llvm/clang-cl.exe" "@$obj.rsp" 2>&1 | ForEach-Object { Write-Host "$_" }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw "Compile failed: $source" }
    Write-Host "Compiled $source"
    return $obj
}
$changed = @('src/libs/libKernel.cpp','src/kernel/syncOnAddress.cpp',
 'src/graphics/shader/recompiler/frontend/decode/ShaderDecoder.cpp',
 'src/graphics/shader/recompiler/frontend/decode/ScalarAluOps.cpp',
 'src/graphics/shader/recompiler/frontend/translate/Scalar.cpp',
 'src/graphics/shader/recompiler/frontend/translate/Dispatch.cpp',
 'src/graphics/shader/recompiler/backend/spirv/spirvEmitterFlow.cpp')
if ($PadOnly) { $changed = @('src/libs/controller.cpp', 'src/graphics/presentation/window/hostInput.cpp', 'src/graphics/presentation/window/window.cpp', 'src/graphics/presentation/systemOverlay.cpp') }
if ($PageOnly) { $changed = @('src/graphics/host_gpu/pageManager.cpp', 'src/graphics/presentation/window/window.cpp', 'src/graphics/host_gpu/renderer/commandScheduler.cpp') }
Push-Location $build
try {
    $compiled = @{}
    if (!$BaselineShader) {
        foreach ($source in $changed) { $compiled[$source] = Compile $source "CMakeFiles/kyty_emulator.dir/$source.obj" }
    }
    if ($PageOnly) {
        $assertObject = Compile 'src/common/assert.cpp' 'common/CMakeFiles/common.dir/assert.cpp.obj'
        $gpuTestObject = Compile 'tests/GpuDiagnosticsTests.cpp' 'CMakeFiles/kyty_emulator.dir/src/graphics/host_gpu/renderer/commandScheduler.cpp.obj'
        $b = Block 'page_manager_tests.exe'
        $gpuExe = "$output/gpu_diagnostics_tests.exe"
        [IO.File]::WriteAllText("$gpuExe.rsp", "`"$gpuTestObject`" `"$assertObject`" $(Variable $b 'LINK_LIBRARIES') $(Variable $b 'LINK_FLAGS') /out:`"$gpuExe`"")
        & "$llvm/lld-link.exe" "@$gpuExe.rsp"
        if ($LASTEXITCODE) { throw 'Link failed: gpu_diagnostics_tests' }
        & $gpuExe
        if ($LASTEXITCODE) { throw 'Test failed: gpu_diagnostics_tests' }
    }
    $targets = @('sync_on_address_tests')
    $targets += 'unified_umtx_tests'
    if (!$SkipShader) { $targets += 'shader_cfg_tests' }
    if ($BaselineShader) { $targets = @('shader_cfg_tests') }
    if ($PadOnly) { $targets = @('pad_input_tests') }
    if ($PageOnly) { $targets = @('page_manager_tests') }
    foreach ($target in $targets) {
        $linkTarget = if ($target -eq 'unified_umtx_tests') { 'shader_cfg_tests' } else { $target }
        $b = Block "$linkTarget.exe"
        $deps = (($b -split "`n")[0] -split ' \| ')[0]
        $objects = foreach ($m in [regex]::Matches($deps,'CMakeFiles/[^ ]+?\.dir/([^ ]+)\.obj')) {
            $source = $m.Groups[1].Value
            if ($compiled.ContainsKey($source)) { $compiled[$source] }
            elseif ($source.StartsWith('tests/')) {
                if ($BaselineShader) {
                    $baseline = (& git -c "safe.directory=$($repo.Replace('\','/'))" -C $repo show "7c03e6a5:$source") -join "`n"
                    [IO.File]::WriteAllText("$output/shaderCfgTests-baseline.cpp",$baseline)
                    Compile '_Build/windows/unified-compat/shaderCfgTests-baseline.cpp' $m.Value
                } elseif ($target -eq 'unified_umtx_tests') {
                    Compile 'tests/UnifiedUmtxTests.cpp' $m.Value
                } else { Compile $source $m.Value }
            }
            else {
                $obj = "CMakeFiles/kyty_emulator.dir/$source.obj"
                if (!(Test-Path $obj)) { throw "Build emulator dependencies first: $obj" }
                $obj
            }
        }
        $exe = "$output/$target.exe"
        if ($PageOnly) { $objects += $assertObject }
        if ($BaselineShader) { $exe = "$output/shader_cfg_tests-baseline.exe" }
        $rsp = ($objects | ForEach-Object { '"'+$_+'"' }) -join ' '
        $rsp += " $(Variable $b 'LINK_LIBRARIES') $(Variable $b 'LINK_FLAGS') /out:`"$exe`""
        [IO.File]::WriteAllText("$exe.rsp",$rsp)
        & "$llvm/lld-link.exe" "@$exe.rsp"
        if ($LASTEXITCODE) { throw "Link failed: $target" }
        foreach ($dll in Get-ChildItem $build -Filter '*.dll') { Copy-Item -LiteralPath $dll.FullName -Destination $output -Force }
        $ErrorActionPreference = 'Continue'
        if ($target -eq 'shader_cfg_tests' -and !$BaselineShader) { & $exe --unified-compat }
        else { & $exe }
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE) { throw "Test failed: $target ($LASTEXITCODE)" }
        Write-Host "PASS $target"
        if ($PageOnly) {
            foreach ($mode in @('normal', 'quick', 'fatal', 'page-fatal')) {
                $log = "$output/page-summary-$mode.out.log"
                # Host-only fixture, never the game executable.
                $p = Start-Process -FilePath $exe -ArgumentList @('--peak-summary', $mode) -PassThru -WindowStyle Hidden -RedirectStandardOutput $log -RedirectStandardError "$output/page-summary-$mode.err.log"
                $null = $p.Handle
                if (!$p.WaitForExit(20000)) { Stop-Process -Id $p.Id; throw "Peak summary fixture timed out: $mode" }
                $expected = if ($mode -eq 'fatal') { 321 } elseif ($mode -eq 'page-fatal') { 322 } else { 0 }
                $reason = if ($mode -eq 'fatal') { 'DbgExit' } elseif ($mode -eq 'page-fatal') { 'page-manager-fatal' } elseif ($mode -eq 'quick') { 'quick_exit' } else { 'exit' }
                if ($p.ExitCode -ne $expected -or [IO.File]::ReadAllText($log) -notmatch "summary reason=$reason max-write-count=137 page=0x0000000200010000") { throw "Bad exit summary: $mode (exit $($p.ExitCode))" }
                Write-Host "PASS exact peak on $mode exit"
            }
        }
    }
} finally { Pop-Location }
