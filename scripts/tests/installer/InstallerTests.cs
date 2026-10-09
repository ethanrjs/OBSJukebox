using System.Text;
using System.Text.Json.Nodes;

namespace SeparateSongSetup;

static class InstallerTests
{
    static void Check(bool condition,string name)
    {
        if(!condition) throw new Exception(name);
        Console.WriteLine("PASS: "+name);
    }
    static async Task Main()
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
        Check(!new Options().CreateScene && Options.Parse(["--integrate"]).CreateScene,"scene edits require explicit opt-in");
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
        Check(Engine.SuccessMessage(new Options { PreviousVersion="1.2.0" }).StartsWith("Successfully updated to 1.2.1."),"existing installation reports successful version update");
        Check(Engine.SuccessMessage(new Options()).StartsWith("Successfully installed OBS Jukebox 1.2.1."),"fresh installation reports install instead of update");
        Check(Engine.SuccessMessage(new Options { PreviousVersion="1.2.1",AlreadyCurrent=true }).Contains("already up to date"),"unchanged installation does not claim an update happened");
        Check(Engine.SuccessMessage(new Options { PreviousVersion="1.2.0",ModOnly=true }).Contains("OBS was kept running"),"mod-only success explains the deferred plugin update");
        bool oldOptionRejected=false;
        try { Options.Parse(["--keep-desktop-audio"]); } catch(ArgumentException) { oldOptionRejected=true; }
        Check(oldOptionRejected,"removed audio-prevention CLI switch is rejected");

        var options=new Options { Restart=true };
        var order=new List<string>();
        await Engine.ExecuteWithRetry(options,stopped=>
        {
            stopped.Add("GD.exe"); stopped.Add("gd.EXE"); stopped.Add("OBS.exe");
            order.Add("closed"); throw new UnauthorizedAccessException();
        },()=>{order.Add("elevated-success");return Task.CompletedTask;},stopped=>order.AddRange(stopped));
        Check(order.SequenceEqual(new[]{"closed","elevated-success","GD.exe","OBS.exe"}),"parent keeps closed apps across elevated retry and restarts each once after success");

        bool restarted=false;
        try
        {
            await Engine.ExecuteWithRetry(options,stopped=>{stopped.Add("GD.exe");throw new UnauthorizedAccessException();},
                ()=>throw new IOException("elevated child failed or UAC cancelled"),_=>restarted=true);
            throw new Exception("Expected failed retry");
        }
        catch(IOException) { }
        Check(!restarted,"failed or cancelled elevation does not restart apps before successful installation");

        options.Restart=false;
        await Engine.ExecuteWithRetry(options,stopped=>{stopped.Add("GD.exe");throw new UnauthorizedAccessException();},()=>Task.CompletedTask,_=>restarted=true);
        Check(!restarted,"restart opt-out persists through elevation");

        options.Restart=true; bool retried=false; List<string>? restartedApps=null;
        await Engine.ExecuteWithRetry(options,stopped=>{stopped.Add("OBS.exe");return Task.CompletedTask;},()=>{retried=true;return Task.CompletedTask;},stopped=>restartedApps=stopped);
        Check(!retried && restartedApps!.SequenceEqual(new[]{"OBS.exe"}),"ordinary successful install restarts only apps it closed");
        Check(!options.Arguments().Contains("--restart"),"elevated arguments never forward restart");
        Check(!options.Arguments().Contains("GD.exe") && !options.Arguments().Contains("OBS.exe"),"closed-app executable list is not sent to elevated child");
    }
}
