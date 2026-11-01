using System.Runtime.InteropServices;

namespace Nova5.Services;

// The performance cores of a hybrid CPU (Intel P-cores), for the emulator's recording and render threads
// (KYTY_RECORDING_CPUS / KYTY_RENDER_CPUS), as run-windows.ps1 picks them: the CPUs of the highest efficiency
// class, without the core of CPU 0 (it takes most interrupts on Windows). Empty on a CPU of one kind of core,
// which leaves the threads to Windows.
public static class CpuSets
{
	[DllImport("kernel32.dll", SetLastError = true)]
	private static extern bool GetSystemCpuSetInformation(IntPtr information, int bufferLength, out int returnedLength,
	                                                      IntPtr process, int flags);

	public static string PerformanceCpus()
	{
		if (!OperatingSystem.IsWindows()) return "";
		var sets = new List<(int Cpu, int Core, int Class)>();
		GetSystemCpuSetInformation(IntPtr.Zero, 0, out var length, IntPtr.Zero, 0);
		if (length == 0) return "";
		var buffer = Marshal.AllocHGlobal(length);
		try
		{
			if (!GetSystemCpuSetInformation(buffer, length, out length, IntPtr.Zero, 0)) return "";
			for (var offset = 0; offset < length; offset += Marshal.ReadInt32(buffer, offset))
			{
				// SYSTEM_CPU_SET_INFORMATION: Size, Type (0 = CpuSetInformation), Id, Group (+12), LogicalProcessorIndex
				// (+14), CoreIndex (+15), ..., EfficiencyClass (+18).
				if (Marshal.ReadInt32(buffer, offset + 4) != 0 || Marshal.ReadInt16(buffer, offset + 12) != 0) continue;
				sets.Add((Marshal.ReadByte(buffer, offset + 14), Marshal.ReadByte(buffer, offset + 15),
				          Marshal.ReadByte(buffer, offset + 18)));
			}
		}
		finally
		{
			Marshal.FreeHGlobal(buffer);
		}
		if (sets.Select(s => s.Class).Distinct().Count() < 2) return "";
		var fastest    = sets.Max(s => s.Class);
		var interrupts = sets.Where(s => s.Cpu == 0).Select(s => s.Core).DefaultIfEmpty(-1).First();
		return string.Join(',', sets.Where(s => s.Class == fastest && s.Core != interrupts && s.Cpu < 64).Select(s => s.Cpu));
	}
}
