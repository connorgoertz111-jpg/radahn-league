// Replaces the mesh of an Elden Ring character model (template chrbnd) with a model loaded from FBX.
// Each FBX material becomes its own mesh + material (cloned from the template's first material), rigidly bound to one bone,
// keeping the template's skeleton and face-set flags. Optionally packs textures into a texbnd the game loads with the chr.
// Usage: octane2flver <template.chrbnd.dcx> <model.fbx> <out.chrbnd.dcx> <lengthMeters> [boneName|-]
//                     [--tex <templateTexbnd.dcx> <out_h.texbnd.dcx> <ddsDir>]
//   ddsDir holds <prefix>_<material>_a.dds (BC1 sRGB) per FBX material and <prefix>_flat_n.dds (BC7).
using Assimp;
using SoulsFormats;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;

class Program
{
    const string Prefix = "octane";

    static int Main(string[] args)
    {
        string templatePath = args[0], fbxPath = args[1], outPath = args[2];
        float targetLength = float.Parse(args[3]);
        string boneName = args.Length > 4 && args[4] != "-" ? args[4] : null;
        int texArg = Array.IndexOf(args, "--tex");

        var bnd = BND4.Read(templatePath);
        var flverFile = bnd.Files.First(f => f.Name.EndsWith(".flver", StringComparison.OrdinalIgnoreCase));
        string chrId = Path.GetFileNameWithoutExtension(flverFile.Name);
        var fl = FLVER2.Read(flverFile.Bytes);
        // --mat <name>: base the car's materials on a named template material (e.g. a plain shader instead of fur)
        int matArg = Array.IndexOf(args, "--mat");
        int templateMatIndex = matArg >= 0 ? fl.Materials.FindIndex(m => m.Name == args[matArg + 1]) : fl.Meshes[0].MaterialIndex;
        var template = fl.Meshes.First(m => m.MaterialIndex == templateMatIndex);
        var templateMat = fl.Materials[templateMatIndex];
        Console.WriteLine($"template material '{templateMat.Name}' {templateMat.MTD}");
        Console.WriteLine($"template {chrId} bbox {fl.Header.BoundingBoxMin} .. {fl.Header.BoundingBoxMax}, facesets {template.FaceSets.Count}");

        var scene = new AssimpContext().ImportFile(fbxPath,
            PostProcessSteps.Triangulate | PostProcessSteps.JoinIdenticalVertices | PostProcessSteps.GenerateSmoothNormals |
            PostProcessSteps.CalculateTangentSpace | PostProcessSteps.PreTransformVertices | PostProcessSteps.FlipUVs);

        // Assimp hands back Y-up with the car's length on X. Elden Ring is Y-up and left-handed with forward on Z:
        // swapping X and Z both turns the car and mirrors it into left-handed space, so flip winding.
        // --reverse turns the model 180° around the vertical axis (a rotation, so winding is unaffected)
        bool reverse = Array.IndexOf(args, "--reverse") >= 0;
        // --double-sided draws every face from both sides (the Octane model has open, one-sided panels)
        bool doubleSided = Array.IndexOf(args, "--double-sided") >= 0;
        Vector3 Conv(Vector3D v) => reverse ? new Vector3(-v.Z, v.Y, -v.X) : new Vector3(v.Z, v.Y, v.X);

        // Overall bounds for scaling/centering across every part
        var all = scene.Meshes.SelectMany(m => m.Vertices).Select(Conv).ToList();
        var min = new Vector3(all.Min(v => v.X), all.Min(v => v.Y), all.Min(v => v.Z));
        var max = new Vector3(all.Max(v => v.X), all.Max(v => v.Y), all.Max(v => v.Z));
        var size = max - min;
        float scale = targetLength / Math.Max(size.X, size.Z);
        var offset = new Vector3((min.X + max.X) / 2, min.Y, (min.Z + max.Z) / 2);
        Vector3 Place(Vector3D v) => (Conv(v) - offset) * scale;
        min = (min - offset) * scale; max = (max - offset) * scale;
        Console.WriteLine($"model size W {size.X:F2} H {size.Y:F2} L {size.Z:F2} -> scaled bbox {min} .. {max}");

        // Skinned layout so each vertex carries bone indices/weights
        int layoutIndex = fl.BufferLayouts.FindIndex(l => l.Any(s => s.Semantic == FLVER.LayoutSemantic.BoneWeights));
        int uvCount = fl.BufferLayouts[layoutIndex].Count(s => s.Semantic == FLVER.LayoutSemantic.UV);
        // Bind to the bone that carries most of the original geometry, or a named bone (e.g. a fixed base that doesn't swivel)
        int bone = boneName != null ? fl.Bones.FindIndex(b => b.Name == boneName) : fl.Meshes.SelectMany(m => m.Vertices)
            .Select(v => { int best = 0; for (int k = 1; k < 4; k++) if (v.BoneWeights[k] > v.BoneWeights[best]) best = k; return v.BoneIndices[best]; })
            .GroupBy(b => b).OrderByDescending(g => g.Count()).First().Key;

        string texDir = $@"N:\GR\data\INTERROOT_win64\chr\{chrId}\tex\";
        var meshes = new List<FLVER2.Mesh>();
        var materials = new List<FLVER2.Material>();
        foreach (var group in scene.Meshes.GroupBy(m => m.MaterialIndex))
        {
            string matName = scene.Materials[group.Key].Name.ToLowerInvariant();

            var mat = new FLVER2.Material($"{Prefix}_{matName}", templateMat.MTD, templateMat.Flags) { GXIndex = templateMat.GXIndex, Unk18 = templateMat.Unk18 };
            bool albedoSet = false, normalSet = false;
            foreach (var t in templateMat.Textures)
            {
                string path = "";
                if (!albedoSet && t.Type.Contains("AlbedoMap")) { path = texDir + $"{Prefix}_{matName}_a.tif"; albedoSet = true; }
                else if (!normalSet && t.Type.Contains("NormalMap")) { path = texDir + $"{Prefix}_flat_n.tif"; normalSet = true; }
                mat.Textures.Add(new FLVER2.Texture(t.Type, path, t.Scale, t.Unk10, t.Unk11, t.Unk14, t.Unk18, t.Unk1C));
            }
            materials.Add(mat);

            var verts = new List<FLVER.Vertex>();
            var idx = new List<int>();
            foreach (var m in group)
            {
                int baseIndex = verts.Count;
                for (int i = 0; i < m.VertexCount; i++)
                {
                    var v = new FLVER.Vertex(uvCount * 2, 1, 1);
                    v.Position = Place(m.Vertices[i]);
                    v.Normal = m.HasNormals ? Vector3.Normalize(Conv(m.Normals[i])) : Vector3.UnitY;
                    v.NormalW = 127;
                    v.Tangents.Add(new Vector4(m.HasTangentBasis ? Vector3.Normalize(Conv(m.Tangents[i])) : Vector3.UnitX, 1));
                    v.BoneIndices[0] = bone;
                    v.BoneWeights[0] = 1;
                    v.Colors.Add(new FLVER.VertexColor(1f, 1f, 1f, 1f));
                    var u = m.HasTextureCoords(0) ? m.TextureCoordinateChannels[0][i] : new Vector3D();
                    for (int k = 0; k < uvCount * 2; k++) v.UVs.Add(new Vector3(u.X, u.Y, 0));
                    verts.Add(v);
                }
                foreach (var f in m.Faces)
                    if (f.IndexCount == 3) { idx.Add(baseIndex + f.Indices[0]); idx.Add(baseIndex + f.Indices[2]); idx.Add(baseIndex + f.Indices[1]); }
            }

            var mesh = new FLVER2.Mesh
            {
                Dynamic = template.Dynamic,
                MaterialIndex = materials.Count - 1,
                DefaultBoneIndex = bone,
                Vertices = verts,
                VertexBuffers = new List<FLVER2.VertexBuffer> { new FLVER2.VertexBuffer(layoutIndex) },
                FaceSets = template.FaceSets.Select(fs => new FLVER2.FaceSet(fs.Flags, false, !doubleSided && fs.CullBackfaces, fs.Unk06, new List<int>(idx))).ToList(),
            };
            if (template.BoundingBox != null) mesh.BoundingBox = new FLVER2.Mesh.BoundingBoxes { Min = min, Max = max };
            meshes.Add(mesh);
            Console.WriteLine($"part '{matName}': {verts.Count} verts, {idx.Count / 3} tris");
        }

        fl.Meshes = meshes;
        fl.Materials = materials;
        fl.Header.BoundingBoxMin = Vector3.Min(fl.Header.BoundingBoxMin, min); fl.Header.BoundingBoxMax = Vector3.Max(fl.Header.BoundingBoxMax, max);
        // The game culls a bone's geometry by the bone's box; an empty (inverted) box means never drawn
        fl.Bones[bone].BoundingBoxMin = min; fl.Bones[bone].BoundingBoxMax = max;

        flverFile.Bytes = fl.Write();
        // Cloth simulation (e.g. Torrent's mane) targets meshes that no longer exist: drop it
        int dropped = bnd.Files.RemoveAll(f => f.Name.EndsWith("_c.hkx", StringComparison.OrdinalIgnoreCase) || f.Name.EndsWith(".clm2", StringComparison.OrdinalIgnoreCase));
        if (dropped > 0) Console.WriteLine($"dropped {dropped} cloth file(s)");
        Directory.CreateDirectory(Path.GetDirectoryName(outPath));
        bnd.Write(outPath);
        Console.WriteLine($"wrote {outPath} ({new FileInfo(outPath).Length} bytes), bone {bone} {fl.Bones[bone].Name}");

        if (texArg >= 0)
        {
            string texTemplate = args[texArg + 1], texOut = args[texArg + 2], ddsDir = args[texArg + 3];
            var texbnd = BND4.Read(texTemplate);
            var tpfFile = texbnd.Files.First(f => f.Name.EndsWith(".tpf", StringComparison.OrdinalIgnoreCase));
            var tpf = TPF.Read(tpfFile.Bytes);
            tpf.Textures.Clear();
            foreach (string dds in Directory.GetFiles(ddsDir, $"{Prefix}_*.dds"))
            {
                string name = Path.GetFileNameWithoutExtension(dds);
                byte format = (byte)(name.EndsWith("_n") ? 106 : 0); // as in shipped chr TPFs: BC7 normals, BC1 albedo
                tpf.Textures.Add(new TPF.Texture(name, format, 0, File.ReadAllBytes(dds)));
                Console.WriteLine($"texture {name} format {format}");
            }
            tpfFile.Bytes = tpf.Write();
            string tpfName = Path.GetFileName(texOut).Split('.')[0]; // e.g. c8002_l from c8002_l.texbnd.dcx
            tpfFile.Name = $@"N:\GR\data\INTERROOT_win64\chr\{chrId}\{tpfName}.tpf";
            texbnd.Files = new List<BinderFile> { tpfFile };
            texbnd.Write(texOut);
            Console.WriteLine($"wrote {texOut} ({new FileInfo(texOut).Length} bytes)");
        }
        return 0;
    }
}
