using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace SeparateSongSetup;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        try
        {
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
            if (!args.Contains("--silent")) MessageBox.Show(ex.Message, "OBS Jukebox Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
    public bool Silent, DryRun, CloseApps, Restart, Uninstall, InstallJukebox, CreateScene = true;
    internal byte[]? DownloadedJukebox;
    public string? Manifest;
    public static Options Parse(string[] args)
    {
        var o = new Options();
        for (int i = 0; i < args.Length; i++)
        {
            string Next() => ++i < args.Length ? args[i] : throw new ArgumentException("Missing value for " + args[i-1]);
            switch (args[i])
            {
                case "--gd": o.GD = Next(); break;
                case "--obs": o.OBS = Next(); break;
                case "--plugin-root": o.PluginRoot = Next(); break;
                case "--state-root": o.StateRoot = Next(); break;
                case "--scene-root": o.SceneRoot = Next(); break;
                case "--manifest": o.Manifest = Next(); break;
                case "--silent": o.Silent = true; break;
                case "--dry-run": o.DryRun = true; break;
                case "--close-apps": o.CloseApps = true; break;
                case "--restart": o.Restart = true; break;
                case "--install-jukebox": o.InstallJukebox = true; break;
                case "--no-scene": case "--no-integrate": o.CreateScene = false; break;
                case "--uninstall": o.Uninstall = true; break;
                default: throw new ArgumentException("Unknown option: " + args[i]);
            }
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
            try { if (p.MainModule?.FileName is string file) return Path.GetDirectoryName(file)!; } catch { }
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
            try { if (p.MainModule?.FileName is string file) return NormalizeOBS(file); } catch { }
        var registry = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio", "InstallLocation", "")?.ToString();
        return !string.IsNullOrWhiteSpace(registry) ? registry : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "obs-studio");
    }
    public IEnumerable<string> Arguments()
    {
        foreach (var s in new[]{"--silent", "--gd", GD, "--obs", OBS, "--plugin-root", PluginRoot, "--state-root", StateRoot, "--scene-root", SceneRoot}) yield return s;
        if (CloseApps) yield return "--close-apps";
        if (Restart) yield return "--restart";
        if (InstallJukebox) yield return "--install-jukebox";
        if (!CreateScene) yield return "--no-integrate";
        if (Uninstall) yield return "--uninstall";
        if (Manifest != null) { yield return "--manifest"; yield return Manifest; }
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
    public bool Applied { get; set; }
}
sealed class InstallRecord
{
    public string Product { get; set; } = "OBS Jukebox 1.0.0";
    public string GD { get; set; } = "";
    public string OBS { get; set; } = "";
    public string Status { get; set; } = "installing";
    public List<ChangedFile> Files { get; set; } = [];
}

static class Engine
{
    static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
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
        if (!zip.Entries.Any(x => x.FullName.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))) throw new IOException(expectedId + " has no Windows DLL.");
        return j["version"]!.ToString().TrimStart('v');
    }
    internal const string JukeboxUrl="https://github.com/Fleeym/jukebox/releases/download/v3.8.0/fleym.nongd.geode";
    internal const string JukeboxHash="A0ECA6C82C7FA6149B956A809A9058959957D7C4672368B6933EA030E641B621";
    internal static (string Path,string Version)? FindJukebox(string gd)
    {
        string mods=Path.Combine(gd,"geode","mods");
        if(!Directory.Exists(mods)) return null;
        foreach(string file in Directory.GetFiles(mods,"*.geode"))
        {
            try
            {
                using var zip=ZipFile.OpenRead(file); var metadata=zip.GetEntry("mod.json"); if(metadata==null)continue;
                using var input=metadata.Open(); var json=JsonNode.Parse(input);
                if(json?["id"]?.ToString()!="fleym.nongd")continue;
                if(!zip.Entries.Any(e=>e.FullName.EndsWith(".dll",StringComparison.OrdinalIgnoreCase))) return (file,"without Windows support");
                return (file,json["version"]?.ToString().TrimStart('v')??"unknown version");
            }
            catch(InvalidDataException) { }
            catch(JsonException) { }
        }
        return null;
    }
    static byte[] DownloadJukebox(Action<string> log)
    {
        log("Downloading official Jukebox 3.8.0...");
        try
        {
            using var client=new System.Net.Http.HttpClient { Timeout=TimeSpan.FromSeconds(60) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("OBS-Jukebox-Setup/1.0.0");
            byte[] bytes=client.GetByteArrayAsync(JukeboxUrl).GetAwaiter().GetResult();
            if(Hash(bytes)!=JukeboxHash)throw new IOException("Jukebox download verification failed. No downloaded files were installed.");
            if(PackageVersion(bytes,"fleym.nongd")!="3.8.0")throw new IOException("Jukebox download has unexpected metadata.");
            log("Verified official Jukebox 3.8.0 SHA256="+JukeboxHash); return bytes;
        }
        catch(System.Net.Http.HttpRequestException ex) { throw new IOException("Could not download Jukebox. Check your connection or install Jukebox 3.8.0 from Geode, then retry.",ex); }
        catch(TaskCanceledException ex) { throw new IOException("Jukebox download timed out. Install Jukebox 3.8.0 from Geode or retry.",ex); }
    }
    internal static bool CanUndo(Options o)
    {
        try
        {
            string path=o.Manifest??File.ReadAllText(Path.Combine(o.StateRoot,"latest-manifest.txt")).Trim();
            var record=JsonSerializer.Deserialize<InstallRecord>(File.ReadAllText(path));
            return record is { Status: "installed" } && (record.Product=="OBS Jukebox 1.0.0" || record.Product=="Separate Song 1.0.0");
        }
        catch(IOException) { return false; }
        catch(UnauthorizedAccessException) { return false; }
        catch(JsonException) { return false; }
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
        byte[] mod=payload["mods/local.separate_song.geode"];
        if(PackageVersion(mod,"local.separate_song")!="1.0.0") throw new IOException("Unexpected OBS Jukebox payload version.");
        Add(Path.Combine(o.GD,"geode","mods","local.separate_song.geode"),mod,"OBS Jukebox mod 1.0.0");
        bool portable = File.Exists(Path.Combine(o.OBS,"portable_mode.txt")) || File.Exists(Path.Combine(o.OBS,"portable_mode"));
        string plugin = portable ? Path.Combine(o.OBS,"obs-plugins","64bit","separate-song.dll") : Path.Combine(o.PluginRoot,"separate-song","bin","64bit","separate-song.dll");
        Add(plugin,payload["obs/separate-song.dll"],"Native OBS source GD Sounds");
        if (o.CreateScene)
        {
            string sceneRoot = portable ? Path.Combine(o.OBS,"config","obs-studio","basic","scenes") : o.SceneRoot;
            if (!Directory.Exists(sceneRoot)) log("No existing OBS collections found. In OBS add Sources > GD Sounds once; setup creates no new collection.");
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
                if(sources.OfType<JsonObject>().Any(s=>s["name"]?.ToString()==name)) name=defaultName+" (OBS Jukebox)";
                existing=new JsonObject { ["name"]=name,["uuid"]=Guid.NewGuid().ToString(),["id"]=type,["versioned_id"]=type,["settings"]=new JsonObject(),["mixers"]=255,["sync"]=0,["flags"]=0,["volume"]=1.0,["balance"]=0.5,["enabled"]=true,["muted"]=false,["monitoring_type"]=0,["hotkeys"]=new JsonObject(),["private_settings"]=new JsonObject() };
                sources.Add(existing); modified=true;
            }
            string oldName=existing["name"]!.ToString();
            if(type=="gd_alternate_song" && (oldName=="GD Alternate Song" || oldName=="Custom Song" || oldName=="OBS Custom Song") && !sources.OfType<JsonObject>().Any(s=>s!=existing && s["name"]?.ToString()=="GD Sounds")) Set(existing,"name",JsonValue.Create("GD Sounds"));
            Set(existing,"monitoring_type",JsonValue.Create(0));
            string sourceName=existing["name"]!.ToString(); string uuid=existing["uuid"]?.ToString()??Guid.NewGuid().ToString(); Set(existing,"uuid",JsonValue.Create(uuid));
            foreach(var scene in sources.OfType<JsonObject>().Where(s=>s["id"]?.ToString()=="scene"))
            {
                var settings=scene["settings"] as JsonObject??throw new IOException("Missing scene settings.");
                var items=settings["items"] as JsonArray??new JsonArray(); if(settings["items"]==null)settings["items"]=items;
                var item=items.OfType<JsonObject>().FirstOrDefault(i=>i["source_uuid"]?.ToString()==uuid || i["name"]?.ToString()==oldName);
                if(item!=null){Set(item,"name",JsonValue.Create(sourceName));Set(item,"source_uuid",JsonValue.Create(uuid));continue;}
                int next=items.OfType<JsonObject>().Select(i=>i["id"]?.GetValue<int>()??0).DefaultIfEmpty(0).Max()+1;
                items.Add(new JsonObject { ["name"]=sourceName,["source_uuid"]=uuid,["id"]=next,["visible"]=true,["locked"]=true,["rot"]=0.0,["pos"]=new JsonObject{["x"]=0.0,["y"]=0.0},["scale"]=new JsonObject{["x"]=1.0,["y"]=1.0},["align"]=5,["bounds_type"]=0,["bounds_align"]=0,["bounds"]=new JsonObject{["x"]=0.0,["y"]=0.0},["crop_left"]=0,["crop_top"]=0,["crop_right"]=0,["crop_bottom"]=0,["private_settings"]=new JsonObject() });
                settings["id_counter"]=Math.Max(settings["id_counter"]?.GetValue<int>()??0,next); modified=true;
            }
        }
        changed=modified; return modified?JsonSerializer.SerializeToUtf8Bytes(collection,JsonOptions):original;
    }
    static void CloseMatchingApps(Options o,Action<string> log,List<string> stopped)
    {
        foreach (var (name, exe) in new[]{("GeometryDash",Path.Combine(o.GD,"GeometryDash.exe")),("obs64",Path.Combine(o.OBS,"bin","64bit","obs64.exe"))})
        foreach (var p in Process.GetProcessesByName(name))
        {
            string? actual;
            try { actual=p.MainModule?.FileName; } catch { throw new IOException("Cannot inspect running " + name + ". Close it yourself before installing."); }
            if (!string.Equals(actual,exe,StringComparison.OrdinalIgnoreCase)) continue;
            if (!o.CloseApps) throw new IOException(name + " is running. Close it first or choose Ask apps to close. OBS can ask you to stop recording/replay; this setup never force-kills it.");
            log("Requesting normal close: " + actual);
            if (!p.CloseMainWindow() || !p.WaitForExit(20000)) throw new IOException(name + " is still running. Resolve its confirmation or close it manually, then retry. No process was force-killed.");
            stopped.Add(exe);
        }
    }
    public static void Execute(Options o,Action<string> output,List<string>? closedApps=null)
    {
        var logLines = new List<string>(); string? logPath=null;
        void Log(string s) { string line=DateTimeOffset.Now.ToString("O") + " " + s; logLines.Add(line); output(s); if(logPath!=null) File.AppendAllText(logPath,line+Environment.NewLine); }
        var stopped=closedApps??new List<string>();
        if (o.Uninstall) { Uninstall(o,Log,stopped); if(closedApps==null && o.Restart) RestartApps(stopped,Log); return; }
        var plan=Plan(o,Log);
        foreach(var f in plan) Log($"{f.Reason}: {f.Destination} SHA256={Hash(f.Data)} ({f.Data.Length:N0} bytes)");
        if(o.DryRun) { Log("Dry run complete. No destination files changed and no apps were closed."); return; }
        if(plan.Count==0) { Log("Already installed and integrated. No files changed; existing restore manifest preserved."); return; }
        CloseMatchingApps(o,Log,stopped);
        plan=Plan(o,Log);
        string session=Path.Combine(o.StateRoot,DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N")[..6]);
        Directory.CreateDirectory(session); logPath=Path.Combine(session,"install.log"); File.WriteAllLines(logPath,logLines);
        var record=new InstallRecord{GD=o.GD,OBS=o.OBS}; string manifest=Path.Combine(session,"manifest.json");
        void Save() => File.WriteAllText(manifest,JsonSerializer.Serialize(record,JsonOptions));
        Save();
        try
        {
            foreach(var f in plan)
            {
                var change=new ChangedFile{Destination=f.Destination,InstalledHash=Hash(f.Data),Reason=f.Reason};
                if(File.Exists(f.Destination))
                {
                    change.Backup=Path.Combine(session,"backups",record.Files.Count.ToString("D4")+".bak"); change.OriginalHash=HashFile(f.Destination);
                    Directory.CreateDirectory(Path.GetDirectoryName(change.Backup)!); File.Copy(f.Destination,change.Backup); Log("Backed up: "+f.Destination+" -> "+change.Backup);
                }
                record.Files.Add(change); Save();
                Directory.CreateDirectory(Path.GetDirectoryName(f.Destination)!);
                string temporary=f.Destination+".separate-song-"+Guid.NewGuid().ToString("N")+".tmp";
                try { File.WriteAllBytes(temporary,f.Data); File.Move(temporary,f.Destination,true); change.Applied=true; Save(); }
                finally { if(File.Exists(temporary)) File.Delete(temporary); }
                Log("Installed: "+f.Destination);
            }
            record.Status="installed"; Save();
            Log("Install complete. Backup and uninstall manifest: "+manifest);
            Log("OBS: GD Sounds is added to existing scenes. In GD Jukebox, use the Game and OBS checkboxes in Jukebox. Audio Monitoring stays off. Existing OBS collections and unrelated mods are preserved.");
            File.WriteAllText(Path.Combine(o.StateRoot,"latest-manifest.txt"),manifest);

        }
        catch(Exception ex)
        {
            Log("Install failed: "+ex.Message+". Restoring this transaction.");
            Restore(record,Log); record.Status="rolled-back"; Save(); throw;
        }
        if(closedApps==null && o.Restart) RestartApps(stopped,Log);
    }
    internal static async Task ExecuteWithRetry(Options o,Func<List<string>,Task> execute,Func<Task> retry,Action<List<string>> restart)
    {
        var stopped=new List<string>();
        try { await execute(stopped); }
        catch(UnauthorizedAccessException) { await retry(); }
        if(o.Restart) restart(stopped.Distinct(StringComparer.OrdinalIgnoreCase).ToList());
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
    static void Restore(InstallRecord record,Action<string> log)
    {
        foreach(var f in record.Files.AsEnumerable().Reverse())
        {
            if(!f.Applied) continue;
            if(File.Exists(f.Destination) && HashFile(f.Destination)!=f.InstalledHash) { log("Preserve changed file; manual restore available: "+f.Destination); continue; }
            if(f.Backup!=null)
            {
                if(!File.Exists(f.Backup) || HashFile(f.Backup)!=f.OriginalHash) throw new IOException("Missing or altered backup: "+f.Backup);
                Directory.CreateDirectory(Path.GetDirectoryName(f.Destination)!); File.Copy(f.Backup,f.Destination,true); log("Restored: "+f.Destination);
            }
            else if(File.Exists(f.Destination)) { File.Delete(f.Destination); log("Removed: "+f.Destination); }
        }
    }
    static void Uninstall(Options o,Action<string> log,List<string> stopped)
    {
        if(o.Manifest==null && !CanUndo(o)) { log("There is no previous installation to undo on this computer."); return; }
        string path=o.Manifest??(File.Exists(Path.Combine(o.StateRoot,"latest-manifest.txt"))?File.ReadAllText(Path.Combine(o.StateRoot,"latest-manifest.txt")).Trim():throw new IOException("There is no previous installation to undo on this computer."));
        var record=JsonSerializer.Deserialize<InstallRecord>(File.ReadAllText(path))??throw new IOException("Invalid manifest.");
        if(record.Product!="OBS Jukebox 1.0.0" && record.Product!="Separate Song 1.0.0") throw new IOException("Not an OBS Jukebox installation manifest.");
        if(record.Status!="installed") throw new IOException("This transaction is not installed: "+record.Status);
        if(o.DryRun) { foreach(var f in record.Files) log("Would restore/remove if unchanged: "+f.Destination); return; }
        o.GD=record.GD; o.OBS=record.OBS; CloseMatchingApps(o,log,stopped);
        Restore(record,log); record.Status="uninstalled"; File.WriteAllText(path,JsonSerializer.Serialize(record,JsonOptions));
        log("Removed this install transaction. Later user changes were preserved. To restore an earlier installer update, repeat with that earlier manifest. Geode/Jukebox already present before this transaction were retained.");
    }
}

sealed class SetupForm : Form
{
    readonly Options options;
    readonly TextBox gd=new(),obs=new();
    readonly Label status=new(){Dock=DockStyle.Fill,MinimumSize=new Size(0,72),Font=new Font("Segoe UI",11,FontStyle.Bold)},dependency=new(){AutoSize=true};
    readonly CheckBox close=new(){Text="Close GD and OBS for installation",AutoSize=true},restart=new(){Text="Reopen GD and OBS afterward",AutoSize=true},scene=new(){Text="Add GD Sounds to my existing OBS scenes",AutoSize=true,Checked=true};
    readonly CheckBox jukebox=new(){Text="Download and install Jukebox 3.8.0",AutoSize=true};
    readonly Button install=new(){Text="Install",AutoSize=true},undo=new(){Text="Undo last install",AutoSize=true};
    readonly object logLock=new();
    string? attemptLog;
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
        var choices=new FlowLayoutPanel{Dock=DockStyle.Fill,FlowDirection=FlowDirection.TopDown,WrapContents=false,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Padding=new Padding(0,0,0,18)};choices.Controls.Add(scene);choices.Controls.Add(close);choices.Controls.Add(restart);layout.Controls.Add(choices);
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
        catch(Exception){dependency.Text="Choose your Geometry Dash folder.";jukebox.Visible=false;}
    }
    void Collect(){options.GD=Options.NormalizeGD(gd.Text);options.OBS=Options.NormalizeOBS(obs.Text);options.CloseApps=close.Checked;options.Restart=restart.Checked;options.CreateScene=scene.Checked;options.InstallJukebox=jukebox.Visible&&jukebox.Checked;}
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
        Control[] inputs=[gd.Parent!,obs.Parent!,scene,close,restart,jukebox,install,undo];
        foreach(var input in inputs)input.Enabled=false;
        bool success=false;
        try
        {
            Collect();options.Uninstall=restoring;
            if(restoring&&!Engine.CanUndo(options)){SetStatus("There is no previous installation to undo.");return;}
            string logs=Path.Combine(options.StateRoot,"SetupLogs");Directory.CreateDirectory(logs);attemptLog=Path.Combine(logs,DateTime.UtcNow.ToString("yyyyMMdd-HHmmss")+"-"+Guid.NewGuid().ToString("N")[..6]+".log");
            SetStatus(restoring?"Restoring the previous installation...":options.Restart?"Installing. OBS will reopen when setup finishes.":"Installing. Restart OBS afterward to load GD Sounds.");
            await Engine.ExecuteWithRetry(options,
                stopped=>Task.Run(()=>Engine.Execute(options,Detail,stopped)),
                async()=>
                {
                    SetStatus("Approve Windows administrator permission to continue.");
                    var psi=new ProcessStartInfo(Environment.ProcessPath!){UseShellExecute=true,Verb="runas"};foreach(var arg in options.Arguments())psi.ArgumentList.Add(arg);
                    using var process=Process.Start(psi)??throw new IOException("Could not start the installer with administrator permission.");await process.WaitForExitAsync();
                    if(process.ExitCode!=0)throw new IOException("Installation did not finish. Open setup logs for details, then retry.");
                },stopped=>Engine.RestartApps(stopped,Detail));
            success=true;
        }
        catch(Exception ex)
        {
            try{Detail(ex.ToString());}catch(IOException){}
            SetStatus(ex.Message,true);
        }
        finally{foreach(var input in inputs)input.Enabled=true;options.Uninstall=false;undo.Enabled=Engine.CanUndo(options);}
        if(success)Close();
    }
}
