# Compile affected translation units using the existing CMake flags, without
# running a game or replacing the emulator. Useful if portable CMake is denied.
param([switch]$HostTestsOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo '_Build/windows'
$output = Join-Path $build 'unified-host'
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
        foreach ($source in @('src/loader/redZonePatcher.cpp','src/loader/x64InstructionEmulator.cpp',
            'src/loader/runtimeLinker.cpp','src/graphics/presentation/window/window.cpp',
            'src/libs/libC.cpp','src/libs/libKernel.cpp','src/main.cpp','src/emulator.cpp',
            'src/kernel/pthread.cpp','src/kernel/memory.cpp',
            'src/graphics/host_gpu/renderer/cache/gpuResourceManager.cpp')) {
            Compile $source "CMakeFiles/kyty_emulator.dir/$source.obj" ([IO.Path]::GetFileNameWithoutExtension($source))
        }
        foreach ($name in @('emulatorConfig','hostException')) {
            Compile "src/common/$name.cpp" "common/CMakeFiles/common.dir/$name.cpp.obj" $name
        }
    }
    if (Test-Path "$repo/tests/UnifiedGuestCpuTests.cpp") {
        $template = 'CMakeFiles/kyty_emulator.dir/src/loader/redZonePatcher.cpp.obj'
        Compile 'src/loader/redZonePatcher.cpp' $template 'patcher-host' -IsTest
        Compile 'src/loader/x64InstructionEmulator.cpp' $template 'emulator-host' -IsTest
        Compile 'tests/UnifiedGuestCpuTests.cpp' $template 'tests-host' -IsTest
        $libs = @(Get-ChildItem "$build/_deps/zydis-build" -Filter '*.lib' -Recurse | Select-Object -ExpandProperty FullName)
        $fmt = Get-ChildItem "$build/3rdparty/fmt" -Filter '*.lib' -Recurse | Select-Object -First 1 -ExpandProperty FullName
        $argsText = '"'+($libs -join '" "')+'" "'+$fmt+'"'
        $argsText += " `"$output/patcher-host.obj`" `"$output/emulator-host.obj`" `"$output/tests-host.obj`" /nologo /machine:x64 /out:`"$output/unified-guest-cpu-tests.exe`""
        [IO.File]::WriteAllText("$output/link.rsp",$argsText)
        & "$llvm/lld-link.exe" "@$output/link.rsp"
        if ($LASTEXITCODE) { throw 'Host test link failed' }
        & "$output/unified-guest-cpu-tests.exe"
        if ($LASTEXITCODE) { throw 'Host tests failed' }
    }
} finally { Pop-Location }
