using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Nova5.Services;

// One patch of a game: Nova5's own label, explanation and the modes that turn it on come from the optional
// "nova5" object of the mod (the emulator ignores it).
public sealed record GamePatch(int Index, string Name, string Label, string Detail, bool Enabled, IReadOnlyList<string> Modes);

// A game's GoldHEN-style mods file, _Patches/<title id>.json, which the emulator applies at launch (only the
// mods with "enabled": true, and only for the exact game version). Nova5 ships tuned files in assets/patches and
// copies one in the first time its game is opened; the player's choices are saved back into the same file.
public sealed class GamePatches
{
	public static readonly string[] ModeNames = { "calidad", "equilibrado", "rendimiento" };

	private const string RemoteIndex = "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/json.txt";
	private const string RemoteFiles = "https://raw.githubusercontent.com/TeeKay87/HEN-Cheats-Collection/master/cheats/json/";

	private readonly string _path;
	private readonly JsonObject _root;

	private GamePatches(string path, JsonObject root)
	{
		_path = path;
		_root = root;
	}

	public string Version => _root["version"]?.GetValue<string>() ?? "";

	public List<GamePatch> Mods => (_root["mods"] as JsonArray ?? new JsonArray()).Select((node, index) =>
	{
		var mod   = node as JsonObject ?? new JsonObject();
		var name  = mod["name"]?.GetValue<string>() ?? "";
		var extra = mod["nova5"] as JsonObject;
		var modes = (extra?["modes"] as JsonArray)?.Select(m => m?.GetValue<string>() ?? "").ToList() ?? new List<string>();
		return new GamePatch(index, name, extra?["label"]?.GetValue<string>() ?? name, extra?["detail"]?.GetValue<string>() ?? "",
		    mod["enabled"]?.GetValue<bool>() ?? true, modes);
	}).ToList();

	// Modes exist only when the file says which patches each one uses.
	public bool HasModes => Mods.Any(m => m.Modes.Count > 0);

	// The mode whose patches are exactly the enabled ones, or "personalizado".
	public string Mode
	{
		get
		{
			var mods = Mods.Where(m => m.Modes.Count > 0).ToList();
			return ModeNames.FirstOrDefault(mode => mods.All(m => m.Enabled == m.Modes.Contains(mode))) ?? "personalizado";
		}
	}

	public static string Folder(string baseDir) => Path.Combine(baseDir, "_Patches");

	// The game's file, seeded from Nova5's bundled one for that version when the player has none yet.
	public static GamePatches? Load(string baseDir, Game game)
	{
		var path = Path.Combine(Folder(baseDir), game.TitleId + ".json");
		var bundled = Path.Combine(baseDir, "assets", "patches", game.TitleId + ".json");
		if (!File.Exists(path) && File.Exists(bundled) && Read(bundled) is { } seed && seed["version"]?.GetValue<string>() == game.Version)
		{
			Directory.CreateDirectory(Folder(baseDir));
			File.Copy(bundled, path);
		}
		if (!File.Exists(path) || Read(path) is not { } root) return null;
		// Labels and modes of a newer Nova5 reach a file the player already has; the on/off choices stay.
		if (File.Exists(bundled) && Read(bundled)?["mods"] is JsonArray shipped && root["mods"] is JsonArray mods)
		{
			var extras = shipped.OfType<JsonObject>().Where(m => m["nova5"] is not null)
			    .ToDictionary(m => m["name"]?.GetValue<string>() ?? "", m => m["nova5"]!);
			var changed = false;
			foreach (var mod in mods.OfType<JsonObject>())
			{
				if (!extras.TryGetValue(mod["name"]?.GetValue<string>() ?? "", out var extra) ||
				    JsonNode.DeepEquals(mod["nova5"], extra)) continue;
				mod["nova5"] = extra.DeepClone();
				changed = true;
			}
			if (changed) File.WriteAllText(path, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
		}
		return new GamePatches(path, root);
	}

	private static JsonObject? Read(string path)
	{
		try
		{
			return JsonNode.Parse(File.ReadAllText(path)) as JsonObject;
		}
		catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException)
		{
			return null;
		}
	}

	public void Set(int index, bool enabled)
	{
		if ((_root["mods"] as JsonArray)?[index] is JsonObject mod) mod["enabled"] = enabled;
		Save();
	}

	public void ApplyMode(string mode)
	{
		foreach (var patch in Mods.Where(m => m.Modes.Count > 0)) Set(patch.Index, patch.Modes.Contains(mode));
	}

	private void Save() =>
	    File.WriteAllText(_path, _root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));

	// Adds the community patches for this exact game version (TeeKay87/HEN-Cheats-Collection, the same source as
	// KytyPS5's launcher), switched off, to the game's file; creates the file when there is none. Returns how many
	// patch files the collection has for it and how many new patches arrived; throws on network errors.
	public static async Task<(int Found, int Added)> DownloadAsync(string baseDir, Game game, CancellationToken cancel)
	{
		using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
		http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("Nova5", Updates.Current.ToString()));
		var index = await http.GetStringAsync(RemoteIndex, cancel);
		var files = index.Split('\n')
		    .Select(line => line.Split('=', 2)[0].Trim())
		    .Where(name => name.StartsWith($"{game.TitleId}_{game.Version}_", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".json"))
		    .ToList();

		var path = Path.Combine(Folder(baseDir), game.TitleId + ".json");
		var root = File.Exists(path) ? Read(path) : null;
		var added = 0;
		foreach (var file in files)
		{
			if (JsonNode.Parse(await http.GetStringAsync(RemoteFiles + Uri.EscapeDataString(file), cancel)) is not JsonObject remote ||
			    remote["version"]?.GetValue<string>() != game.Version || remote["mods"] is not JsonArray remoteMods)
				continue;
			if (root is null)
			{
				root = new JsonObject
				{
					["id"] = game.TitleId, ["name"] = remote["name"]?.GetValue<string>() ?? game.Name, ["version"] = game.Version,
					["process"] = remote["process"]?.GetValue<string>() ?? "eboot.bin", ["mods"] = new JsonArray(),
				};
			}
			var mods  = (JsonArray)root["mods"]!;
			var names = mods.Select(m => m?["name"]?.GetValue<string>()).ToHashSet();
			foreach (var node in remoteMods.ToList())
			{
				if (node is not JsonObject mod || names.Contains(mod["name"]?.GetValue<string>())) continue;
				var copy = (JsonObject)mod.DeepClone();
				copy["enabled"] = false;
				mods.Add(copy);
				added++;
			}
		}
		if (root is not null && added > 0)
		{
			Directory.CreateDirectory(Folder(baseDir));
			File.WriteAllText(path, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
		}
		return (files.Count, added);
	}
}
