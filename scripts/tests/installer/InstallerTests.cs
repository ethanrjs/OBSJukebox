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
        Check(!options.Arguments().Contains("GD.exe") && !options.Arguments().Contains("OBS.exe"),"closed-app executable list is not sent to elevated child");
    }
}
