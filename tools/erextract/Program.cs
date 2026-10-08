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
            foreach (var d in fl.Dummies) Console.WriteLine($"dummy ref={d.ReferenceID} pos={d.Position} parent={(d.ParentBoneIndex >= 0 ? fl.Bones[d.ParentBoneIndex].Name : "-")} attach={(d.AttachBoneIndex >= 0 ? fl.Bones[d.AttachBoneIndex].Name : "-")}");
            return 0;
        }
        if (args.Length == 9 && args[0] == "adddummy")
        {
            // adddummy <in.chrbnd.dcx> <out.chrbnd.dcx> <templateRefId> <newRefId> <x> <y> <z> <fwdZ>:
            // copies a dummy (keeping its parent/attach setup) to a new position, facing +Z or -Z
            var ci = System.Globalization.CultureInfo.InvariantCulture;
            var bnd = BND4.Read(args[1]);
            var file = bnd.Files.First(f => f.Name.EndsWith(".flver", StringComparison.OrdinalIgnoreCase));
            var fl = FLVER2.Read(file.Bytes);
            var tpl = fl.Dummies.First(d => d.ReferenceID == short.Parse(args[3]));
            short newId = short.Parse(args[4]);
            fl.Dummies.RemoveAll(d => d.ReferenceID == newId);
            var nd = new FLVER.Dummy
            {
                Position = new System.Numerics.Vector3(float.Parse(args[5], ci), float.Parse(args[6], ci), float.Parse(args[7], ci)),
                Forward = new System.Numerics.Vector3(0, 0, float.Parse(args[8], ci)),
                Upward = new System.Numerics.Vector3(0, 1, 0),
                ReferenceID = newId,
                ParentBoneIndex = tpl.ParentBoneIndex,
                AttachBoneIndex = tpl.AttachBoneIndex,
                Flag1 = tpl.Flag1,
                UseUpwardVector = true,
                Color = tpl.Color,
            };
            fl.Dummies.Add(nd);
            Console.WriteLine($"added dummy {newId} at {nd.Position} (parent {tpl.ParentBoneIndex}, attach {tpl.AttachBoneIndex})");
            file.Bytes = fl.Write();
            Directory.CreateDirectory(Path.GetDirectoryName(args[2]));
            bnd.Write(args[2]);
            return 0;
        }
        if (args.Length == 5 && args[0] == "dummy")
        {
            // dummy <in.chrbnd.dcx> <out.chrbnd.dcx> <referenceId> <dy>: move every dummy with that reference ID vertically
            var bnd = BND4.Read(args[1]);
            var file = bnd.Files.First(f => f.Name.EndsWith(".flver", StringComparison.OrdinalIgnoreCase));
            var fl = FLVER2.Read(file.Bytes);
            short refId = short.Parse(args[3]); float dy = float.Parse(args[4], System.Globalization.CultureInfo.InvariantCulture);
            foreach (var d in fl.Dummies.Where(d => d.ReferenceID == refId))
            {
                var before = d.Position;
                d.Position += new System.Numerics.Vector3(0, dy, 0);
                Console.WriteLine($"dummy {refId}: {before} -> {d.Position}");
            }
            file.Bytes = fl.Write();
            Directory.CreateDirectory(Path.GetDirectoryName(args[2]));
            bnd.Write(args[2]);
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



