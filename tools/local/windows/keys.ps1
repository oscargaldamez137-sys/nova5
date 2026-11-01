param([string]$Plan = 'w:down:0,h:down:4000,h:up:4500,w:up:25000')
# A timed key sequence from one process (separate key.ps1 processes start with a variable delay,
# which moved the walk's turn by up to a second): "key:down|up:milliseconds,...", times from the
# first event. Focuses the emulator window first, like key.ps1.
$p = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if (!$p) { "emulator not running"; exit 1 }
Add-Type @"
using System; using System.Runtime.InteropServices;
public class KS {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("imm32.dll")] public static extern IntPtr ImmGetDefaultIMEWnd(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  public static void Send(ushort vk, bool up, bool ext) {
    var i = new INPUT[1]; i[0].type = 1; i[0].ki.wScan = (ushort)MapVirtualKey(vk, 0);
    i[0].ki.dwFlags = 8u | (up ? 2u : 0u) | (ext ? 1u : 0u);
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
}
"@
if ([KS]::IsIconic($p.MainWindowHandle)) { [KS]::ShowWindow($p.MainWindowHandle, 9) | Out-Null }
if ([KS]::GetForegroundWindow() -ne $p.MainWindowHandle) {
	[KS]::keybd_event(0x12, 0, 0, [IntPtr]::Zero); [KS]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
	[KS]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
	Start-Sleep -Milliseconds 300
}
if ([KS]::GetForegroundWindow() -ne $p.MainWindowHandle) { "emulator window is not in the foreground; no key sent"; exit 1 }
$ime = [KS]::ImmGetDefaultIMEWnd($p.MainWindowHandle)
if ($ime -ne [IntPtr]::Zero) { [KS]::SendMessage($ime, 0x283, [IntPtr]6, [IntPtr]::Zero) | Out-Null }
$vk = @{ up = 0x26; down = 0x28; left = 0x25; right = 0x27; enter = 0x0D; esc = 0x1B; space = 0x20 }
$events = foreach ($item in $Plan -split ',') {
	$key, $action, $at = $item -split ':'
	$code = if ($vk.ContainsKey($key)) { $vk[$key] } else { [int][char]$key.ToUpper() }
	[pscustomobject]@{ Code = [uint16]$code; Up = ($action -eq 'up'); At = [int]$at; Ext = ($key -in @('up', 'down', 'left', 'right')) }
}
# Another window taking the foreground mid-plan (a notification, another app) sends the keys
# elsewhere and releases the held ones in the game, and the walk leaves its route (a missed turn
# walks into a wall). Before each event the emulator is focused again once, pressing the held keys
# again; focus is never fought for in between (someone may be using the desktop). The count of
# lost focus marks a walk whose route cannot be trusted.
$held = @{}
$lost = 0
function Keep-Focus {
	if ([KS]::GetForegroundWindow() -eq $p.MainWindowHandle) { return $true }
	$script:lost++
	[KS]::keybd_event(0x12, 0, 0, [IntPtr]::Zero); [KS]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
	[KS]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
	Start-Sleep -Milliseconds 50
	# Keys sent while another window is in front would type into it (a chat box, an editor).
	if ([KS]::GetForegroundWindow() -ne $p.MainWindowHandle) { return $false }
	foreach ($h in $held.Values) { [KS]::Send($h.Code, $false, $h.Ext) }
	return $true
}
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$away = $false
foreach ($e in ($events | Sort-Object At)) {
	while ($clock.ElapsedMilliseconds -lt $e.At) {
		if ([KS]::GetForegroundWindow() -ne $p.MainWindowHandle) { $away = $true }
		Start-Sleep -Milliseconds 5
	}
	if (!(Keep-Focus)) { "emulator window lost the foreground; plan stopped"; exit 1 }
	[KS]::Send($e.Code, $e.Up, $e.Ext)
	if ($e.Up) { $held.Remove($e.Code) } else { $held[$e.Code] = $e }
}
$note = if ($away -or $lost) { " (focus lost: route not trusted)" } else { "" }
"plan done in $($clock.ElapsedMilliseconds) ms$note"
