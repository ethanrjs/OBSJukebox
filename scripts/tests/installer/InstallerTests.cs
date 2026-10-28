using System.IO.Compression;
using System.Text;
using System.Text.Json.Nodes;
using System.Diagnostics;

namespace SeparateSongSetup;

static class InstallerTests
{
    static void Check(bool condition,string name)
    {
        if(!condition) throw new Exception(name);
        Console.WriteLine("PASS: "+name);
    }
    static void WriteMod(string path,string id,string version,string binary)
    {
        using var zip=ZipFile.Open(path,ZipArchiveMode.Create);
        using(var writer=new StreamWriter(zip.CreateEntry("mod.json").Open())) writer.Write($$"""{"id":"{{id}}","version":"v{{version}}"}""");
        zip.CreateEntry(binary).Open().Dispose();
    }
    // A process with the given name, from a copy of ping.exe, for tests that need an app "running".
    static Process FakeApp(string folder,string name)
    {
        string exe=Path.Combine(folder,name+".exe");
        File.Copy(Path.Combine(Environment.SystemDirectory,"PING.EXE"),exe,true);
        return Process.Start(new ProcessStartInfo(exe,"-n 120 127.0.0.1"){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true})!;
    }
    // A fake update host. Each loop reads one (GD, OBS) state; the last one repeats.
    static UpdateMode.Host FakeHost(List<string> events,(bool Gd,bool Obs)[] states,Action install,string? installed=null)
    {
        int at=-1;
        return new(
            name => { if(name=="GeometryDash") at=Math.Min(at+1,states.Length-1); return name=="GeometryDash" ? states[at].Gd : states[at].Obs; },
            () => events.Add("sleep"),
            () => { events.Add("install"); install(); },
            () => events.Add("start-gd"),
            () => installed);
    }
    static JsonNode ReadResult(string path) => JsonNode.Parse(File.ReadAllText(path))!;
    static void Main()
    {
        byte[] original=Encoding.UTF8.GetBytes("""
        {"name":"User scenes","DesktopAudioDevice1":{"muted":false,"volume":0.7},
         "DesktopAudioDevice2":{"muted":true},"sources":[
         {"id":"scene","name":"Game","settings":{"items":[],"id_counter":0}},
         {"id":"game_capture","name":"GD","muted":false,"settings":{"window":"Geometry Dash:GLFW30:GeometryDash.exe","capture_audio":true}},
         {"id":"game_capture","name":"Fullscreen","muted":true,"settings":{"capture_mode":"any_fullscreen","capture_audio":true}},
         {"id":"wasapi_output_capture","name":"Desktop","muted":false,"settings":{"device_id":"default"}},
         {"id":"wasapi_process_output_capture","name":"GD audio","muted":false,"settings":{"window":"GD:GLFW30:GeometryDash.exe"}},
         {"id":"wasapi_input_capture","name":"Mic","muted":true,"volume":0.25,"settings":{}}]}
        """);
        var before=JsonNode.Parse(original)!;
        byte[] integrated=Engine.IntegrateCollection(original,out bool changed);
        var after=JsonNode.Parse(integrated)!;
        Check(changed && after["sources"]!.AsArray().Count==before["sources"]!.AsArray().Count+1,"adds shared GD Sounds source");
        Check(JsonNode.DeepEquals(before["DesktopAudioDevice1"],after["DesktopAudioDevice1"]) && JsonNode.DeepEquals(before["DesktopAudioDevice2"],after["DesktopAudioDevice2"]),"preserves global desktop audio and mute states");
        Check(before["sources"]!.AsArray().Skip(1).All(b=>after["sources"]!.AsArray().Any(a=>JsonNode.DeepEquals(a,b))),"preserves capture audio, individual mutes, volumes and microphone settings");
        var again=Engine.IntegrateCollection(integrated,out bool changedAgain);
        Check(!changedAgain && again.SequenceEqual(integrated),"integration remains idempotent");

        byte[] collisions=Encoding.UTF8.GetBytes("""
        {"sources":[
         {"id":"image_source","name":"GD Sounds","uuid":"image-1"},
         {"id":"image_source","name":"GD Sounds (OBS Jukebox)","uuid":"image-2"},
         {"id":"image_source","name":"GD Sounds (OBS Jukebox) 2","uuid":"image-3"},
         {"id":"scene","name":"Game","settings":{"items":[
           {"name":"GD Sounds","source_uuid":"image-1","id":1,"visible":true},
           {"name":"GD Sounds (OBS Jukebox)","source_uuid":"image-2","id":2,"visible":false},
           {"name":"GD Sounds (OBS Jukebox) 2","source_uuid":"image-3","id":3,"visible":true}],"id_counter":3}}]}
        """);
        byte[] collisionResult=Engine.IntegrateCollection(collisions,out _);
        var collisionSources=JsonNode.Parse(collisionResult)!["sources"]!.AsArray();
        var newAudio=collisionSources.Single(s=>s!["id"]!.ToString()=="gd_alternate_song")!;
        var collisionItems=collisionSources.Single(s=>s!["id"]!.ToString()=="scene")!["settings"]!["items"]!.AsArray();
        var originalItems=JsonNode.Parse(collisions)!["sources"]![3]!["settings"]!["items"]!.AsArray();
        Check(newAudio["name"]!.ToString()=="GD Sounds (OBS Jukebox) 3" && collisionSources.Select(s=>s!["name"]!.ToString()).Distinct().Count()==collisionSources.Count,"source names remain unique through repeated fallback collisions");
        Check(collisionItems.Count==4 && originalItems.Select((item,index)=>JsonNode.DeepEquals(item,collisionItems[index])).All(same=>same),"colliding scene visuals remain unchanged while audio is appended");
        var collisionAgain=Engine.IntegrateCollection(collisionResult,out bool collisionChangedAgain);
        Check(!collisionChangedAgain && collisionAgain.SequenceEqual(collisionResult),"collision integration remains idempotent");

        byte[] mismatchedNames=Encoding.UTF8.GetBytes("""
        {"sources":[
         {"id":"gd_alternate_song","name":"Custom Song","uuid":"audio-1","monitoring_type":0},
         {"id":"image_source","name":"Picture","uuid":"image-1"},
         {"id":"scene","name":"Game","settings":{"items":[
           {"name":"Custom Song","source_uuid":"image-1","id":1},
           {"name":"Custom Song","id":2},
           {"name":"Old audio name","source_uuid":"audio-1","id":3}],"id_counter":3}},
         {"id":"scene","name":"Legacy","settings":{"items":[{"name":"Custom Song","id":1}],"id_counter":1}},
         {"id":"scene","name":"Wrong UUID only","settings":{"items":[{"name":"Custom Song","source_uuid":"image-1","id":1}],"id_counter":1}}]}
        """);
        byte[] mismatchResult=Engine.IntegrateCollection(mismatchedNames,out _);
        var mismatchSources=JsonNode.Parse(mismatchResult)!["sources"]!.AsArray();
        var gameItems=mismatchSources[2]!["settings"]!["items"]!.AsArray();
        Check(gameItems.Count==3 && gameItems[0]!["source_uuid"]!.ToString()=="image-1" && gameItems[0]!["name"]!.ToString()=="Custom Song" && gameItems[1]!["source_uuid"]==null && gameItems[2]!["name"]!.ToString()=="GD Sounds","UUID match takes priority over stale names and legacy name matches");
        var legacyItems=mismatchSources[3]!["settings"]!["items"]!.AsArray();
        Check(legacyItems.Count==1 && legacyItems[0]!["source_uuid"]!.ToString()=="audio-1" && legacyItems[0]!["name"]!.ToString()=="GD Sounds","legacy item without UUID is migrated by its old name");
        var wrongUuidItems=mismatchSources[4]!["settings"]!["items"]!.AsArray();
        Check(wrongUuidItems.Count==2 && wrongUuidItems[0]!["source_uuid"]!.ToString()=="image-1" && wrongUuidItems[1]!["source_uuid"]!.ToString()=="audio-1","matching name with another UUID is preserved and audio is appended");
        var mismatchAgain=Engine.IntegrateCollection(mismatchResult,out bool mismatchChangedAgain);
        Check(!mismatchChangedAgain && mismatchAgain.SequenceEqual(mismatchResult),"renamed and legacy integration remains idempotent");
        var rewritten=JsonNode.Parse(integrated)!;
        rewritten["userLaterEdit"]=9001;
        rewritten["sources"]![1]!["volume"]=0.23;
        rewritten["sources"]!.AsArray().Last()!["obs_default"]=true;
        var undone=JsonNode.Parse(Engine.UndoCollection(original,integrated,Encoding.UTF8.GetBytes(rewritten.ToJsonString())))!;
        Check(undone["userLaterEdit"]!.GetValue<int>()==9001 && undone["sources"]![1]!["volume"]!.GetValue<double>()==0.23,"semantic undo preserves later user fields and source edits");
        Check(!undone["sources"]!.AsArray().Any(s=>s!["id"]?.ToString()=="gd_alternate_song") && undone["sources"]![0]!["settings"]!["items"]!.AsArray().Count==0,"semantic undo removes added audio source and scene items after OBS rewrite");
        var newScene=JsonNode.Parse(integrated)!;
        string addedUuid=newScene["sources"]!.AsArray().Last()!["uuid"]!.ToString();
        newScene["sources"]!.AsArray().Add(new JsonObject { ["id"]="scene",["name"]="Later user scene",["settings"]=new JsonObject { ["items"]=new JsonArray(new JsonObject { ["id"]=1,["source_uuid"]=addedUuid,["name"]="GD Sounds" }) } });
        var retained=JsonNode.Parse(Engine.UndoCollection(original,integrated,Encoding.UTF8.GetBytes(newScene.ToJsonString())))!;
        Check(retained["sources"]!.AsArray().Any(s=>s?["uuid"]?.ToString()==addedUuid) && retained["sources"]!.AsArray().Last()!["name"]!.ToString()=="Later user scene","semantic undo retains a source referenced by a later user-added scene");
        var migrated=JsonNode.Parse(mismatchResult)!;migrated["userLaterEdit"]=true;
        var migrationUndone=JsonNode.Parse(Engine.UndoCollection(mismatchedNames,mismatchResult,Encoding.UTF8.GetBytes(migrated.ToJsonString())))!;
        Check(JsonNode.DeepEquals(migrationUndone["sources"],JsonNode.Parse(mismatchedNames)!["sources"]),"semantic undo restores legacy UUID and name migrations without deleting existing items");
        Check(Engine.SceneNumber(JsonNode.Parse("2147483648"))==2147483648 && Engine.SceneNumber(JsonNode.Parse("5.0"))==5 && Engine.SceneNumber(JsonNode.Parse("\"invalid\""))==0,"scene counters tolerate large integers integral doubles and malformed types");
        var large=Encoding.UTF8.GetBytes("""{"sources":[{"id":"scene","name":"日本語 + <scene>","settings":{"items":[{"id":2147483648,"name":"existing"}],"id_counter":2147483648}}]}""");
        var largeResult=Engine.IntegrateCollection(large,out _);
        Check(JsonNode.Parse(largeResult)!["sources"]![0]!["settings"]!["id_counter"]!.GetValue<long>()==2147483649,"scene item IDs support values above int32");
        Check(Encoding.UTF8.GetString(largeResult).Contains("日本語 + <scene>"),"scene JSON preserves readable Unicode and punctuation");
        var unnamed=Engine.IntegrateCollection(Encoding.UTF8.GetBytes("""{"sources":[{"id":"gd_alternate_song"},{"id":"scene","name":"Game","settings":{"items":[]}}]}"""),out _);
        Check(JsonNode.Parse(unnamed)!["sources"]![0]!["name"]!.ToString()=="GD Sounds","unnamed existing audio source receives a safe name");
        string fixture=Path.Combine(Path.GetTempPath(),"obs-jukebox-test-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(fixture);
        try
        {
            string scenePath=Path.Combine(fixture,"scene.json"),backup=Path.Combine(fixture,"scene.bak"),plugin=Path.Combine(fixture,"plugin.dll");
            File.WriteAllBytes(backup,original);File.WriteAllText(scenePath,rewritten.ToJsonString());File.WriteAllText(plugin,"user replacement");
            var record=new InstallRecord { Files=[
                new ChangedFile { Destination=scenePath,Backup=backup,OriginalHash=Engine.Hash(original),InstalledHash=Engine.Hash(integrated),InstalledScene=integrated,Applied=true },
                new ChangedFile { Destination=plugin,InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("installed plugin")),Applied=true }] };
            Engine.Restore(record,_=>{});
            Check(JsonNode.DeepEquals(JsonNode.Parse(File.ReadAllBytes(scenePath)),undone) && File.ReadAllText(plugin)=="user replacement","transaction restore semantically undoes scenes and preserves replaced plugin bytes");
        }
        finally { Directory.Delete(fixture,true); }
        string manifests=Path.Combine(Path.GetTempPath(),"obs-jukebox-manifests-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(manifests);
        try
        {
            bool undoable(string product)
            {
                string path=Path.Combine(manifests,Guid.NewGuid().ToString("N")+".json");
                File.WriteAllText(path,$$"""{"Product":"{{product}}","Status":"installed","Files":[]}""");
                return Engine.CanUndo(new Options { Manifest=path });
            }
            Check(undoable("OBS Jukebox 1.2.1") && undoable("OBS Jukebox 1.1.0") && undoable("Separate Song 1.0.0"),"installs from earlier releases can be undone");
            Check(!undoable("Something Else 1.0.0"),"other products' manifests are not undone");
            string noProduct=Path.Combine(manifests,"no-product.json");
            File.WriteAllText(noProduct,"""{"Product":null,"Status":"installed","Files":[]}""");
            Check(!Engine.CanUndo(new Options { Manifest=noProduct }),"a manifest with no product name is not undone");
        }
        finally { Directory.Delete(manifests,true); }
        string interrupted=Path.Combine(Path.GetTempPath(),"obs-jukebox-interrupted-"+Guid.NewGuid().ToString("N"));
        string session=Path.Combine(interrupted,"20260101-000000-abcdef");
        Directory.CreateDirectory(session);
        try
        {
            string modFile=Path.Combine(interrupted,"mod.geode"),saved=Path.Combine(session,"0000.bak");
            File.WriteAllText(saved,"previous mod");File.WriteAllText(modFile,"half-installed mod");
            var stranded=new InstallRecord { GD=interrupted,OBS=interrupted,Files=[
                new ChangedFile { Destination=modFile,Backup=saved,OriginalHash=Engine.Hash(Encoding.UTF8.GetBytes("previous mod")),InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("half-installed mod")),Applied=true }] };
            string manifest=Path.Combine(session,"manifest.json");
            File.WriteAllText(manifest,System.Text.Json.JsonSerializer.Serialize(stranded));
            // Recovery may need to close OBS, which a mod-only update promises not to do.
            Engine.RecoverInterrupted(new Options { StateRoot=interrupted,GD=interrupted,OBS=interrupted,ModOnly=true },_=>{},[]);
            Check(File.ReadAllText(modFile)=="half-installed mod","a mod-only update leaves an interrupted install for full setup");
            Engine.RecoverInterrupted(new Options { StateRoot=interrupted,GD=interrupted,OBS=interrupted },_=>{},[]);
            Check(File.ReadAllText(modFile)=="previous mod" && File.ReadAllText(manifest).Contains("rolled-back"),"a transaction left installing by a killed setup is restored on the next run");
            // Setup can be killed after replacing a file but before recording that it did.
            string unrecorded=Path.Combine(interrupted,"20260101-000001-abcdef");Directory.CreateDirectory(unrecorded);
            File.WriteAllText(Path.Combine(unrecorded,"0000.bak"),"previous mod");File.WriteAllText(modFile,"half-installed mod");
            File.WriteAllText(Path.Combine(unrecorded,"manifest.json"),System.Text.Json.JsonSerializer.Serialize(new InstallRecord { GD=interrupted,OBS=interrupted,Files=[
                new ChangedFile { Destination=modFile,Backup=Path.Combine(unrecorded,"0000.bak"),OriginalHash=Engine.Hash(Encoding.UTF8.GetBytes("previous mod")),InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("half-installed mod")) }] }));
            Engine.RecoverInterrupted(new Options { StateRoot=interrupted,GD=interrupted,OBS=interrupted },_=>{},[]);
            Check(File.ReadAllText(modFile)=="previous mod","recovery restores a file replaced just before setup was killed");
            // A destination recovery can't read must not stop it restoring the other files.
            string partial=Path.Combine(interrupted,"20260102-000000-abcdef");Directory.CreateDirectory(partial);
            string applied=Path.Combine(interrupted,"applied.dll"),locked=Path.Combine(interrupted,"locked.dll");
            File.WriteAllText(Path.Combine(partial,"0000.bak"),"previous plugin");File.WriteAllText(applied,"installed plugin");File.WriteAllText(locked,"previous scene");
            File.WriteAllText(Path.Combine(partial,"manifest.json"),System.Text.Json.JsonSerializer.Serialize(new InstallRecord { GD=interrupted,OBS=interrupted,Files=[
                new ChangedFile { Destination=applied,Backup=Path.Combine(partial,"0000.bak"),OriginalHash=Engine.Hash(Encoding.UTF8.GetBytes("previous plugin")),InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("installed plugin")),Applied=true },
                new ChangedFile { Destination=locked,InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("installed scene")) }] }));
            using(new FileStream(locked,FileMode.Open,FileAccess.Read,FileShare.None))
                Engine.RecoverInterrupted(new Options { StateRoot=interrupted,GD=interrupted,OBS=interrupted },_=>{},[]);
            Check(File.ReadAllText(applied)=="previous plugin" && File.ReadAllText(Path.Combine(partial,"manifest.json")).Contains("rollback-incomplete"),"recovery restores the files it can when one destination is locked");
            // Recovery would roll back another setup's live transaction, so only one setup runs at a time.
            using(var held=new ManualResetEventSlim()) using(var release=new ManualResetEventSlim())
            {
                var other=new Thread(()=>{ using var running=new Mutex(false,Engine.SetupLockName); running.WaitOne(); held.Set(); release.Wait(); running.ReleaseMutex(); });
                other.Start(); held.Wait();
                bool refused=false;
                try { Engine.Execute(new Options { DryRun=true,StateRoot=interrupted,GD=interrupted,OBS=interrupted },_=>{}); }
                catch(SetupBusyException) { refused=true; }
                release.Set(); other.Join();
                Check(refused,"setup refuses to run while another setup is running");
            }
            // A recovery that fails, or a manifest it can't read, must not stop every later install.
            var recover=()=>Engine.RecoverInterrupted(new Options { StateRoot=interrupted,GD=interrupted,OBS=interrupted },_=>{},[]);
            string lostSession=Path.Combine(interrupted,"20260103-000000-abcdef");Directory.CreateDirectory(lostSession);
            string lostManifest=Path.Combine(lostSession,"manifest.json");
            File.WriteAllText(lostManifest,System.Text.Json.JsonSerializer.Serialize(new InstallRecord { GD=interrupted,OBS=interrupted,Files=[
                new ChangedFile { Destination=modFile,Backup=Path.Combine(lostSession,"missing.bak"),OriginalHash="00",InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("previous mod")),Applied=true }] }));
            string nullSession=Path.Combine(interrupted,"20260104-000000-abcdef");Directory.CreateDirectory(nullSession);
            File.WriteAllText(Path.Combine(nullSession,"manifest.json"),System.Text.Json.JsonSerializer.Serialize(new InstallRecord { GD=null!,OBS=null!,Files=[] }));
            bool recovered=true;
            try { recover(); recover(); }
            catch(Exception) { recovered=false; }
            Check(recovered && File.ReadAllText(lostManifest).Contains("rollback-incomplete"),"a recovery that can't finish, or a manifest with missing fields, doesn't block later installs");
        }
        finally { Directory.Delete(interrupted,true); }
        string gd=Path.Combine(Path.GetTempPath(),"obs-jukebox-mods-"+Guid.NewGuid().ToString("N"));
        string mods=Path.Combine(gd,"geode","mods");
        Directory.CreateDirectory(mods);
        try
        {
            WriteMod(Path.Combine(mods,"my-obs-jukebox.geode"),"local.separate_song","1.2.2","local.separate_song.dll");
            Check(Engine.FindInstalledMod(gd)?.Path==Path.Combine(mods,"my-obs-jukebox.geode") && Engine.InstalledVersion(gd)=="1.2.2","a renamed OBS Jukebox package is found by its ID");
            WriteMod(Path.Combine(mods,"local.separate_song.geode"),"local.separate_song","1.2.3","local.separate_song.dll");
            bool refused=false; try { Engine.FindInstalledMod(gd); } catch(IOException) { refused=true; }
            Check(refused,"two OBS Jukebox packages are refused");
            WriteMod(Path.Combine(mods,"jukebox.geode"),"fleym.nongd","3.8.0","other.dll");
            Check(Engine.FindJukebox(gd)?.Version=="without Windows support","a Jukebox package without fleym.nongd.dll is not counted as Windows support");
            WriteMod(Path.Combine(mods,"jukebox-copy.geode"),"fleym.nongd","3.8.0","fleym.nongd.dll");
            refused=false; try { Engine.FindJukebox(gd); } catch(IOException) { refused=true; }
            Check(refused,"two Jukebox packages are refused");
        }
        finally { Directory.Delete(gd,true); }
        string rollback=Path.Combine(Path.GetTempPath(),"obs-jukebox-rollback-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(rollback);
        try
        {
            string added=Path.Combine(rollback,"added.dll"),replaced=Path.Combine(rollback,"replaced.dll");
            File.WriteAllText(added,"installed");File.WriteAllText(replaced,"installed");
            string installedHash=Engine.Hash(Encoding.UTF8.GetBytes("installed"));
            var broken=new InstallRecord { Files=[
                new ChangedFile { Destination=added,InstalledHash=installedHash,Applied=true },
                new ChangedFile { Destination=replaced,Backup=Path.Combine(rollback,"missing.bak"),OriginalHash="00",InstalledHash=installedHash,Applied=true }] };
            bool reported=false; try { Engine.Restore(broken,_=>{}); } catch(IOException) { reported=true; }
            Check(reported && !File.Exists(added),"a missing backup is reported after the rest of the transaction is restored");
            File.WriteAllText(added,"installed");
            string locked=Path.Combine(rollback,"locked.dll"),lockedBackup=Path.Combine(rollback,"locked.bak");
            File.WriteAllText(locked,"installed");File.WriteAllText(lockedBackup,"original");
            var blocked=new InstallRecord { Files=[
                new ChangedFile { Destination=added,InstalledHash=installedHash,Applied=true },
                new ChangedFile { Destination=locked,Backup=lockedBackup,OriginalHash=Engine.Hash(Encoding.UTF8.GetBytes("original")),InstalledHash=installedHash,Applied=true }] };
            reported=false;
            using(new FileStream(locked,FileMode.Open,FileAccess.Read,FileShare.None)) { try { Engine.Restore(blocked,_=>{}); } catch(IOException) { reported=true; } }
            Check(reported && !File.Exists(added),"a file that can't be written is reported after the rest of the transaction is restored");
        }
        finally { Directory.Delete(rollback,true); }
        Check(Engine.IsNewer("1.2.10","1.2.9") && Engine.IsNewer("v2.0.0","1.9.9"),"a higher installed release counts as newer");
        Check(!Engine.IsNewer("1.2.9","1.2.9") && !Engine.IsNewer(null,"1.2.9") && !Engine.IsNewer("1.2.10-beta.1","1.2.9") && !Engine.IsNewer("1.2.3.4","1.2.3") && !Engine.IsNewer("1.2","1.2.9"),"equal, missing, malformed and prerelease versions don't count as newer");
        string repairRoot=Path.Combine(Path.GetTempPath(),"obs-jukebox-repair-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(repairRoot);
        try
        {
            string target=Path.Combine(repairRoot,"mod.geode");File.WriteAllText(target,"installed");
            var unrestorable=new InstallRecord { Files=[ new ChangedFile { Destination=target,Backup=Path.Combine(repairRoot,"missing.bak"),OriginalHash="00",InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("installed")),Applied=true } ] };
            var error=new IOException("write failed");
            Engine.RollBack(unrestorable,error,_=>{});
            Check(error.Data["repair"] is true,"a rollback that can't finish marks the error for repair");
            var clean=new IOException("write failed");
            Engine.RollBack(new InstallRecord(),clean,_=>{});
            Check(clean.Data["repair"]==null,"a finished rollback doesn't mark the error");
            File.WriteAllText(target,"installed");
            var unlogged=new IOException("write failed");
            try { Engine.RollBack(unrestorable,unlogged,line=>{ if(line.StartsWith("Rollback incomplete")) throw new IOException("log is full"); }); } catch(IOException) { }
            Check(unlogged.Data["repair"] is true,"the repair mark survives a log write that fails after it");
            // A log that fails on every line must not stop a restore that can finish.
            string restorable=Path.Combine(repairRoot,"restorable.geode"),restorableBackup=Path.Combine(repairRoot,"restorable.bak");
            File.WriteAllText(restorable,"installed");File.WriteAllText(restorableBackup,"original");
            var restorableRecord=new InstallRecord { Files=[ new ChangedFile { Destination=restorable,Backup=restorableBackup,OriginalHash=Engine.Hash(Encoding.UTF8.GetBytes("original")),InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("installed")),Applied=true } ] };
            var silenced=new IOException("write failed");
            Engine.RollBack(restorableRecord,silenced,Engine.QuietLog(_=>throw new IOException("log is full")));
            Check(File.ReadAllText(restorable)=="original" && silenced.Data["repair"]==null,"a failing log doesn't stop the restore or mark repair");
            // Recovery must wait, not give up, when an app opens just before it restores anything.
            string obsFolder=Path.Combine(repairRoot,"obs"),repairSession=Path.Combine(repairRoot,"state","20260101-000000-abcdef");
            Directory.CreateDirectory(Path.Combine(obsFolder,"bin","64bit"));Directory.CreateDirectory(repairSession);
            string plugin=Path.Combine(obsFolder,"plugin.dll");File.WriteAllText(plugin,"half-installed");
            string manifest=Path.Combine(repairSession,"manifest.json");
            File.WriteAllText(manifest,System.Text.Json.JsonSerializer.Serialize(new InstallRecord { GD=Path.Combine(repairRoot,"gd"),OBS=obsFolder,Files=[
                new ChangedFile { Destination=plugin,InstalledHash=Engine.Hash(Encoding.UTF8.GetBytes("half-installed")),Applied=true }] }));
            using var fakeObs=FakeApp(repairRoot,"obs64");
            try
            {
                bool waited=false;
                try { Engine.RecoverInterrupted(new Options { Update=true,StateRoot=Path.Combine(repairRoot,"state"),GD=Path.Combine(repairRoot,"gd"),OBS=obsFolder },_=>{},[]); }
                catch(AppRunningException) { waited=true; }
                Check(waited && File.ReadAllText(manifest).Contains("\"installing\"") && File.ReadAllText(plugin)=="half-installed","recovery leaves a session for later when an app is open in update mode");
            }
            finally { fakeObs.Kill(); fakeObs.WaitForExit(); }
            // A newer mod installed by hand: the update stops before recovery touches anything.
            string newerGd=Path.Combine(repairRoot,"gd-newer");
            Directory.CreateDirectory(Path.Combine(newerGd,"geode","mods"));
            WriteMod(Path.Combine(newerGd,"geode","mods","local.separate_song.geode"),"local.separate_song","999.0.0","local.separate_song.dll");
            bool stopped=false;
            try { Engine.Execute(new Options { Update=true,StateRoot=Path.Combine(repairRoot,"state"),GD=newerGd,OBS=obsFolder },_=>{}); }
            catch(NewerVersionInstalledException) { stopped=true; }
            Check(stopped && File.ReadAllText(manifest).Contains("\"installing\"") && File.ReadAllText(plugin)=="half-installed","a newer installed mod stops the update before recovery runs");
        }
        finally { Directory.Delete(repairRoot,true); }
        string updates=Path.Combine(Path.GetTempPath(),"obs-jukebox-update-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(updates);
        try
        {
            string result=Path.Combine(updates,"result.json"),flag=Path.Combine(updates,"reopen");
            var o=new Options { Update=true,ResultPath=result,ReopenFlag=flag };
            var events=new List<string>();
            Check(UpdateMode.Run(o,FakeHost(events,[(true,false),(false,true),(false,false)],()=>{}))==0 && events.SequenceEqual(["sleep","sleep","install"]) && ReadResult(result)["status"]!.ToString()=="installed","an update waits for GD and OBS, then installs");
            Check(!events.Contains("start-gd"),"without the reopen flag, GD stays closed");
            File.WriteAllText(flag,"");events.Clear();
            UpdateMode.Run(o,FakeHost(events,[(false,false)],()=>{}));
            Check(events.SequenceEqual(["install","start-gd"]) && !File.Exists(flag),"the reopen flag starts GD after the install, once");
            File.WriteAllText(flag,"");events.Clear();
            UpdateMode.Run(o,FakeHost(events,[(false,true),(false,true),(false,false)],()=>{}));
            Check(events.SequenceEqual(["start-gd","sleep","sleep","install"]),"OBS still open after GD exits: GD comes back at once and the later install leaves it closed");
            events.Clear();int tries=0;
            UpdateMode.Run(o,FakeHost(events,[(false,false)],()=>{ if(++tries==1) throw new AppRunningException("GD opened"); }));
            Check(events.SequenceEqual(["install","sleep","install"]) && ReadResult(result)["status"]!.ToString()=="installed","an app opening at the last moment sends the update back to waiting");
            tries=0;events.Clear();
            UpdateMode.Run(o,FakeHost(events,[(false,false)],()=>{ if(++tries==1) throw new SetupBusyException("busy"); }));
            Check(events.Count(e=>e=="install")==2,"a setup opened by hand makes the update wait, not fail");
            UpdateMode.Run(o,FakeHost([],[(false,false)],()=>throw new IOException("Plugin copy",new System.ComponentModel.Win32Exception(1223))));
            Check(ReadResult(result)["status"]!.ToString()=="declined" && ReadResult(result)["repair"]==null,"a cancelled UAC prompt gives declined");
            var unfinished=new IOException("copy failed",new System.ComponentModel.Win32Exception(1223));unfinished.Data["repair"]=true;
            UpdateMode.Run(o,FakeHost([],[(false,false)],()=>throw unfinished));
            Check(ReadResult(result)["status"]!.ToString()=="failed" && ReadResult(result)["repair"]!.GetValue<bool>(),"an unfinished rollback gives repair, even after a cancelled UAC prompt");
            UpdateMode.Run(o,FakeHost([],[(false,false)],()=>throw new IOException("disk full")));
            Check(ReadResult(result)["status"]!.ToString()=="failed" && ReadResult(result)["message"]!.ToString()=="disk full","any other error gives failed with its message");
            File.Delete(result);
            Check(UpdateMode.Run(o,FakeHost([],[(false,false)],()=>{},installed:"999.0.0"))==0 && !File.Exists(result),"a newer installed mod stops the update before it writes anything");
            UpdateMode.Run(o,FakeHost([],[(false,false)],()=>throw new NewerVersionInstalledException()));
            Check(ReadResult(result)["status"]!.ToString()=="waiting","a newer mod installed during the wait stops the update without a final result");
            File.Delete(result);
            Check(UpdateMode.Main(["--update","--result",result,"--bogus"])==1 && ReadResult(result)["status"]!.ToString()=="failed","a parsing error in update mode reaches the result file without a dialog");
            Check(UpdateMode.Main(["--update","--result",Path.Combine(updates,"missing-folder","r.json"),"--bogus"])==1,"an unwritable result path doesn't throw out of update mode");
            var parsed=Options.Parse(["--update","--close-apps","--restart","--integrate","--result",result,"--reopen-flag",flag]);
            Check(parsed.Update && parsed.Silent && !parsed.CloseApps && !parsed.Restart && !parsed.CreateScene && parsed.ResultPath==result && parsed.ReopenFlag==flag,"update mode never closes or reopens apps by itself and never edits scenes");
            string state=Path.Combine(updates,"state"),recordedObs=Path.Combine(updates,"OBS Studio");
            Directory.CreateDirectory(Path.Combine(recordedObs,"bin","64bit"));File.WriteAllText(Path.Combine(recordedObs,"bin","64bit","obs64.exe"),"");
            Directory.CreateDirectory(state);string record=Path.Combine(state,"manifest.json");
            File.WriteAllText(record,$$"""{"Product":"OBS Jukebox 1.2.3","OBS":{{System.Text.Json.JsonSerializer.Serialize(recordedObs)}},"Status":"installed","Files":[]}""");
            File.WriteAllText(Path.Combine(state,"latest-manifest.txt"),record);
            Check(Engine.RecordedOBS(state)==recordedObs,"update mode finds OBS through the last install record");
            File.Delete(Path.Combine(recordedObs,"bin","64bit","obs64.exe"));
            Check(Engine.RecordedOBS(state)==null,"a recorded OBS folder without obs64.exe is ignored");
        }
        finally { Directory.Delete(updates,true); }
        Check(!new Options().CreateScene && Options.Parse(["--integrate"]).CreateScene,"scene edits require explicit opt-in");
        Check(Engine.ObsSetupMessage(new Options{CreateScene=true,ScenesFound=true}).StartsWith("OBS: GD Sounds is in your existing scenes.") &&
              Engine.ObsSetupMessage(new Options{CreateScene=true}).StartsWith("OBS: add Sources > GD Sounds once in OBS.") &&
              Engine.ObsSetupMessage(new Options()).StartsWith("OBS: add Sources > GD Sounds once in OBS."),"the OBS step in the success log matches what setup did to the scenes");
        var liveOptions=Options.Parse(["--mod-only"]);
        Check(liveOptions.ModOnly,"mod-only update is available from the command line");
        liveOptions.GD=Path.Combine(Path.GetTempPath(),"GD");
        liveOptions.OBS=Path.Combine(Path.GetTempPath(),"OBS");
        string modDestination=Path.Combine(liveOptions.GD,"geode","mods","local.separate_song.geode");
        Check(Engine.AppsForChanges(liveOptions,[modDestination]).Select(a=>a.Name).SequenceEqual(["GeometryDash"]),"mod-only changes never request OBS to close");
        Check(Engine.AppsForChanges(liveOptions,[]).Count==0,"unchanged installation closes no apps");
        Check(Engine.AppsForChanges(liveOptions,[Path.Combine(liveOptions.OBS,"obs-plugins","64bit","separate-song.dll")]).Select(a=>a.Name).SequenceEqual(["obs64"]),"plugin-only changes close OBS without closing GD");
        Check(Engine.AppsForChanges(liveOptions,[modDestination,Path.Combine(liveOptions.SceneRoot,"scene.json")]).Count==2,"scene edits still require OBS to close");
        Check(Engine.AppsForChanges(liveOptions,[liveOptions.GD+"-other/plugin.dll"]).Single().Name=="obs64","path prefix collision is not treated as a GD-only change");
        Check(Engine.SuccessMessage(new Options { PreviousVersion="1.2.0" }).StartsWith($"Successfully updated to {Program.ReleaseVersion}."),"existing installation reports successful version update");
        Check(Engine.SuccessMessage(new Options()).StartsWith($"Successfully installed OBS Jukebox {Program.ReleaseVersion}."),"fresh installation reports install instead of update");
        Check(Engine.SuccessMessage(new Options { PreviousVersion=Program.ReleaseVersion,AlreadyCurrent=true }).Contains("already up to date"),"unchanged installation does not claim an update happened");
        Check(Engine.SuccessMessage(new Options { PreviousVersion="1.2.0",ModOnly=true }).Contains("OBS was kept running"),"mod-only success explains the deferred plugin update");
        bool oldOptionRejected=false;
        try { Options.Parse(["--keep-desktop-audio"]); } catch(ArgumentException) { oldOptionRejected=true; }
        Check(oldOptionRejected,"removed audio-prevention CLI switch is rejected");
    }
}
