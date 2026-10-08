// Extracts selected files from the player's own Elden Ring archives (local development only).
// Usage: erextract <gameDir> <outDir> <path> [path...]   e.g. /chr/c8000.chrbnd.dcx
using SoulsFormats;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

class Program
{
    static readonly string[] Archives = { "Data0", "Data1", "Data2", "Data3", "DLC" };

    static ulong Hash(string path)
    {
        string h = path.Trim().Replace('\\', '/').ToLowerInvariant();
        if (!h.StartsWith("/")) h = "/" + h;
        return h.Aggregate(0ul, (i, c) => i * 0x85ul + c);
    }

    static int Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "ls")
        {
            var bnd = BND4.Read(args[1]);
            foreach (var f in bnd.Files) Console.WriteLine($"{f.ID}\t{f.Bytes.Length}\t{f.Name}");
            return 0;
        }
        if (args.Length == 3 && args[0] == "flver")
        {
            var bnd = BND4.Read(args[1]);
            var fl = FLVER2.Read(bnd.Files.First(f => f.Name.EndsWith(args[2], StringComparison.OrdinalIgnoreCase)).Bytes);
            Console.WriteLine($"bones {fl.Bones.Count}, meshes {fl.Meshes.Count}, materials {fl.Materials.Count}");
            foreach (var m in fl.Materials) Console.WriteLine($"mat {m.Name} | {m.MTD} | " + string.Join(", ", m.Textures.Select(t => $"{t.Type}={t.Path}")));
            foreach (var m in fl.Meshes) Console.WriteLine($"mesh mat={m.MaterialIndex} verts={m.Vertices.Count} defbone={m.DefaultBoneIndex} dyn={m.Dynamic} bones=[{string.Join(",", m.BoneIndices.Take(8))}]");
            for (int bi = 0; bi < fl.Bones.Count; bi++) { var b = fl.Bones[bi]; Console.WriteLine($"bone {bi} {b.Name} parent={b.ParentIndex} rot={b.Rotation} box={(b.BoundingBoxMin.X <= b.BoundingBoxMax.X ? "yes" : "empty")}"); }
            foreach (var l in fl.BufferLayouts) Console.WriteLine("layout " + string.Join(" ", l.Select(s => $"{s.Semantic}:{s.Type}")));
            return 0;
        }
        if (args.Length == 2 && args[0] == "tpf")
        {
            var tpf = TPF.Read(BND4.Read(args[1]).Files.First(f => f.Name.EndsWith(".tpf")).Bytes);
            Console.WriteLine($"platform {tpf.Platform} encoding {tpf.Encoding} flag2 {tpf.Flag2}");
            foreach (var t in tpf.Textures.Take(12)) Console.WriteLine($"{t.Name} format={t.Format} type={t.Type} mips={t.Mipmaps} flags1={t.Flags1} bytes={t.Bytes.Length} dds4={System.Text.Encoding.ASCII.GetString(t.Bytes, 84, 4)}");
            return 0;
        }
        if (args.Length == 3 && args[0] == "msb")
        {
            // No MSBE reader in this SoulsFormats build: list UTF-16 strings matching a regex instead
            byte[] raw = DCX.Decompress(File.ReadAllBytes(args[1])).ToArray();
            string text = Encoding.Unicode.GetString(raw, 0, raw.Length & ~1);
            var rx = new System.Text.RegularExpressions.Regex(args[2]);
            foreach (var g in rx.Matches(text).Select(m => m.Value).GroupBy(x => x).OrderBy(g => g.Key))
                Console.WriteLine($"{g.Key} x{g.Count()}");
            return 0;
        }
        if (args.Length < 3) { Console.Error.WriteLine("usage: erextract <gameDir> <outDir> <path>..."); return 2; }
        string gameDir = args[0], outDir = args[1];
        var wanted = args.Skip(2).ToDictionary(Hash, p => p);
        var found = new HashSet<ulong>();

        foreach (string archive in Archives)
        {
            string bhdPath = Path.Combine(gameDir, archive + ".bhd");
            string bdtPath = Path.Combine(gameDir, archive + ".bdt");
            if (!File.Exists(bhdPath)) continue;
            BHD5 bhd;
            using (var s = UXM.CryptographyUtility.DecryptRsa(bhdPath, UXM.ArchiveKeys.EldenRingKeys[archive]))
                bhd = BHD5.Read(s, BHD5.Game.EldenRing);
            Console.WriteLine($"loaded {archive}: {bhd.Buckets.Sum(b => b.Count)} files");
            using var bdt = File.OpenRead(bdtPath);
            foreach (var bucket in bhd.Buckets)
                foreach (var header in bucket)
                {
                    if (!wanted.TryGetValue(header.FileNameHash, out string path)) continue;
                    byte[] bytes = header.ReadFile(bdt);
                    string target = Path.Combine(outDir, path.TrimStart('/').Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    File.WriteAllBytes(target, bytes);
                    found.Add(header.FileNameHash);
                    Console.WriteLine($"{archive}: {path} ({bytes.Length} bytes)");
                }
        }
        foreach (var kv in wanted.Where(kv => !found.Contains(kv.Key)))
            Console.WriteLine($"NOT FOUND: {kv.Value}");
        return found.Count == wanted.Count ? 0 : 1;
    }
}



