namespace Nova5.Services;

// Nova5 switches for adaptations of chenxiao07/KytyPS5 (GPL-2.0):
// 8791b592/455f8d6b/079dc8f9/b9dbda43, a7d46e82, 400d414b, 2ac66300.
public static class DemonsSoulsOptions
{
    public static readonly (string Key, string Variable)[] Switches =
    [
        ("ds_idle_enabled", "KYTY_DS_IDLE"),
        ("ds_spin_backoff_enabled", "KYTY_DS_SPIN_BACKOFF"),
        ("ds_disable_index_culling", "KYTY_DS_DISABLE_INDEX_CULLING"),
        ("ds_affinity_enabled", "KYTY_DS_AFFINITY"),
        ("ds_render_priority_enabled", "KYTY_DS_RENDER_PRIORITY")
    ];

    // On by default for Demon's Souls; an explicit INI false or KYTY_DS_*=0 turns one off.
    public static bool Effective(string? setting, string? inherited) => setting is null
        ? inherited is null || inherited == "1"
        : setting.Trim().Equals("true", StringComparison.OrdinalIgnoreCase) || setting.Trim() == "1";

    public static void Apply(string titleId, IDictionary<string, string?> environment, Func<string, string?> setting)
    {
        foreach (var (key, variable) in Switches)
        {
            if (titleId != "PPSA01341") { environment[variable] = "0"; continue; }
            var value = setting(key);
            // Explicit Nova5.ini settings win, including false over an inherited environment.
            // With no INI entry, an external A/B runner can set KYTY_DS_* independently.
            // The emulator additionally checks the exact game version and hook signatures.
            if (value is not null)
                environment[variable] = Effective(value, null) ? "1" : "0";
            else if (!environment.ContainsKey(variable))
                environment[variable] = "1";
        }
    }
}
