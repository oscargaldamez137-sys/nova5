# Static audit, not an ISA conformance test. Keeps raw candidates for review.
param([string]$ForkRef='7c03e6a5', [string]$NovaRef='nova5')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
$out=Join-Path $repo '_Build/unified-analysis/coverage'
New-Item -ItemType Directory -Force $out | Out-Null
$g=@('-c',"safe.directory=$($repo.Replace('\','/'))",'-C',$repo)
function ReadGit($ref,$path) { ((& git @g show "${ref}:$path") -join "`n") }
$files=@('ScalarAluOps.cpp','VectorAluOps.cpp','MemoryOps.cpp','ImageOps.cpp','ExportOps.cpp')
$all=@{}
foreach($ref in @($ForkRef,$NovaRef)) {
    $map=@{}
    foreach($file in $files) {
        $path="src/graphics/shader/recompiler/frontend/decode/$file"
        $t=ReadGit $ref $path
        foreach($table in [regex]::Matches($t,'(?s)constexpr\s+[\w:]+\s+(\w+)\[\]\s*=\s*\{(.*?)\n\};')) {
            foreach($e in [regex]::Matches($table.Groups[2].Value,'\{(0x[0-9a-fA-F]+)u?,\s*(?:Opcode::(\w+)|"([^"]+)")')) {
                $key=$table.Groups[1].Value+':'+$e.Groups[1].Value.ToLower()
                $name=$e.Groups[2].Value; if(!$name){$name=$e.Groups[3].Value}
                $map[$key]=[pscustomobject]@{table=$table.Groups[1].Value;encoding=$e.Groups[1].Value;opcode=$name;file=$file}
            }
        }
    }
    $all[$ref]=$map
}
$fork=$all[$ForkRef]; $nova=$all[$NovaRef]
$added=@(foreach($k in $nova.Keys | Sort-Object){ if(!$fork.ContainsKey($k)){ $nova[$k] } })
$removed=@(foreach($k in $fork.Keys | Sort-Object){ if(!$nova.ContainsKey($k)){ $fork[$k] } })
$remapped=@(foreach($k in $nova.Keys | Sort-Object){ if($fork.ContainsKey($k) -and $nova[$k].opcode -ne $fork[$k].opcode){
 [pscustomobject]@{table=$nova[$k].table;encoding=$nova[$k].encoding;fork=$fork[$k].opcode;nova=$nova[$k].opcode}
} })
$added | Export-Csv "$out/nova-extra-encodings.csv" -NoTypeInformation
$removed | Export-Csv "$out/fork-extra-encodings.csv" -NoTypeInformation
$remapped | Export-Csv "$out/remapped-encodings.csv" -NoTypeInformation
$added | Group-Object table | ForEach-Object { "Nova extra: $($_.Name) $($_.Count): $(($_.Group.opcode | Sort-Object -Unique) -join ', ')" }
"Totals: Nova extra encodings=$($added.Count); fork extra=$($removed.Count); remapped=$($remapped.Count)"
# Enumerated names are a second check for instructions decoded outside tables.
$opsets=@{}
foreach($ref in @($ForkRef,$NovaRef)) {
    $t=ReadGit $ref 'src/graphics/shader/recompiler/frontend/decode/ShaderDecoder.h'
    $body=[regex]::Match($t,'(?s)enum class Opcode[^\{]*\{(.*?)\n\};').Groups[1].Value
    $opsets[$ref]=@([regex]::Matches($body,'(?m)^\s*(\w+)\s*[,=]') | ForEach-Object {$_.Groups[1].Value})
}
$extra=@($opsets[$NovaRef] | Where-Object {$_ -notin $opsets[$ForkRef]})
$extra | Set-Content "$out/nova-extra-enums.txt"
"Nova extra enum names=$($extra.Count) (not proof of end-to-end support)"
function Functions($t) {
    $result=@{}; $counts=@{}
    # Mask literals/comments without changing character positions before balancing braces.
    $mask=[regex]::Replace($t,'(?s)/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''', {param($m) ' '*$m.Length})
    foreach($m in [regex]::Matches($mask,'(?m)^[A-Za-z_][\w:*<> \[\],&]*?\b(\w+)\([^;{}]*\)\s*(?:noexcept\s*)?\{')) {
        $name=$m.Groups[1].Value; if(!$counts.ContainsKey($name)){$counts[$name]=0}; $counts[$name]++
        $key=$name+'#'+$counts[$name]; $at=$m.Index+$m.Length; $depth=1
        while($at -lt $mask.Length -and $depth -gt 0){if($mask[$at] -eq '{'){$depth++};if($mask[$at] -eq '}'){$depth--};$at++}
        $body=$t.Substring($m.Index,$at-$m.Index)
        $result[$key]=[pscustomobject]@{name=$name;line=1+($t.Substring(0,$m.Index)-split "`n").Count-1;lines=($body-split "`n").Count;body=$body}
    }
    return $result
}
$candidates=@(); $exports=@()
foreach($file in @('src/libs/libKernel.cpp','src/kernel/pthread.cpp','src/kernel/syncOnAddress.cpp')) {
    $a=ReadGit $ForkRef $file; $b=ReadGit $NovaRef $file
    $fa=Functions $a; $fb=Functions $b
    foreach($k in $fa.Keys | Sort-Object) {
        if($fa[$k].body -match 'EXIT_NOT_IMPLEMENTED|EXIT\(' -and $fb.ContainsKey($k)) {
            $old=@([regex]::Matches($fa[$k].body,'EXIT_NOT_IMPLEMENTED\([^;]*;|EXIT\([^;]*;') | ForEach-Object {$_.Value -replace '\s+',' '})
            $new=@([regex]::Matches($fb[$k].body,'EXIT_NOT_IMPLEMENTED\([^;]*;|EXIT\([^;]*;') | ForEach-Object {$_.Value -replace '\s+',' '})
            foreach($guard in $old){if($guard -notin $new){
                $candidates += [pscustomobject]@{file=$file;function=$fa[$k].name;fork_line=$fa[$k].line;fork_lines=$fa[$k].lines;nova_line=$fb[$k].line;nova_lines=$fb[$k].lines;removed_guard=$guard}
            }}
        }
    }
    $nids=@([regex]::Matches($a,'LIB_FUNC\("([^"]+)",\s*([^\)]+)\)') | ForEach-Object {$_.Groups[1].Value})
    foreach($m in [regex]::Matches($b,'LIB_FUNC\("([^"]+)",\s*([^\)]+)\)')) {
        if($m.Groups[1].Value -notin $nids){$exports += [pscustomobject]@{file=$file;nid=$m.Groups[1].Value;function=$m.Groups[2].Value}}
    }
}
$candidates | Export-Csv "$out/removed-fatal-guards.csv" -NoTypeInformation
$exports | Export-Csv "$out/nova-extra-exports.csv" -NoTypeInformation
$candidates | Format-Table function,fork_lines,nova_lines,removed_guard -Wrap
$exports | Format-Table function,nid
& git @g diff --numstat $ForkRef $NovaRef -- src/graphics/shader/recompiler/frontend src/libs/libKernel.cpp src/kernel/pthread.cpp src/kernel/syncOnAddress.cpp src/kernel/syncOnAddress.h | Set-Content "$out/source-numstat.tsv"
