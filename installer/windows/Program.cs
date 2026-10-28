using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace SeparateSongSetup;

static class Program
{
    // package-windows.ps1 builds with the version in mod.json.
    internal static readonly string ReleaseVersion = typeof(Program).Assembly.GetName().Version!.ToString(3);
    internal static readonly string ProductName = "OBS Jukebox " + ReleaseVersion;
    [STAThread]
    static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        try
        {
            if(args.Length>0 && args[0]=="--plugin-helper")return Engine.PluginHelper(args);
            if(args.Contains("--update"))return UpdateMode.Entry(args);
            var options = Options.Parse(args);
            if (options.Silent)
            {
                Engine.Execute(options, Console.WriteLine);
                return 0;
            }
            Application.Run(new SetupForm(options));
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex.Message);
            if (!args.Contains("--silent") && !args.Contains("--update")) MessageBox.Show(ex.Message, "OBS Jukebox Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }
}

sealed class Options
{
    public string GD = DetectGD();
    public string OBS = DetectOBS();
    public string PluginRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "obs-studio", "plugins");
    public string StateRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OBS Jukebox", "Installations");
    public string SceneRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "obs-studio", "basic", "scenes");
    public bool Silent, DryRun, CloseApps, Restart, Uninstall, InstallJukebox, CreateScene, ModOnly, Update;
    public string? ResultPath, ReopenFlag;
    internal byte[]? DownloadedJukebox;
    internal string? PreviousVersion;
    internal bool AlreadyCurrent;
    internal bool ScenesFound;
    public string? Manifest;
    public static Options Parse(string[] args)
    {
        var o = new Options();
        bool obsGiven=false;
        for (int i = 0; i < args.Length; i++)
        {
            string Next() => ++i < args.Length ? args[i] : throw new ArgumentException("Missing value for " + args[i-1]);
            switch (args[i])
            {
                case "--gd": o.GD = Next(); break;
                case "--obs": o.OBS = Next(); obsGiven = true; break;
                case "--plugin-root": o.PluginRoot = Next(); break;
                case "--state-root": o.StateRoot = Next(); break;
                case "--scene-root": o.SceneRoot = Next(); break;
                case "--manifest": o.Manifest = Next(); break;
                case "--silent": o.Silent = true; break;
                case "--dry-run": o.DryRun = true; break;
                case "--close-apps": o.CloseApps = true; break;
                case "--restart": o.Restart = true; break;
                case "--install-jukebox": o.InstallJukebox = true; break;
                case "--mod-only": o.ModOnly = true; break;
                case "--integrate": o.CreateScene = true; break;
                case "--no-scene": case "--no-integrate": o.CreateScene = false; break;
                case "--uninstall": o.Uninstall = true; break;
                case "--update": o.Update = true; break;
                case "--result": o.ResultPath = Next(); break;
                case "--reopen-flag": o.ReopenFlag = Next(); break;
                default: throw new ArgumentException("Unknown option: " + args[i]);
            }
        }
        if (o.Update)
        {
            // An update never closes or reopens apps by itself and never edits scenes.
            o.Silent = true; o.CloseApps = false; o.Restart = false; o.CreateScene = false;
            // OBS is usually closed when an update starts, so process detection would find nothing.
            if (!obsGiven && Engine.RecordedOBS(Path.GetFullPath(o.StateRoot)) is string recorded) o.OBS = recorded;
        }
        o.GD = NormalizeGD(o.GD); o.OBS = NormalizeOBS(o.OBS);
        o.PluginRoot = Path.GetFullPath(o.PluginRoot); o.StateRoot = Path.GetFullPath(o.StateRoot); o.SceneRoot = Path.GetFullPath(o.SceneRoot);
        return o;
    }
    public static string NormalizeGD(string p) => Path.GetFullPath(File.Exists(p) ? Path.GetDirectoryName(p)! : p);
    public static string NormalizeOBS(string p)
    {
        p = Path.GetFullPath(p);
        if (p.EndsWith("obs64.exe", StringComparison.OrdinalIgnoreCase)) return Path.GetFullPath(Path.Combine(Path.GetDirectoryName(p)!, "..", ".."));
        if (p.EndsWith(Path.Combine("bin", "64bit"), StringComparison.OrdinalIgnoreCase)) return Path.GetFullPath(Path.Combine(p, "..", ".."));
        return p;
    }
    static string DetectGD()
    {
        foreach (var p in Process.GetProcessesByName("GeometryDash"))
            using(p) try { if (Engine.ProcessPath(p) is string file) return Path.GetDirectoryName(file)!; } catch { }
        var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        string steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", "")?.ToString() ?? "";
        if (steam.Length != 0) roots.Add(steam);
        roots.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam"));
        foreach (var root in roots.ToArray())
        {
            string vdf = Path.Combine(root, "steamapps", "libraryfolders.vdf");
            if (File.Exists(vdf)) foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\"")) roots.Add(m.Groups[1].Value.Replace("\\\\", "\\"));
        }
        foreach (var root in roots)
        {
            var candidate = Path.Combine(root, "steamapps", "common", "Geometry Dash");
            if (File.Exists(Path.Combine(candidate, "GeometryDash.exe"))) return candidate;
        }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam", "steamapps", "common", "Geometry Dash");
    }
    static string DetectOBS()
    {
        foreach (var p in Process.GetProcessesByName("obs64"))
            using(p) try { if (Engine.ProcessPath(p) is string file) return NormalizeOBS(file); } catch { }
        var registry = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio", "InstallLocation", "")?.ToString();
        return !string.IsNullOrWhiteSpace(registry) ? registry : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "obs-studio");
    }
}

sealed record PlannedFile(string Destination, byte[] Data, string Reason);
sealed class ChangedFile
{
    public string Destination { get; set; } = "";
    public string InstalledHash { get; set; } = "";
    public string? Backup { get; set; }
    public string? OriginalHash { get; set; }
    public string Reason { get; set; } = "";
    public byte[]? InstalledScene { get; set; }
    public bool Applied { get; set; }
}
sealed class InstallRecord
{
    public string Product { get; set; } = Program.ProductName;
    public string GD { get; set; } = "";
    public string OBS { get; set; } = "";
    public string Status { get; set; } = "installing";
    public List<ChangedFile> Files { get; set; } = [];
}

// An app the install must change is running. Update mode waits and tries again.
sealed class AppRunningException(string message) : IOException(message);
// Another setup holds the install lock. Update mode waits and tries again.
sealed class SetupBusyException(string message) : IOException(message);
// A newer OBS Jukebox was installed by hand while an update waited. The update stops without a result.
sealed class NewerVersionInstalledException() : IOException("A newer OBS Jukebox is already installed.");

