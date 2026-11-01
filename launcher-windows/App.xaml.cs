using System;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows;

namespace KytyLauncher {

// Command line (a double-click passes none):
//   --play                 the game started with the saved settings, without this window (Steam's launch options, a
//                          shortcut); this process lasts until the game's console closes. No game folder: the window.
// For tests:
//   --dry-run <file>       the play, precompile and hints commands the current settings make, written to <file>
//   --screenshot <folder>  each page rendered to <folder>\<page>.png, then exit
//   --lang en|zh           the window's language for this run (the saved choice otherwise)
//   --no-art               (with --screenshot) no game art in the banner
//   --demo-path <text>     (with --screenshot) the game path box shows <text> (pictures for the release notes)
public partial class App : Application {
	protected override void OnStartup(StartupEventArgs e) {
		base.OnStartup(e);
		string dryRun = null, screenshots = null, lang = null;
		for (int i = 0; i + 1 < e.Args.Length; i++) {
			if (e.Args[i] == "--dry-run") dryRun = e.Args[++i];
			else if (e.Args[i] == "--screenshot") screenshots = e.Args[++i];
			else if (e.Args[i] == "--lang") lang = e.Args[++i];
			else if (e.Args[i] == "--demo-path") KytyLauncher.MainWindow.ScreenshotPath = e.Args[++i];
		}
		KytyLauncher.MainWindow.ScreenshotNoArt = Array.IndexOf(e.Args, "--no-art") >= 0;
		var context = new LauncherContext();
		context.Settings.ReadOnly = dryRun != null || screenshots != null;
		if (lang != null) Strings.Language = lang == "zh" ? UiLanguage.Chinese : UiLanguage.English;
		if (dryRun != null) {
			var text = new StringBuilder();
			var commands = new Commands(context);
			text.AppendLine("play:       cmd " + commands.PlayArguments());
			text.AppendLine("precompile: cmd " + commands.PrecompileArguments(""));
			text.AppendLine("hints:      cmd " + commands.PrecompileArguments("-ExportHints"));
			text.AppendLine(string.Format("efficiency cores: 0x{0:X}", context.EfficiencyMask));
			text.AppendLine(string.Format("display: {0} lines", context.DisplayHeight));
			text.AppendLine("frame generation: " + (context.FrameGenAvailable ? "NVIDIA's Streamline runtime in " + context.Streamline
				: !context.RtxGpu ? "no NVIDIA RTX graphics card (DLSS off)" : "no NVIDIA-signed Streamline runtime (settings: '" + context.Settings.Str("streamline") + "')"));
			File.WriteAllText(dryRun, text.ToString(), new UTF8Encoding(false));
			Shutdown(0);
			return;
		}
		if (Array.IndexOf(e.Args, "--play") >= 0) {
			var game = GameInfo.Read(context.Settings.Str("game"), context.PrecompileTool, context.Root);
			if (game.Found) {
				var commands = new Commands(context);
				// The window that had the focus as this started (Steam's Big Picture when Steam starts it): the emulator
				// hands the focus back to it as the game quits (issue #217: Windows gave it to another window, and Big
				// Picture took no pad input until clicked).
				var focus = Native.ForegroundWindow();
				var parent = Native.ParentProcess();
				Environment.SetEnvironmentVariable("KYTY_RETURN_FOCUS", focus);
				Environment.SetEnvironmentVariable("KYTY_RETURN_FOCUS_PID", parent);
				try {
					var process = commands.Start(commands.PlayArguments(game, true), true);
					// Steam counts the game as running while this process lasts (its controller layout, play time).
					new Thread(() => {
						process.WaitForExit();
						// (Again, where the game ended without handing it back.)
						Native.Focus(focus, parent);
						var code = process.ExitCode;
						Dispatcher.BeginInvoke(new Action(() => Shutdown(code)));
					}) { IsBackground = true }.Start();
				} catch (Exception error) {
					MessageBox.Show(Strings.F("error.start", error.Message), Strings.T("error.title"), MessageBoxButton.OK, MessageBoxImage.Error);
					Shutdown(1);
				}
				return;
			}
		}
		var window = new MainWindow(context);
		base.MainWindow = window;
		if (screenshots != null) {
			window.SaveScreenshots(screenshots);
			Shutdown(0);
			return;
		}
		window.Show();
	}
}

}
