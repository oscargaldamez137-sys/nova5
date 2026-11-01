# Compile affected translation units using the existing CMake flags, without
# running a game or replacing the emulator. Useful if portable CMake is denied.
param([switch]$HostTestsOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo '_Build/windows'
$output = Join-Path $build 'jetsku-draw-ports'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$llvm = if ($env:KYTY_LLVM_BIN) { $env:KYTY_LLVM_BIN } else { Join-Path $PSScriptRoot '..\..\herramientas\llvm\bin' }
$msvc = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231'
$sdk = 'C:/Program Files (x86)/Windows Kits/10'
$env:INCLUDE = "$msvc/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um;$sdk/Include/10.0.26100.0/winrt"
$env:LIB = "$msvc/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
$manifest = [IO.File]::ReadAllText((Join-Path $build 'build.ninja')).Replace('\','/')
function Block($target) {
    $m = [regex]::Match($manifest, '(?m)^build ' + [regex]::Escape($target) + ':[^\r\n]*\r?\n(?:  [^\r\n]*\r?\n)*')
    if (!$m.Success) { throw "Missing target $target" }
    return $m.Value
}
function Variable($block, $name) {
    [regex]::Match($block, '(?m)^  '+$name+' = ([^\r\n]*)').Groups[1].Value
}
function Compile($source, $target, $name, [switch]$IsTest) {
    $block = Block $target
    $flags = Variable $block 'FLAGS'
    if ($IsTest) { $flags = $flags -replace '-flto=thin','' -replace '-fprofile-instr-use=\S+','' }
    $rsp = Join-Path $output "$name.rsp"
    $obj = Join-Path $output "$name.obj"
    [IO.File]::WriteAllText($rsp, "$(Variable $block 'DEFINES') $(Variable $block 'INCLUDES') $flags /c `"$repo/$source`" /Fo`"$obj`"")
    $ErrorActionPreference = 'Continue'
    & "$llvm/clang-cl.exe" "@$rsp" 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw "Compile failed: $source" }
    Write-Output "Compiled $source"
}
Push-Location $build
try {
    if (!$HostTestsOnly) {
        foreach ($source in @('src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp',
          'src/graphics/host_gpu/renderer/renderDraw.cpp', 'src/graphics/guest_gpu/graphicsRun.cpp',
          'src/graphics/host_gpu/renderer/cache/bufferCache.cpp')) {
            Compile $source "CMakeFiles/kyty_emulator.dir/$source.obj" ([IO.Path]::GetFileNameWithoutExtension($source))
        }
    }
    Compile 'tests/JetskuDrawPortsTests.cpp' 'CMakeFiles/kyty_emulator.dir/src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp.obj' 'tests-host' -IsTest
    & "$llvm/lld-link.exe" "$output/tests-host.obj" /nologo /machine:x64 "/out:$output/jetsku-draw-ports-tests.exe" kernel32.lib
    if ($LASTEXITCODE) { throw 'Host test link failed' }
    & "$output/jetsku-draw-ports-tests.exe"
    if ($LASTEXITCODE) { throw 'Host tests failed' }
    $testObjects = @()
    foreach ($source in @('tests/ResourceMaterializationTests.cpp',
        'src/graphics/shader/recompiler/ir/BdaReadPlan.cpp', 'src/graphics/guest_gpu/gpu_format.cpp',
        'src/graphics/host_gpu/hostMemory.cpp', 'src/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp',
        'src/graphics/shader/recompiler/ir/passes/SrtWalker.cpp', 'src/local/jit-arena.cpp')) {
        $name = 'materialize-' + [IO.Path]::GetFileNameWithoutExtension($source)
        Compile $source "CMakeFiles/resource_materialization_tests.dir/$source.obj" $name -IsTest
        $testObjects += "$output/$name.obj"
    }
    & "$llvm/lld-link.exe" @testObjects /nologo /machine:x64 "/out:$output/resource-materialization-tests.exe" "3rdparty/fmt/fmt.lib" kernel32.lib
    if ($LASTEXITCODE) { throw 'Materialization test link failed' }
    # Negative test cases intentionally print errors; PowerShell 5 treats
    # redirected native stderr as an error record. Judge this harness by its exit code.
    $ErrorActionPreference = 'Continue'
    & "$output/resource-materialization-tests.exe"
    $testExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($testExit) { throw "Materialization tests failed ($testExit)" }
} finally { Pop-Location }