static class Engine
{
    // Manifests name the release that wrote them, and an install by any release can be undone.
    static bool IsKnownProduct(string? product) =>
        product?.StartsWith("OBS Jukebox ", StringComparison.Ordinal) == true || product == "Separate Song 1.0.0";
    static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
    public static string Hash(byte[] data) => Convert.ToHexString(SHA256.HashData(data));
    static string HashFile(string path) { using var s = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(s)); }
    static Dictionary<string, byte[]> Payload()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip") ?? throw new IOException("This setup has no release payload. Build it with scripts/package-windows.ps1.");
        using var zip = new ZipArchive(stream);
        var files = new Dictionary<string, byte[]>();
        foreach (var e in zip.Entries.Where(e => !e.FullName.EndsWith('/')))
        {
            using var data = new MemoryStream(); using var input = e.Open(); input.CopyTo(data); files.Add(e.FullName.Replace('\\','/'), data.ToArray());
        }
        return files;
    }
    static string PackageVersion(byte[] bytes, string expectedId)
    {
        using var memory = new MemoryStream(bytes); using var zip = new ZipArchive(memory);
        var e = zip.GetEntry("mod.json") ?? throw new IOException("Missing mod.json in " + expectedId);
        using var input = e.Open(); var j = JsonNode.Parse(input)!;
        if (j["id"]?.ToString() != expectedId) throw new IOException("Unexpected package id for " + expectedId);
        if (zip.GetEntry(expectedId + ".dll") == null) throw new IOException(expectedId + " has no Windows DLL.");
        return j["version"]!.ToString().TrimStart('v');
    }
    internal const string JukeboxUrl="https://github.com/Fleeym/jukebox/releases/download/v3.8.0/fleym.nongd.geode";
    internal const string JukeboxHash="A0ECA6C82C7FA6149B956A809A9058959957D7C4672368B6933EA030E641B621";
    // Every package in the mods folder whose mod.json has this ID. Geode loads only one package per
    // ID, and which one depends on the file names, so callers refuse more than one.
    static List<(string Path,JsonNode Metadata,bool HasDll)> FindPackages(string gd,string id)
    {
        var found=new List<(string,JsonNode,bool)>();
        string mods=Path.Combine(gd,"geode","mods");
        if(!Directory.Exists(mods)) return found;
        foreach(string file in Directory.GetFiles(mods,"*.geode"))
        {
            try
            {
                using var zip=ZipFile.OpenRead(file); var metadata=zip.GetEntry("mod.json"); if(metadata==null)continue;
                using var input=metadata.Open(); var json=JsonNode.Parse(input);
                if(json?["id"]?.ToString()!=id)continue;
                found.Add((file,json,zip.GetEntry(id+".dll")!=null));
            }
            catch(InvalidDataException) { }
            catch(JsonException) { }
        }
        return found;
    }
    static IOException Duplicates(string name,List<(string Path,JsonNode Metadata,bool HasDll)> found) =>
        new("More than one "+name+" package is installed ("+string.Join(", ",found.Select(f=>Path.GetFileName(f.Path)))+"). Remove the extra copies from geode/mods, then retry.");
    internal static (string Path,string Version)? FindJukebox(string gd)
    {
        var found=FindPackages(gd,"fleym.nongd");
        if(found.Count>1) throw Duplicates("Jukebox",found);
        if(found.Count==0) return null;
        var (path,json,hasDll)=found[0];
        return hasDll ? (path,json["version"]?.ToString().TrimStart('v')??"unknown version") : (path,"without Windows support");
    }
    internal static (string Path,string? Version)? FindInstalledMod(string gd)
    {
        var found=FindPackages(gd,"local.separate_song");
        if(found.Count>1) throw Duplicates("OBS Jukebox",found);
        if(found.Count==0) return null;
        return (found[0].Path,found[0].Metadata["version"]?.ToString().TrimStart('v'));
    }
    static byte[] DownloadJukebox(Action<string> log)
    {
        log("Downloading official Jukebox 3.8.0...");
        try
        {
            using var client=new System.Net.Http.HttpClient { Timeout=TimeSpan.FromSeconds(60) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("OBS-Jukebox-Setup/" + Program.ReleaseVersion);
            byte[] bytes=client.GetByteArrayAsync(JukeboxUrl).GetAwaiter().GetResult();
            if(Hash(bytes)!=JukeboxHash)throw new IOException("Jukebox download verification failed. No downloaded files were installed.");
            if(PackageVersion(bytes,"fleym.nongd")!="3.8.0")throw new IOException("Jukebox download has unexpected metadata.");
            log("Verified official Jukebox 3.8.0 SHA256="+JukeboxHash); return bytes;
        }
        catch(System.Net.Http.HttpRequestException ex) { throw new IOException("Could not download Jukebox. Check your connection or install Jukebox 3.8.0 from Geode, then retry.",ex); }
        catch(TaskCanceledException ex) { throw new IOException("Jukebox download timed out. Install Jukebox 3.8.0 from Geode or retry.",ex); }
    }
    // True only when installed is a release version above release; anything unparsable isn't newer.
    internal static bool IsNewer(string? installed,string release) =>
        Release(installed) is { } a && Release(release) is { } b && a.CompareTo(b)>0;
    // Exactly three integers, like version_newer in Install.command; "1.2", "1.2.3.4" and prereleases don't parse.
    static (int,int,int)? Release(string? text)
    {
        var parts=text?.TrimStart('v').Split('.');
        if (parts is not { Length: 3 }) return null;
        var n=new int[3];
        for (int i=0;i<3;i++) if (!int.TryParse(parts[i],System.Globalization.NumberStyles.None,null,out n[i])) return null;
        return (n[0],n[1],n[2]);
    }
    // The same logger, minus its failures: losing a log line is better than stopping a restore halfway.
    internal static Action<string> QuietLog(Action<string> log) => line =>
    {
        try { log(line); }
        catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) { }
    };
    // A restore that can't finish marks the error, so update mode reports "repair" whatever went wrong first.
    internal static void RollBack(InstallRecord record,Exception error,Action<string> log)
    {
        try { Restore(record,log); }
        // Marked before logging, so a failing log write can't lose the mark.
        catch(Exception restoreError) { error.Data["repair"]=true; QuietLog(log)("Rollback incomplete: "+restoreError.Message); }
    }
    // The OBS folder of the last install, while it still holds OBS.
    internal static string? RecordedOBS(string stateRoot)
    {
        try
        {
            string manifest=File.ReadAllText(Path.Combine(stateRoot,"latest-manifest.txt")).Trim();
            var record=JsonSerializer.Deserialize<InstallRecord>(File.ReadAllText(manifest));
            return record?.OBS is { Length: > 0 } obs && File.Exists(Path.Combine(obs,"bin","64bit","obs64.exe")) ? obs : null;
        }
        catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException) { return null; }
    }
    internal static bool CanUndo(Options o)
    {
        try
        {
            string path=o.Manifest??File.ReadAllText(Path.Combine(o.StateRoot,"latest-manifest.txt")).Trim();
            var record=JsonSerializer.Deserialize<InstallRecord>(File.ReadAllText(path));
            return record is { Status: "installed" } && IsKnownProduct(record.Product);
        }
        catch(IOException) { return false; }
        catch(UnauthorizedAccessException) { return false; }
        catch(JsonException) { return false; }
    }
    internal static string? InstalledVersion(string gd) => FindInstalledMod(gd)?.Version;
    internal static string ObsSetupMessage(Options o) =>
        (o.CreateScene && o.ScenesFound ? "OBS: GD Sounds is in your existing scenes." : "OBS: add Sources > GD Sounds once in OBS.")+
        " In GD Jukebox, use the Game and OBS checkboxes. Keep Audio Monitoring off. Existing OBS collections and unrelated mods are preserved.";
    internal static string SuccessMessage(Options o)
    {
        string result=o.AlreadyCurrent ? "OBS Jukebox " + Program.ReleaseVersion + " is already up to date." :
            o.PreviousVersion!=null ? "Successfully updated to " + Program.ReleaseVersion + "." :
            "Successfully installed OBS Jukebox " + Program.ReleaseVersion + ".";
        return result+(o.ModOnly ? " OBS was kept running. Run full setup after your stream to apply OBS plugin fixes." : " You may close setup.");
    }
    public static List<PlannedFile> Plan(Options o, Action<string> log)
    {
        if (!File.Exists(Path.Combine(o.GD, "GeometryDash.exe"))) throw new IOException("Choose the Geometry Dash folder containing GeometryDash.exe.");
        if (!File.Exists(Path.Combine(o.OBS, "bin", "64bit", "obs64.exe"))) throw new IOException("Choose the OBS Studio folder containing bin/64bit/obs64.exe. Install 64-bit OBS Studio first.");
        var payload = Payload(); var result = new List<PlannedFile>();
        void Add(string dest, byte[] data, string reason)
        {
            if (File.Exists(dest) && HashFile(dest) == Hash(data)) { log("Keep unchanged: " + dest); return; }
            result.Add(new(dest, data, reason));
        }
        string loader = Path.Combine(o.GD, "Geode.dll");
        Version? version = null;
        if (File.Exists(loader)) Version.TryParse((FileVersionInfo.GetVersionInfo(loader).FileVersion ?? "").Replace(",", ".").Replace(" ", ""), out version);
        if (File.Exists(loader) && version == null) throw new IOException("Cannot determine installed Geode version. Use the official Geode installer to update to 5.10.1, then retry.");
        if (version?.Major >= 6) throw new IOException("This build requires Geode 5.x. Geode " + version + " needs a compatible OBS Jukebox build.");
        if (version == null || version < new Version(5,10,1))
        {
            if (!File.Exists(loader) && File.Exists(Path.Combine(o.GD, "XInput1_4.dll"))) throw new IOException("An existing XInput1_4.dll belongs to another loader. Install Geode with its official installer before retrying; this setup will not overwrite an unknown loader.");
            if (!payload.ContainsKey("geode/Geode.dll") || !payload.Keys.Any(k => k.StartsWith("geode/geode/resources/geode.loader/"))) throw new IOException("This setup has no complete Geode payload; install Geode 5.10.1 first.");
            foreach (var file in payload.Where(p => p.Key.StartsWith("geode/"))) Add(Path.Combine(o.GD, file.Key[6..].Replace('/',Path.DirectorySeparatorChar)), file.Value, "Official Geode 5.10.1 " + (version == null ? "install" : "upgrade"));
        }
        else log("Keep compatible Geode " + version);
        var jukebox=FindJukebox(o.GD);
        if(jukebox is { Version: "3.8.0" }) log("Keep installed Jukebox 3.8.0: "+jukebox.Value.Path);
        else if(jukebox!=null) throw new IOException("Jukebox "+jukebox.Value.Version+" is installed. Install Jukebox 3.8.0 from Geode before continuing.");
        else if(!o.InstallJukebox) throw new IOException("Jukebox 3.8.0 is required. Install it from Geode, or select Download and install Jukebox, then retry.");
        else if(o.DryRun) log("Would download official Jukebox 3.8.0: "+JukeboxUrl+" SHA256="+JukeboxHash);
        else
        {
            o.DownloadedJukebox??=DownloadJukebox(log);
            Add(Path.Combine(o.GD,"geode","mods","fleym.nongd.geode"),o.DownloadedJukebox,"Jukebox 3.8.0 (requested download)");
        }
        byte[] Required(string key) => payload.TryGetValue(key,out var data) ? data : throw new IOException("This setup is incomplete: missing " + key + ". Download a complete release installer and retry.");
        byte[] mod=Required("mods/local.separate_song.geode");
        if(PackageVersion(mod,"local.separate_song")!=Program.ReleaseVersion) throw new IOException("Unexpected OBS Jukebox payload version.");
        // Update the installed package where it is, so a renamed copy isn't left beside the new one.
        string modDestination=FindInstalledMod(o.GD)?.Path ?? Path.Combine(o.GD,"geode","mods","local.separate_song.geode");
        Add(modDestination,mod,"OBS Jukebox mod " + Program.ReleaseVersion);
        bool portable = File.Exists(Path.Combine(o.OBS,"portable_mode.txt")) || File.Exists(Path.Combine(o.OBS,"portable_mode"));
        string plugin = portable ? Path.Combine(o.OBS,"obs-plugins","64bit","separate-song.dll") : Path.Combine(o.PluginRoot,"separate-song","bin","64bit","separate-song.dll");
        if (o.ModOnly)
        {
            if (!File.Exists(plugin)) throw new IOException("Mod-only updates require an existing OBS Jukebox plugin. Run a full installation first.");
            log("Mod-only update: OBS stays running. The installed OBS plugin and scenes are kept. Run full setup after your stream to apply OBS plugin fixes.");
        }
        else Add(plugin,Required("obs/separate-song.dll"),"Native OBS source GD Sounds");
        if (o.CreateScene && !o.ModOnly)
        {
            string sceneRoot = portable ? Path.Combine(o.OBS,"config","obs-studio","basic","scenes") : o.SceneRoot;
            o.ScenesFound = Directory.Exists(sceneRoot);
            if (!o.ScenesFound) log("No existing OBS collections found. In OBS add Sources > GD Sounds once; setup creates no new collection.");
            else foreach (var file in Directory.GetFiles(sceneRoot,"*.json"))
            {
                byte[] original=File.ReadAllBytes(file);
                byte[] integrated=IntegrateCollection(original,out bool changed);
                if(changed) Add(file,integrated,"Add shared GD Sounds to existing scenes");
                else log("Keep already integrated OBS collection: "+file);
            }
        }
        return result;
    }
    internal static byte[] IntegrateCollection(byte[] original,out bool changed)
    {
        var collection=JsonNode.Parse(original)?.AsObject()??throw new IOException("Invalid OBS scene collection JSON.");
        var sources=collection["sources"] as JsonArray??throw new IOException("Missing OBS scene source array.");
        bool modified=false;
        void Set(JsonObject obj,string key,JsonNode? value) { if(!JsonNode.DeepEquals(obj[key],value)) { obj[key]=value; modified=true; } }
        foreach(var (type,defaultName) in new[]{("gd_alternate_song","GD Sounds")})
        {
            var existing=sources.OfType<JsonObject>().FirstOrDefault(s=>s["id"]?.ToString()==type || s["versioned_id"]?.ToString()==type);
            if(existing==null)
            {
                string name=defaultName;
                var names=sources.OfType<JsonObject>().Select(s=>s["name"]?.ToString()).ToHashSet(StringComparer.Ordinal);
                if(names.Contains(name))
                {
                    string fallback=defaultName+" (OBS Jukebox)";
                    name=fallback;
                    for(int suffix=2;names.Contains(name);suffix++) name=fallback+" "+suffix;
                }
                existing=new JsonObject { ["name"]=name,["uuid"]=Guid.NewGuid().ToString(),["id"]=type,["versioned_id"]=type,["settings"]=new JsonObject(),["mixers"]=255,["sync"]=0,["flags"]=0,["volume"]=1.0,["balance"]=0.5,["enabled"]=true,["muted"]=false,["monitoring_type"]=0,["hotkeys"]=new JsonObject(),["private_settings"]=new JsonObject() };
                sources.Add(existing); modified=true;
            }
            string oldName=existing["name"]?.ToString()??defaultName;
            if(string.IsNullOrWhiteSpace(existing["name"]?.ToString())) Set(existing,"name",JsonValue.Create(oldName));
            if(type=="gd_alternate_song" && (oldName=="GD Alternate Song" || oldName=="Custom Song" || oldName=="OBS Custom Song") && !sources.OfType<JsonObject>().Any(s=>s!=existing && s["name"]?.ToString()=="GD Sounds")) Set(existing,"name",JsonValue.Create("GD Sounds"));
            Set(existing,"monitoring_type",JsonValue.Create(0));
            string sourceName=existing["name"]!.ToString(); string uuid=existing["uuid"]?.ToString()??Guid.NewGuid().ToString(); Set(existing,"uuid",JsonValue.Create(uuid));
            foreach(var scene in sources.OfType<JsonObject>().Where(s=>s["id"]?.ToString()=="scene"))
            {
                var settings=scene["settings"] as JsonObject??throw new IOException("Missing scene settings.");
                var items=settings["items"] as JsonArray??new JsonArray(); if(settings["items"]==null)settings["items"]=items;
                var item=items.OfType<JsonObject>().FirstOrDefault(i=>i["source_uuid"]?.ToString()==uuid)
                    ??items.OfType<JsonObject>().FirstOrDefault(i=>string.IsNullOrEmpty(i["source_uuid"]?.ToString()) && i["name"]?.ToString()==oldName);
                if(item!=null){Set(item,"name",JsonValue.Create(sourceName));Set(item,"source_uuid",JsonValue.Create(uuid));continue;}
                long maximum=items.OfType<JsonObject>().Select(i=>SceneNumber(i["id"])).Append(SceneNumber(settings["id_counter"])).Max();
                if(maximum==long.MaxValue) throw new IOException("OBS scene item IDs are exhausted. Recreate this collection in OBS before installing.");
                long next=maximum+1;
                items.Add(new JsonObject { ["name"]=sourceName,["source_uuid"]=uuid,["id"]=next,["visible"]=true,["locked"]=true,["rot"]=0.0,["pos"]=new JsonObject{["x"]=0.0,["y"]=0.0},["scale"]=new JsonObject{["x"]=1.0,["y"]=1.0},["align"]=5,["bounds_type"]=0,["bounds_align"]=0,["bounds"]=new JsonObject{["x"]=0.0,["y"]=0.0},["crop_left"]=0,["crop_top"]=0,["crop_right"]=0,["crop_bottom"]=0,["private_settings"]=new JsonObject() });
                settings["id_counter"]=Math.Max(SceneNumber(settings["id_counter"]),next); modified=true;
            }
        }
        changed=modified; return modified?JsonSerializer.SerializeToUtf8Bytes(collection,JsonOptions):original;
    }
    static string MachinePlugin => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),"obs-studio","plugins","separate-song","bin","64bit","separate-song.dll");
    static bool IsMachinePlugin(string path)=>string.Equals(Path.GetFullPath(path),MachinePlugin,StringComparison.OrdinalIgnoreCase);
    // The privileged process has one fixed write target. It never parses user
    // manifests, edits scenes, closes apps, restarts apps, or writes user backups.
    internal static int PluginHelper(string[] args)
    {
        if(args.Length<3 || args[1] is not ("write" or "remove"))throw new IOException("Invalid plugin helper request.");
        string destination=MachinePlugin;
        void RefuseLinks()
        {
            for(string? path=destination;path!=null;path=Path.GetDirectoryName(path))
                if((File.Exists(path)||Directory.Exists(path)) && (File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)throw new IOException("Plugin installation path contains a link or junction: "+path);
        }
        RefuseLinks();
        if(args[1]=="remove")
        {
            if(args.Length!=3)throw new IOException("Invalid plugin removal request.");
            if(File.Exists(destination)) { if(HashFile(destination)!=args[2])throw new IOException("Plugin changed since uninstall began; it was preserved."); File.Delete(destination); }
        }
        else
        {
            if(args.Length!=4)throw new IOException("Invalid plugin copy request.");
            using var input=new FileStream(args[2],FileMode.Open,FileAccess.Read,FileShare.Read);
            if(input.Length>128*1024*1024)throw new IOException("Plugin is too large.");
            using var buffer=new MemoryStream();input.CopyTo(buffer);byte[] bytes=buffer.ToArray();
            if(Hash(bytes)!=args[3])throw new IOException("Plugin copy verification failed.");
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            // Folders created above didn't exist at the first check, and a junction could appear meanwhile.
            RefuseLinks();
            string temporary=destination+"."+Guid.NewGuid().ToString("N")+".tmp";
            try { File.WriteAllBytes(temporary,bytes);File.Move(temporary,destination,true); }
            finally { if(File.Exists(temporary))File.Delete(temporary); }
        }
        return 0;
    }
    static void RunPluginHelper(params string[] arguments)
    {
        var start=new ProcessStartInfo(Environment.ProcessPath!){UseShellExecute=true,Verb="runas"};
        start.ArgumentList.Add("--plugin-helper");foreach(var argument in arguments)start.ArgumentList.Add(argument);
        using var child=Process.Start(start)??throw new IOException("Could not start the OBS plugin copy helper.");child.WaitForExit();
        if(child.ExitCode!=0)throw new IOException("OBS plugin copy was not completed. Check folder permissions and retry.");
    }
    static void PrivilegedPluginWrite(byte[] data)
    {
        string source=Path.Combine(Path.GetTempPath(),"obs-jukebox-"+Guid.NewGuid().ToString("N")+".dll");
        try { File.WriteAllBytes(source,data);RunPluginHelper("write",source,Hash(data)); }
        finally { if(File.Exists(source))File.Delete(source); }
    }
    internal static long SceneNumber(JsonNode? node)
    {
        if(node is not JsonValue value)return 0;
        if(value.TryGetValue<long>(out var integer))return Math.Max(0,integer);
        if(value.TryGetValue<double>(out var number) && double.IsFinite(number) && number>=0 && number<long.MaxValue && Math.Truncate(number)==number)return (long)number;
        return 0;
    }
    // Reverse only fields and objects changed by this transaction. Matching by
    // stable IDs tolerates OBS formatting changes and reordered source arrays.
    internal static byte[] UndoCollection(byte[] original,byte[] installed,byte[] current)
    {
        var originalTree=JsonNode.Parse(original)!;var installedTree=JsonNode.Parse(installed)!;var currentTree=JsonNode.Parse(current)!;
        var retainedSources=new HashSet<string>(StringComparer.Ordinal);
        var installedSources=installedTree["sources"]!.AsArray();
        foreach(var scene in currentTree["sources"]!.AsArray().OfType<JsonObject>().Where(s=>s["id"]?.ToString()=="scene"))
        {
            var oldScene=installedSources.OfType<JsonObject>().FirstOrDefault(s=>s["id"]?.ToString()=="scene" && (scene["uuid"]!=null ? s["uuid"]?.ToString()==scene["uuid"]?.ToString() : s["name"]?.ToString()==scene["name"]?.ToString()));
            var oldItems=oldScene?["settings"]?["items"] as JsonArray;
            foreach(var item in (scene["settings"]?["items"] as JsonArray??new JsonArray()).OfType<JsonObject>())
                if(item["source_uuid"]?.ToString() is string uuid && (oldItems==null || !oldItems.OfType<JsonObject>().Any(old=>old["source_uuid"]?.ToString()==uuid && old["id"]?.ToString()==item["id"]?.ToString())))retainedSources.Add(uuid);
        }
        JsonNode? Undo(JsonNode? before,JsonNode? ours,JsonNode? now)
        {
            if(JsonNode.DeepEquals(before,ours))return now?.DeepClone();
            if(JsonNode.DeepEquals(ours,now))return before?.DeepClone();
            if(before is JsonObject beforeObject && ours is JsonObject afterObject && now is JsonObject currentObject)
            {
                var result=(JsonObject)currentObject.DeepClone();
                foreach(var key in beforeObject.Select(x=>x.Key).Union(afterObject.Select(x=>x.Key)))
                {
                    if(JsonNode.DeepEquals(beforeObject[key],afterObject[key]))continue;
                    var value=Undo(beforeObject[key],afterObject[key],currentObject[key]);
                    if(value==null && !beforeObject.ContainsKey(key))result.Remove(key);else result[key]=value;
                }
                return result;
            }
            if(before is JsonArray ba && ours is JsonArray aa && now is JsonArray ca)
            {
                string Key(JsonNode? n)
                {
                    if(n is not JsonObject o)return n?.ToJsonString()??"null";
                    string? id=o["id"]?.ToString();
                    if(o["id"] is JsonValue value && value.TryGetValue<long>(out var number))return "item:"+number;
                    if(id!=null && ba.Count(x=>x?["id"]?.ToString()==id)==1 && aa.Count(x=>x?["id"]?.ToString()==id)==1 && ba.Any(x=>x?["id"]?.ToString()==id && x?["uuid"]==null))return "type:"+id;
                    return o["uuid"]?.ToString() is string u ? "uuid:"+u : "name:"+o["name"];
                }
                var result=new JsonArray();
                foreach(var item in ca)
                {
                    string key=Key(item);var a=aa.FirstOrDefault(x=>Key(x)==key);var b=ba.FirstOrDefault(x=>Key(x)==key);
                    if(a!=null && b==null)
                    {
                        if(item?["uuid"]?.ToString() is string uuid && retainedSources.Contains(uuid))result.Add(item.DeepClone());
                        continue;
                    }
                    result.Add(a==null?item?.DeepClone():Undo(b,a,item));
                }
                foreach(var b in ba.Where(x=>!aa.Any(a=>Key(a)==Key(x)) && !ca.Any(c=>Key(c)==Key(x))))result.Add(b?.DeepClone());
                return result;
            }
            return now?.DeepClone();
        }
        return JsonSerializer.SerializeToUtf8Bytes(Undo(originalTree,installedTree,currentTree),JsonOptions);
    }
    [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,int id);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder path,ref int length);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    internal static string? ProcessPath(Process process)
    {
        var handle=OpenProcess(0x1000,false,process.Id);
        if(handle==IntPtr.Zero)return null;
        try { var path=new StringBuilder(32768);int length=path.Capacity;return QueryFullProcessImageName(handle,0,path,ref length)?path.ToString():null; }
        finally { CloseHandle(handle); }
    }
    internal static List<(string Name,string Exe)> AppsForChanges(Options o,IEnumerable<string> destinations)
    {
        string gdRoot=Path.GetFullPath(o.GD).TrimEnd(Path.DirectorySeparatorChar,Path.AltDirectorySeparatorChar)+Path.DirectorySeparatorChar;
        bool gd=false,obs=false;
        foreach(var path in destinations)
        {
            if(Path.GetFullPath(path).StartsWith(gdRoot,StringComparison.OrdinalIgnoreCase)) gd=true;
            else obs=true;
        }
        var apps=new List<(string,string)>();
        if(gd) apps.Add(("GeometryDash",Path.Combine(o.GD,"GeometryDash.exe")));
        if(obs) apps.Add(("obs64",Path.Combine(o.OBS,"bin","64bit","obs64.exe")));
        return apps;
    }
    static void CloseMatchingApps(Options o,Action<string> log,List<string> stopped,IEnumerable<string> destinations)
    {
        foreach (var (name, exe) in AppsForChanges(o,destinations))
        foreach (var p in Process.GetProcessesByName(name))
        {
            using var process=p;
            string? actual=ProcessPath(p);
            if(actual==null)throw new AppRunningException("Cannot inspect running " + name + ". Close it manually before installing.");
            // An update waits for every copy of the app, wherever it runs from.
            if (!o.Update && !string.Equals(actual,exe,StringComparison.OrdinalIgnoreCase)) continue;
            if (!o.CloseApps) throw new AppRunningException(name + " is running. Close it first or choose Ask apps to close. OBS can ask you to stop recording/replay; this setup never force-kills it.");
            log("Requesting normal close: " + actual);
            if (!p.CloseMainWindow()) throw new IOException(name + " has no visible main window. If OBS is minimized to the system tray, open its tray menu and choose Exit, then retry. No process was force-killed.");
            if (!p.WaitForExit(20000)) throw new IOException(name + " is still running. Resolve its confirmation or close it manually, then retry. No process was force-killed.");
            stopped.Add(exe);
        }
    }
    // A setup killed mid-transaction leaves its session at "installing", with backups that neither
    // undo nor a new install reach. Each run restores such sessions before doing anything else.
    internal static int RecoverInterrupted(Options o,Action<string> log,List<string> stopped)
    {
        // Recovery may have to close OBS, which a mod-only update promises not to do.
        if(o.ModOnly || !Directory.Exists(o.StateRoot)) return 0;
        int incomplete=0;
        foreach(var session in Directory.GetDirectories(o.StateRoot))
        {
            string manifest=Path.Combine(session,"manifest.json");
            if(!File.Exists(manifest)) continue;
            InstallRecord? record;
            try { record=JsonSerializer.Deserialize<InstallRecord>(File.ReadAllText(manifest)); }
            catch(Exception ex) when(ex is JsonException or IOException or UnauthorizedAccessException) { continue; }
            if(record is not { Status: "installing" } || !IsKnownProduct(record.Product)) continue;
            log("Restoring an installation that was interrupted: "+manifest);
            // A session left at "installing" would be retried, and fail, on every later run.
            try
            {
                string gd=o.GD,obs=o.OBS;
                try { o.GD=record.GD; o.OBS=record.OBS; CloseMatchingApps(o,log,stopped,record.Files.Where(Replaced).Select(f=>f.Destination)); }
                finally { o.GD=gd; o.OBS=obs; }
                Restore(record,log);
                record.Status="rolled-back";
            }
            // Nothing was restored yet, so the session stays at "installing" for the next run.
            catch(AppRunningException) { throw; }
            catch(Exception ex) { log("Rollback incomplete: "+ex.Message); record.Status="rollback-incomplete"; incomplete++; }
            // A journal that can't be written counts as unfinished, instead of escaping as a plain error.
            try { WriteManifest(manifest,record); }
            catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) { log("Could not record the recovery: "+ex.Message); incomplete++; }
            // Update mode stops at the first unfinished session. Continuing could hide this one behind an
            // app that opens for a later session, and the retry would skip a session already marked.
            if (incomplete>0 && o.Update) break;
        }
        return incomplete;
    }
    // Through a temporary file, so a setup killed mid-write leaves the previous manifest, not half of one.
    static void WriteManifest(string path,InstallRecord record)
    {
        File.WriteAllText(path+".tmp",JsonSerializer.Serialize(record,JsonOptions));
        File.Move(path+".tmp",path,true);
    }
    internal const string SetupLockName=@"Local\OBS Jukebox Setup";
    // One setup at a time: recovery treats every session left at "installing" as abandoned, so it would
    // roll back a second window's live transaction.
    public static void Execute(Options o,Action<string> output,List<string>? closedApps=null)
    {
        using var running=new Mutex(false,SetupLockName);
        try { if(!running.WaitOne(0)) throw new SetupBusyException("Another OBS Jukebox setup is running. Let it finish, then try again."); }
        catch(AbandonedMutexException) { } // a killed setup held it; recovery handles what it left
        try { ExecuteAlone(o,output,closedApps); }
        finally { running.ReleaseMutex(); }
    }
    static void ExecuteAlone(Options o,Action<string> output,List<string>? closedApps)
    {
        var logLines = new List<string>(); string? logPath=null;
        void Log(string s) { string line=DateTimeOffset.Now.ToString("O") + " " + s; logLines.Add(line); output(s); if(logPath!=null) File.AppendAllText(logPath,line+Environment.NewLine); }
        var stopped=closedApps??new List<string>();
        if (o.Update && IsNewer(InstalledVersion(o.GD),Program.ReleaseVersion)) throw new NewerVersionInstalledException();
        if (!o.DryRun && RecoverInterrupted(o,Log,stopped)>0 && o.Update)
        {
            var unfinished=new IOException("An earlier interrupted install couldn't be fully undone.");
            unfinished.Data["repair"]=true; throw unfinished;
        }
        if (o.Uninstall) { Uninstall(o,Log,stopped); if(closedApps==null && o.Restart) RestartApps(stopped,Log); return; }
        o.PreviousVersion=InstalledVersion(o.GD); o.AlreadyCurrent=false;
        var plan=Plan(o,Log);
        foreach(var f in plan) Log($"{f.Reason}: {f.Destination} SHA256={Hash(f.Data)} ({f.Data.Length:N0} bytes)");
        if(o.DryRun) { Log("Dry run complete. No destination files changed and no apps were closed."); return; }
        if(plan.Count==0) { o.AlreadyCurrent=true; Log(SuccessMessage(o)); Log("No files changed; existing restore manifest preserved."); return; }
        CloseMatchingApps(o,Log,stopped,plan.Select(f=>f.Destination));
        plan=Plan(o,Log);
        string session=Path.Combine(o.StateRoot,DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N")[..6]);
        Directory.CreateDirectory(session); logPath=Path.Combine(session,"install.log"); File.WriteAllLines(logPath,logLines);
        var record=new InstallRecord{GD=o.GD,OBS=o.OBS}; string manifest=Path.Combine(session,"manifest.json");
        void Save() => WriteManifest(manifest,record);
        Save();
        try
        {
            foreach(var f in plan)
            {
                var change=new ChangedFile{Destination=f.Destination,InstalledHash=Hash(f.Data),Reason=f.Reason,InstalledScene=f.Reason=="Add shared GD Sounds to existing scenes"?f.Data:null};
                if(File.Exists(f.Destination))
                {
                    change.Backup=Path.Combine(session,"backups",record.Files.Count.ToString("D4")+".bak"); change.OriginalHash=HashFile(f.Destination);
                    Directory.CreateDirectory(Path.GetDirectoryName(change.Backup)!); File.Copy(f.Destination,change.Backup); Log("Backed up: "+f.Destination+" -> "+change.Backup);
                }
                record.Files.Add(change); Save();
                string temporary=f.Destination+".separate-song-"+Guid.NewGuid().ToString("N")+".tmp";
                try {
                    try { Directory.CreateDirectory(Path.GetDirectoryName(f.Destination)!); File.WriteAllBytes(temporary,f.Data); File.Move(temporary,f.Destination,true); }
                    catch(UnauthorizedAccessException) when(IsMachinePlugin(f.Destination)) { PrivilegedPluginWrite(f.Data); }
                    change.Applied=true; Save(); }
                finally { if(File.Exists(temporary)) File.Delete(temporary); }
                Log("Installed: "+f.Destination);
            }
            record.Status="installed"; Save();
            Log(SuccessMessage(o));
            Log("Backup and uninstall manifest: "+manifest);
            Log(o.ModOnly ? "Game mod updated. OBS was left running. Run full setup after your stream for the OBS plugin update." : ObsSetupMessage(o));
            File.WriteAllText(Path.Combine(o.StateRoot,"latest-manifest.txt"),manifest);

        }
        catch(Exception ex)
        {
            // A log write that fails must never stop the restore, so the rollback logs through QuietLog.
            var quiet=QuietLog(Log);
            quiet("Install failed: "+ex.Message+". Restoring this transaction.");
            RollBack(record,ex,quiet);
            record.Status=ex.Data["repair"] is true ? "rollback-incomplete" : "rolled-back";
            // A journal that can't record the outcome leaves the transaction in doubt.
            try { Save(); }
            catch(Exception journal) when(journal is IOException or UnauthorizedAccessException) { ex.Data["repair"]=true; }
            throw;
        }
        if(closedApps==null && o.Restart) RestartApps(stopped,Log);
    }
    internal static void RestartApps(List<string> stopped,Action<string> log)
    {
        foreach(var exe in stopped.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var psi=new ProcessStartInfo(exe){UseShellExecute=true,WorkingDirectory=Path.GetDirectoryName(exe)!};
            using var process=Process.Start(psi)??throw new IOException("Could not restart: "+exe);
            log("Restarted: "+exe);
        }
    }
    // Setup saves Applied only after replacing a file, so a setup killed in between leaves the new file
    // unrecorded; its hash still shows it was replaced. A file it can't read counts as replaced, so its
    // apps get closed and Restore reports that one file instead of stopping before the others.
    static bool Replaced(ChangedFile f)
    {
        if(f.Applied) return true;
        try { return File.Exists(f.Destination) && HashFile(f.Destination)==f.InstalledHash; }
        catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) { return true; }
    }
    // Restores every file it can, then reports the ones it couldn't.
    internal static void Restore(InstallRecord record,Action<string> log)
    {
        var failed=new List<string>();
        foreach(var f in record.Files.AsEnumerable().Reverse())
        {
            if(!Replaced(f)) continue;
            try { if(!RestoreFile(f,log)) failed.Add(f.Destination); }
            catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            {
                log("Could not restore, restore this file by hand: "+f.Destination+" ("+ex.Message+")"+(f.Backup!=null?" Backup: "+f.Backup:""));
                failed.Add(f.Destination);
            }
        }
        if(failed.Count>0) throw new IOException("Could not restore "+failed.Count+" file(s): "+string.Join(", ",failed)+". The setup log says how to restore each one by hand.");
    }
    // False when the backup is missing or altered.
    static bool RestoreFile(ChangedFile f,Action<string> log)
    {
        if(File.Exists(f.Destination) && HashFile(f.Destination)!=f.InstalledHash)
        {
            if(f.InstalledScene!=null && f.Backup!=null && File.Exists(f.Backup) && HashFile(f.Backup)==f.OriginalHash)
            {
                var restored=UndoCollection(File.ReadAllBytes(f.Backup),f.InstalledScene,File.ReadAllBytes(f.Destination));
                File.WriteAllBytes(f.Destination,restored); log("Removed installer scene changes while preserving later edits: "+f.Destination);
            }
            else log("Preserve changed file; manual restore available: "+f.Destination);
            return true;
        }
        if(f.Backup!=null)
        {
            if(!File.Exists(f.Backup) || HashFile(f.Backup)!=f.OriginalHash)
            {
                log("Missing or altered backup, restore this file by hand: "+f.Destination);
                return false;
            }
            try { Directory.CreateDirectory(Path.GetDirectoryName(f.Destination)!); File.Copy(f.Backup,f.Destination,true); }
            catch(UnauthorizedAccessException) when(IsMachinePlugin(f.Destination)) { PrivilegedPluginWrite(File.ReadAllBytes(f.Backup)); }
            log("Restored: "+f.Destination);
        }
        else if(File.Exists(f.Destination)) {
            try { File.Delete(f.Destination); }
            catch(UnauthorizedAccessException) when(IsMachinePlugin(f.Destination)) { RunPluginHelper("remove",f.InstalledHash); }
            log("Removed: "+f.Destination); }
        return true;
    }
    static void Uninstall(Options o,Action<string> log,List<string> stopped)
    {
        if(o.Manifest==null && !CanUndo(o)) { log("There is no previous installation to undo on this computer."); return; }
        string path=o.Manifest??(File.Exists(Path.Combine(o.StateRoot,"latest-manifest.txt"))?File.ReadAllText(Path.Combine(o.StateRoot,"latest-manifest.txt")).Trim():throw new IOException("There is no previous installation to undo on this computer."));
        var record=JsonSerializer.Deserialize<InstallRecord>(File.ReadAllText(path))??throw new IOException("Invalid manifest.");
        if(!IsKnownProduct(record.Product)) throw new IOException("Not an OBS Jukebox installation manifest.");
        if(record.Status!="installed") throw new IOException("This transaction is not installed: "+record.Status);
        if(o.DryRun) { foreach(var f in record.Files) log("Would restore/remove if unchanged: "+f.Destination); return; }
        o.GD=record.GD; o.OBS=record.OBS; CloseMatchingApps(o,log,stopped,record.Files.Where(f=>f.Applied).Select(f=>f.Destination));
        Restore(record,log); record.Status="uninstalled"; WriteManifest(path,record);
        log("Removed this install transaction. Later user changes were preserved. To restore an earlier installer update, repeat with that earlier manifest. Geode/Jukebox already present before this transaction were retained.");
    }
}

