param([double]$Seconds = 5, [int]$Top = 25, [string]$Out = '')
# CPU use per emulator thread over an interval, as % of one core, with the thread's description
# (SetThreadDescription) where it has one. Run it while the scene of interest plays.
Add-Type @"
using System; using System.Runtime.InteropServices;
public class TD {
  [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
  [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr p);
  public static string Name(uint id) {
    IntPtr h = OpenThread(0x0800, false, id); // THREAD_QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) return "";
    IntPtr p; string s = "";
    if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) { s = Marshal.PtrToStringUni(p); LocalFree(p); }
    CloseHandle(h); return s;
  }
}
"@
$p = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object { $_.Threads.Count -gt 1 } | Select-Object -First 1
if (!$p) { 'emulator not running'; exit 1 }
function Snap { $h = @{}; foreach ($t in (Get-Process -Id $p.Id).Threads) { try { $h[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }; $h }
$a = Snap; $clock = [Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Milliseconds ([int]($Seconds * 1000))
$b = Snap; $elapsed = $clock.Elapsed.TotalMilliseconds
$rows = foreach ($id in $b.Keys) {
	$before = if ($a.ContainsKey($id)) { $a[$id] } else { 0 }
	[pscustomobject]@{ Tid = $id; Cpu = [math]::Round(100 * ($b[$id] - $before) / $elapsed, 1); Name = [TD]::Name([uint32]$id) }
}
$total = ($rows | Measure-Object Cpu -Sum).Sum
$text = @("threads over {0:N1} s: total {1:N0}% of one core" -f ($elapsed / 1000), $total) +
	($rows | Sort-Object Cpu -Descending | Select-Object -First $Top | ForEach-Object { "{0,6:N1}%  {1,6}  {2}" -f $_.Cpu, $_.Tid, $_.Name })
if ($Out) { $text | Set-Content $Out }
$text
