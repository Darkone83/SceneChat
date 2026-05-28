// Creds.cs -- Credential persistence (JSON, same data as Python client creds.json)

using System.IO;
using System.Text.Json;

namespace SceneChatWPF.Net;

public record SavedCreds(string Server, string Username, string Password);

public static class Creds
{
    private static readonly string Path = System.IO.Path.Combine(
        AppDomain.CurrentDomain.BaseDirectory, "creds.json");

    public static SavedCreds? Load()
    {
        try
        {
            if (!File.Exists(Path)) return null;
            return JsonSerializer.Deserialize<SavedCreds>(File.ReadAllText(Path));
        }
        catch { return null; }
    }

    public static void Save(string server, string username, string password)
    {
        try
        {
            var json = JsonSerializer.Serialize(new SavedCreds(server, username, password),
                new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(Path, json);
        }
        catch { }
    }

    public static void Clear()
    {
        try { if (File.Exists(Path)) File.Delete(Path); } catch { }
    }
}
