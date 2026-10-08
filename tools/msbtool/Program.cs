// msbtool find <npcParamId> <msb...>: list enemy parts with that NpcParam (Elden Ring maps, via SoulsFormats MSBE)
using SoulsFormats;
if (args[0] == "find")
{
    int npc = int.Parse(args[1]);
    foreach (var path in args.Skip(2))
    {
        MSBE msb;
        try { msb = MSBE.Read(path); } catch (Exception e) { Console.WriteLine($"{Path.GetFileName(path)}: read failed {e.Message}"); continue; }
        foreach (var e in msb.Parts.Enemies.Where(e => e.NPCParamID == npc))
            Console.WriteLine($"{Path.GetFileName(path)}: {e.Name} model {e.ModelName} pos {e.Position} rot {e.Rotation} entity {e.EntityID} think {e.ThinkParamID}");
    }
}
if (args[0] == "list")
{
    // list <msb> <modelFilter>: enemy parts whose model contains the filter
    var msb = MSBE.Read(args[1]);
    foreach (var e in msb.Parts.Enemies.Where(e => e.ModelName.Contains(args[2])))
        Console.WriteLine($"{e.Name} model {e.ModelName} npc {e.NPCParamID} think {e.ThinkParamID} pos {e.Position} entity {e.EntityID}");
    Console.WriteLine($"models: {string.Join(" ", msb.Models.Enemies.Select(m => m.Name).Where(n => n.Contains(args[2])))}");
}
if (args[0] == "addball")
{
    // addball <in.msb> <out.msb> <templatePartName> <model> <npc> <think> <dx> <dy> <dz> <nearPartName>
    var ci = System.Globalization.CultureInfo.InvariantCulture;
    var msb = MSBE.Read(args[1]);
    var tpl = msb.Parts.Enemies.First(e => e.Name == args[3]);
    var near = msb.Parts.Enemies.First(e => e.Name == args[10]);
    string model = args[4];
    if (!msb.Models.Enemies.Any(m => m.Name == model))
    {
        var mt = (MSBE.Model.Enemy)msb.Models.Enemies.First().DeepCopy();
        mt.Name = model; mt.SibPath = $@"N:\GR\data\Model\chr\{model}\sib\{model}.sib";
        msb.Models.Enemies.Add(mt);
    }
    var ball = (MSBE.Part.Enemy)tpl.DeepCopy();
    ball.Name = $"{model}_9500";
    ball.ModelName = model;
    ball.NPCParamID = int.Parse(args[5]);
    ball.ThinkParamID = int.Parse(args[6]);
    ball.EntityID = 0;
    ball.Position = near.Position + new System.Numerics.Vector3(float.Parse(args[7], ci), float.Parse(args[8], ci), float.Parse(args[9], ci));
    msb.Parts.Enemies.Add(ball);
    Directory.CreateDirectory(Path.GetDirectoryName(args[2]));
    msb.Write(args[2]);
    Console.WriteLine($"added {ball.Name} at {ball.Position} (near {near.Name} at {near.Position})");
}