sealed class SetupForm : Form
{
    readonly Options options;
    readonly TextBox gd=new(),obs=new();
    readonly Label status=new(){Dock=DockStyle.Fill,MinimumSize=new Size(0,72),Font=new Font("Segoe UI",11,FontStyle.Bold)},dependency=new(){AutoSize=true};
    readonly CheckBox close=new(){Text="Close apps only when their files need updating",AutoSize=true},restart=new(){Text="Reopen apps closed by setup",AutoSize=true},scene=new(){Text="Add GD Sounds to my existing OBS scenes",AutoSize=true,Checked=false};
    readonly CheckBox modOnly=new(){Text="Update game mod only (keep OBS running)",AutoSize=true};
    readonly CheckBox jukebox=new(){Text="Download and install Jukebox 3.8.0",AutoSize=true};
    readonly Button install=new(){Text="Install",AutoSize=true},undo=new(){Text="Undo last install",AutoSize=true};
    readonly object logLock=new();
    string? attemptLog;
    bool working;
    // Closing mid-transaction would end the process before its rollback could run.
    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if(working){e.Cancel=true;SetStatus("Wait for setup to finish before closing it.");}
        base.OnFormClosing(e);
    }
    public SetupForm(Options o)
    {
        options=o; Text="OBS Jukebox - Windows Setup"; Width=730;Height=650;MinimumSize=new(730,650);AutoSize=true;AutoSizeMode=AutoSizeMode.GrowOnly;StartPosition=FormStartPosition.CenterScreen;Font=new Font("Segoe UI",10);
        var layout=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(22),ColumnCount=1,RowCount=9,AutoSize=true,AutoScroll=true};Controls.Add(layout);
        for(int row=0;row<6;row++)layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent,100));layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        using(var logoStream=Assembly.GetExecutingAssembly().GetManifestResourceStream("logo.png"))
        {
            var heading=new FlowLayoutPanel{Dock=DockStyle.Fill,WrapContents=false,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink};
            if(logoStream!=null){using var bitmap=new Bitmap(logoStream);heading.Controls.Add(new PictureBox{Image=new Bitmap(bitmap),SizeMode=PictureBoxSizeMode.Zoom,Width=42,Height=42,Margin=new Padding(0,0,12,0)});}
            heading.Controls.Add(new Label{Text="OBS Jukebox",Font=new Font("Segoe UI",23,FontStyle.Bold),AutoSize=true});layout.Controls.Add(heading);
        }
        Icon=Icon.ExtractAssociatedIcon(Environment.ProcessPath!);
        layout.Controls.Add(new Label{Text="Separate GD/Song output for OBS",AutoSize=true});
        gd.Text=DisplayPath(o.GD);obs.Text=DisplayPath(o.OBS);layout.Controls.Add(PathRow("Geometry Dash folder",gd));layout.Controls.Add(PathRow("OBS Studio folder",obs));
        var prerequisite=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Padding=new Padding(0,6,0,6)};prerequisite.Controls.Add(dependency);prerequisite.Controls.Add(jukebox);layout.Controls.Add(prerequisite);
        var choices=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Padding=new Padding(0,0,0,18)};choices.Controls.Add(modOnly);choices.Controls.Add(new Label{Text="Mod-only: restart GD now; run full setup after your stream for OBS fixes.",AutoSize=true});choices.Controls.Add(scene);choices.Controls.Add(close);choices.Controls.Add(restart);layout.Controls.Add(choices);
        modOnly.Checked=o.ModOnly;modOnly.CheckedChanged+=(_,_)=>{scene.Enabled=!modOnly.Checked;};scene.Enabled=!modOnly.Checked;
        close.Checked=o.CloseApps||!o.Silent;restart.Checked=o.Restart||!o.Silent;scene.Checked=o.CreateScene;jukebox.Checked=o.InstallJukebox;
        status.Text="Finish any OBS recording, then click Install.";layout.Controls.Add(status);
        var buttons=new FlowLayoutPanel{Dock=DockStyle.Fill,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink};install.Click+=async(_,_)=>await Install();undo.Click+=async(_,_)=>await Install(true);buttons.Controls.Add(install);buttons.Controls.Add(undo);layout.Controls.Add(buttons);
        var logs=new LinkLabel{Text="Open backups and setup logs",AutoSize=true};logs.LinkClicked+=(_,_)=>{Directory.CreateDirectory(options.StateRoot);Process.Start(new ProcessStartInfo(options.StateRoot){UseShellExecute=true});};layout.Controls.Add(logs);
        gd.TextChanged+=(_,_)=>UpdateDependency();UpdateDependency();undo.Enabled=Engine.CanUndo(options);
        if(!undo.Enabled)new ToolTip().SetToolTip(undo,"There is no previous installation to undo.");
    }
    static Control PathRow(string title,TextBox input)
    {
        var panel=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2,RowCount=2,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Padding=new Padding(0,6,0,6)};panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));panel.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));panel.RowStyles.Add(new RowStyle(SizeType.AutoSize));panel.Controls.Add(new Label{Text=title,AutoSize=true},0,0);input.Anchor=AnchorStyles.Left|AnchorStyles.Right;panel.Controls.Add(input,0,1);
        var browse=new Button{Text="Browse...",AutoSize=true,Anchor=AnchorStyles.Left|AnchorStyles.Right};browse.Click+=(_,_)=>{using var dialog=new FolderBrowserDialog{Description="Choose "+title,InitialDirectory=input.Text};if(dialog.ShowDialog()==DialogResult.OK)input.Text=DisplayPath(dialog.SelectedPath);};panel.Controls.Add(browse,1,1);return panel;
    }
    static string DisplayPath(string path)
    {
        try
        {
            var folder=new DirectoryInfo(path);
            if(!folder.Exists)return path;
            if(folder.Parent is not DirectoryInfo parent)
                return folder.FullName.Length>=2&&folder.FullName[1]==':'?char.ToUpperInvariant(folder.FullName[0])+folder.FullName[1..]:folder.FullName;
            var name=parent.EnumerateDirectories().FirstOrDefault(entry=>string.Equals(entry.Name,folder.Name,StringComparison.OrdinalIgnoreCase))?.Name??folder.Name;
            return Path.Combine(DisplayPath(parent.FullName),name);
        }
        catch(IOException){return path;}
        catch(UnauthorizedAccessException){return path;}
        catch(ArgumentException){return path;}
    }
    void UpdateDependency()
    {
        try
        {
            var found=Engine.FindJukebox(Options.NormalizeGD(gd.Text));
            jukebox.Visible=found==null;
            dependency.Text=found==null?"Jukebox 3.8.0 is required.":found.Value.Version=="3.8.0"?"Jukebox 3.8.0 is installed.":"Install Jukebox 3.8.0 from Geode before continuing.";
            if(found!=null)jukebox.Checked=false;
        }
        catch(IOException ex){dependency.Text=ex.Message;jukebox.Visible=false;}
        catch(Exception){dependency.Text="Choose your Geometry Dash folder.";jukebox.Visible=false;}
    }
    void Collect(){options.GD=Options.NormalizeGD(gd.Text);options.OBS=Options.NormalizeOBS(obs.Text);options.CloseApps=close.Checked;options.Restart=restart.Checked;options.ModOnly=modOnly.Checked;options.CreateScene=scene.Checked&&!modOnly.Checked;options.InstallJukebox=jukebox.Visible&&jukebox.Checked;}
    void SetStatus(string text,bool error=false)
    {
        if(InvokeRequired){BeginInvoke(()=>SetStatus(text,error));return;}
        status.ForeColor=error?Color.Firebrick:SystemColors.ControlText;status.Text=text;
    }
    void Detail(string message)
    {
        lock(logLock)if(attemptLog!=null)File.AppendAllText(attemptLog,DateTimeOffset.Now.ToString("O")+" "+message+Environment.NewLine);
        if(message.StartsWith("Downloading"))SetStatus("Downloading Jukebox 3.8.0...");
        else if(message.StartsWith("Requesting normal close"))SetStatus("Close any OBS confirmation to continue installation.");
        else if(message.StartsWith("Installed:"))SetStatus("Installing OBS Jukebox...");
        else if(message.StartsWith("Restarted:"))SetStatus("Reopening Geometry Dash and OBS...");
    }
    async Task Install(bool restoring=false)
    {
        Control[] inputs=[gd.Parent!,obs.Parent!,scene,modOnly,close,restart,jukebox,install,undo];
        foreach(var input in inputs)input.Enabled=false;
        bool success=false;
        working=true;
        try
        {
            Collect();options.Uninstall=restoring;
            if(restoring&&!Engine.CanUndo(options)){SetStatus("There is no previous installation to undo.");return;}
            string logs=Path.Combine(options.StateRoot,"SetupLogs");Directory.CreateDirectory(logs);attemptLog=Path.Combine(logs,DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N")[..6]+".log");
            SetStatus(restoring?"Restoring the previous installation...":options.ModOnly?"Updating the game mod. OBS will stay running.":"Installing. Only apps with changed files need to close.");
            var stopped=new List<string>();
            await Task.Run(()=>Engine.Execute(options,Detail,stopped));
            if(options.Restart)Engine.RestartApps(stopped,Detail);
            success=true;
        }
        catch(Exception ex)
        {
            try{Detail(ex.ToString());}catch(IOException){}
            SetStatus(ex is UnauthorizedAccessException ? "Setup cannot write this folder. Choose a writable installation or grant your account access, then retry. User files are never edited by an elevated installer." : ex.Message,true);
        }
        finally{working=false;foreach(var input in inputs)input.Enabled=true;scene.Enabled=!modOnly.Checked;options.Uninstall=false;undo.Enabled=Engine.CanUndo(options);}
        if(success)SetStatus(restoring?"Undo complete. Later user changes were preserved. You may close setup.":Engine.SuccessMessage(options));
    }
}

