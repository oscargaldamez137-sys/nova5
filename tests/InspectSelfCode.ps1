# Read-only extraction of an uncompressed SELF code range for LLVM disassembly.
param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][uint32]$Start,
      [int]$Length = 512, [Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
$data = [IO.File]::ReadAllBytes($Source)
if ([BitConverter]::ToUInt32($data,0) -ne 0x1d3d154f) { throw 'Not a SELF' }
$count = [BitConverter]::ToUInt16($data,24)
$elf = 32 + $count * 32
if ([BitConverter]::ToUInt32($data,$elf) -ne 0x464c457f) { throw 'Missing ELF header' }
$phoff = [BitConverter]::ToUInt64($data,$elf+32)
$phsize = [BitConverter]::ToUInt16($data,$elf+54)
$phcount = [BitConverter]::ToUInt16($data,$elf+56)
if ($phsize -ne 56) { throw 'Unexpected program header size' }
for ($i=0; $i -lt $count; $i++) {
    $entry = 32 + $i*32
    $kind = [BitConverter]::ToUInt64($data,$entry)
    if (!($kind -band 0x800)) { continue }
    $idx = ($kind -shr 20) -band 0xfff
    if ($idx -ge $phcount) { throw 'Invalid segment index' }
    $ph = $elf + $phoff + $idx*56
    $va = [BitConverter]::ToUInt64($data,$ph+16)
    $fs = [BitConverter]::ToUInt64($data,$ph+32)
    if ([BitConverter]::ToUInt32($data,$ph) -ne 1 -or $Start -lt $va -or ([uint64]$Start+$Length) -gt ($va+$fs)) { continue }
    if ([BitConverter]::ToUInt64($data,$entry+16) -ne $fs -or [BitConverter]::ToUInt64($data,$entry+24) -ne $fs) { throw 'Compressed/encrypted segment unsupported' }
    $pos = [BitConverter]::ToUInt64($data,$entry+8) + $Start - $va
    $bytes = New-Object byte[] (60+$Length)
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes,0)
    [BitConverter]::GetBytes([uint16]1).CopyTo($bytes,2)
    [Text.Encoding]::ASCII.GetBytes('.text').CopyTo($bytes,20)
    [BitConverter]::GetBytes($Start).CopyTo($bytes,32)
    [BitConverter]::GetBytes([uint32]$Length).CopyTo($bytes,36)
    [BitConverter]::GetBytes([uint32]60).CopyTo($bytes,40)
    [BitConverter]::GetBytes([uint32]0x60500020).CopyTo($bytes,56)
    [Array]::Copy($data,[long]$pos,$bytes,60,$Length)
    [IO.File]::WriteAllBytes($Output,$bytes)
    & (Join-Path $(if ($env:KYTY_LLVM_BIN) { $env:KYTY_LLVM_BIN } else { Join-Path $PSScriptRoot '..\..\herramientas\llvm\bin' }) 'llvm-objdump.exe') -d --x86-asm-syntax=intel $Output
    if ($LASTEXITCODE) { throw 'Disassembly failed' }
    return
}
throw 'Code range not found'