// The setup the mod starts for an update: wait until GD and OBS are closed, install, and report the
// outcome in a file the mod reads on its next launch. It never shows a window.
static class UpdateMode
{
    internal const string LockName=@"Local\OBSJukeboxUpdate";
    internal sealed record Host(Func<string,bool> Running,Action Sleep,Action Install,Action StartGD,Func<string?> InstalledVersion);

    internal static int Entry(string[] args)
    {
        // Found by a plain scan, so a parsing error still reaches the mod.
        int at=Array.IndexOf(args,"--result");
        string? result=at>=0 && at+1<args.Length ? args[at+1] : null;
        try
        {
            var o=Options.Parse(args);
            using var single=new Mutex(false,LockName);
            try { if(!single.WaitOne(0)) return 0; }
            catch(AbandonedMutexException) { }
            try { return Run(o,RealHost(o)); }
            finally { single.ReleaseMutex(); }
        }
        catch(Exception ex)
        {
            // Best effort: with no writable result path, the session log is the only record.
            try { if(result!=null) WriteResult(result,"failed",ex.Message,false); }
            catch(Exception ex2) when(ex2 is IOException or UnauthorizedAccessException) { }
            return 1;
        }
    }
    static Host RealHost(Options o) => new(
        name => { var found=Process.GetProcessesByName(name); foreach(var p in found) p.Dispose(); return found.Length>0; },
        () => Thread.Sleep(2000),
        () => Engine.Execute(o,s=>Log(o,s)),
        () => Engine.RestartApps([Path.Combine(o.GD,"GeometryDash.exe")],s=>Log(o,s)),
        () => Engine.InstalledVersion(o.GD));
    // No console is attached in update mode, so every line also goes to a file the player can send.
    static void Log(Options o,string line)
    {
        try { Directory.CreateDirectory(o.StateRoot); File.AppendAllText(Path.Combine(o.StateRoot,"update.log"),DateTimeOffset.Now.ToString("O")+" "+line+Environment.NewLine); }
        catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) { }
    }

    internal static int Run(Options o,Host host)
    {
        if(Engine.IsNewer(host.InstalledVersion(),Program.ReleaseVersion)) return 0;
        Write(o,"waiting");
        while(true)
        {
            bool gd=host.Running("GeometryDash"),obs=host.Running("obs64");
            if(gd || obs)
            {
                // "Update now" closed GD but OBS is open: give GD back now, install later.
                if(!gd && TakeReopenFlag(o)) StartGD(o,host);
                host.Sleep(); continue;
            }
            try { host.Install(); }
            catch(Exception ex) when(ex is AppRunningException or SetupBusyException) { host.Sleep(); continue; }
            catch(NewerVersionInstalledException) { return 0; }
            catch(Exception ex)
            {
                if(ex.Data["repair"] is true) Write(o,"failed",ex.Message,true);
                else if(CancelledElevation(ex)) Write(o,"declined");
                else Write(o,"failed",ex.Message);
                return 1;
            }
            Write(o,"installed");
            if(TakeReopenFlag(o)) StartGD(o,host);
            return 0;
        }
    }
    // Reopening can fail (the game moved, the shell refused); that must not turn a finished install into a failure.
    static void StartGD(Options o,Host host)
    {
        try { host.StartGD(); }
        catch(Exception ex) when(ex is IOException or System.ComponentModel.Win32Exception) { Log(o,"Could not reopen Geometry Dash: "+ex.Message); }
    }
    static bool TakeReopenFlag(Options o)
    {
        // A flag that can't be read or deleted is not worth failing a finished install over.
        try
        {
            if(o.ReopenFlag==null || !File.Exists(o.ReopenFlag)) return false;
            File.Delete(o.ReopenFlag); return true;
        }
        catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) { return false; }
    }
    static bool CancelledElevation(Exception? ex)
    {
        for(;ex!=null;ex=ex.InnerException) if(ex is System.ComponentModel.Win32Exception { NativeErrorCode: 1223 }) return true;
        return false;
    }
    static void Write(Options o,string status,string? message=null,bool repair=false)
    {
        if(o.ResultPath!=null) WriteResult(o.ResultPath,status,message,repair);
    }
    // Through a temporary file, so the mod never reads half a result.
    internal static void WriteResult(string path,string status,string? message,bool repair)
    {
        var json=new JsonObject{["version"]=Program.ReleaseVersion,["status"]=status};
        if(message!=null) json["message"]=message;
        if(repair) json["repair"]=true;
        File.WriteAllText(path+".tmp",json.ToJsonString());
        File.Move(path+".tmp",path,true);
    }
}

