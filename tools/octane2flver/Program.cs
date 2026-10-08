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

    // Debug: connected pieces of a mesh (by shared vertices) with their bounds, to locate the wheels
    static void ListPieces(FLVER2.Mesh mesh, string name)
    {
        int n = mesh.Vertices.Count;
        var parent = Enumerable.Range(0, n).ToArray();
        int Find(int a) { while (parent[a] != a) a = parent[a] = parent[parent[a]]; return a; }
        var idx = mesh.FaceSets[0].Indices;
        for (int t = 0; t + 2 < idx.Count; t += 3) { int a = Find(idx[t]), b = Find(idx[t + 1]), c = Find(idx[t + 2]); parent[b] = a; parent[Find(c)] = a; }
        // vertices that share a position also belong together (UV seams split them)
        var byPos = new Dictionary<(int, int, int), int>();
        for (int i = 0; i < n; i++)
        {
            var p = mesh.Vertices[i].Position; var key = ((int)MathF.Round(p.X * 1000), (int)MathF.Round(p.Y * 1000), (int)MathF.Round(p.Z * 1000));
            if (byPos.TryGetValue(key, out int other)) parent[Find(i)] = Find(other); else byPos[key] = i;
        }
        var groups = Enumerable.Range(0, n).GroupBy(Find).Where(g => g.Count() > 50).OrderByDescending(g => g.Count());
        foreach (var g in groups.Take(25))
        {
            var ps = g.Select(i => mesh.Vertices[i].Position).ToList();
            Console.WriteLine($"{name} piece: {g.Count(),5} verts  x {ps.Min(p => p.X),5:F2}..{ps.Max(p => p.X),5:F2}  y {ps.Min(p => p.Y),5:F2}..{ps.Max(p => p.Y),5:F2}  z {ps.Min(p => p.Z),5:F2}..{ps.Max(p => p.Z),5:F2}");
        }
    }

    static void AddCanopy(FLVER2.Mesh target, List<FLVER2.Mesh> all, Vector3 min, Vector3 max, float radius, int bone, int uvCount, string debugImage)
    {
        const float cell = 0.04f;
        int nx = (int)((max.X - min.X) / cell) + 1, nz = (int)((max.Z - min.Z) / cell) + 1;
        var h = new float[nx, nz];
        for (int i = 0; i < nx; i++) for (int j = 0; j < nz; j++) h[i, j] = float.NegativeInfinity;

        // Top surface: highest point of any triangle over each cell (sample triangles densely)
        foreach (var m in all)
        {
            var idx = m.FaceSets[0].Indices;
            for (int t = 0; t + 2 < idx.Count; t += 3)
            {
                Vector3 a = m.Vertices[idx[t]].Position, b = m.Vertices[idx[t + 1]].Position, c = m.Vertices[idx[t + 2]].Position;
                float longest = Math.Max(Vector3.Distance(a, b), Math.Max(Vector3.Distance(b, c), Vector3.Distance(a, c)));
                int steps = Math.Max(1, (int)(longest / (cell * 0.5f)));
                for (int u = 0; u <= steps; u++)
                    for (int v = 0; v <= steps - u; v++)
                    {
                        var p = a + (b - a) * (u / (float)steps) + (c - a) * (v / (float)steps);
                        int i = (int)((p.X - min.X) / cell), j = (int)((p.Z - min.Z) / cell);
                        if (i >= 0 && i < nx && j >= 0 && j < nz && p.Y > h[i, j]) h[i, j] = p.Y;
                    }
            }
        }

        // Morphological closing with a disc: dilate (max) then erode (min). Undefined cells stay undefined.
        int r = (int)(radius / cell);
        float[,] Filter(float[,] src, bool dilate)
        {
            var dst = new float[nx, nz];
            for (int i = 0; i < nx; i++)
                for (int j = 0; j < nz; j++)
                {
                    if (float.IsNegativeInfinity(h[i, j])) { dst[i, j] = float.NegativeInfinity; continue; }
                    float best = dilate ? float.NegativeInfinity : float.PositiveInfinity;
                    for (int di = -r; di <= r; di++)
                        for (int dj = -r; dj <= r; dj++)
                        {
                            if (di * di + dj * dj > r * r) continue;
                            int a = i + di, b = j + dj;
                            if (a < 0 || a >= nx || b < 0 || b >= nz || float.IsNegativeInfinity(src[a, b])) continue;
                            best = dilate ? Math.Max(best, src[a, b]) : Math.Min(best, src[a, b]);
                        }
                    dst[i, j] = best;
                }
            return dst;
        }
        // Centerline profile (debug): height along the car's length at x = 0
        int mid = (int)((0 - min.X) / cell);
        for (int j = 0; j < nz; j += 4)
        {
            var row = Enumerable.Range(-12, 25).Select(d => mid + d * 2).Where(i => i >= 0 && i < nx).Select(i => float.IsNegativeInfinity(h[i, j]) ? "  .  " : $"{h[i, j],5:F2}");
            Console.WriteLine($"z {min.Z + j * cell,6:F2}: {string.Join(" ", row)}");
        }
        var closed = Filter(Filter(h, true), false);

        // Lid cells: the closing raised the surface noticeably (a hollow such as the open cabin)
        var lid = new bool[nx, nz];
        int count = 0;
        for (int i = 0; i < nx; i++)
            for (int j = 0; j < nz; j++)
                if (!float.IsNegativeInfinity(h[i, j]) && closed[i, j] - h[i, j] > 0.08f) { lid[i, j] = true; count++; }
        Console.WriteLine($"canopy: grid {nx}x{nz}, radius {radius} m, {count} lid cells");

        // Debug image (top view): grey = car height, red = lid
        float lo = min.Y, hi = max.Y;
        using (var f = new StreamWriter(debugImage))
        {
            f.Write($"P3\n{nx} {nz}\n255\n");
            for (int j = nz - 1; j >= 0; j--)
            {
                for (int i = 0; i < nx; i++)
                {
                    int g = float.IsNegativeInfinity(h[i, j]) ? 0 : (int)(40 + 200 * (h[i, j] - lo) / (hi - lo));
                    f.Write(lid[i, j] ? $"230 40 40 " : $"{g} {g} {g} ");
                }
                f.Write('\n');
            }
        }

        // Lid mesh: one vertex per lid cell corner at the closed height, quads between lid cells
        var vertIndex = new Dictionary<(int, int), int>();
        int V(int i, int j)
        {
            if (vertIndex.TryGetValue((i, j), out int k)) return k;
            int ci = Math.Clamp(i, 0, nx - 1), cj = Math.Clamp(j, 0, nz - 1);
            float y = closed[ci, cj];
            var v = new FLVER.Vertex(uvCount * 2, 1, 1);
            v.Position = new Vector3(min.X + i * cell, y, min.Z + j * cell);
            v.Normal = Vector3.UnitY; v.NormalW = 127;
            v.Tangents.Add(new Vector4(1, 0, 0, 1));
            v.BoneIndices[0] = bone; v.BoneWeights[0] = 1;
            v.Colors.Add(new FLVER.VertexColor(1f, 1f, 1f, 1f));
            for (int q = 0; q < uvCount * 2; q++) v.UVs.Add(new Vector3(0.5f, 0.5f, 0));
            target.Vertices.Add(v);
            return vertIndex[(i, j)] = target.Vertices.Count - 1;
        }
        var lidIdx = new List<int>();
        for (int i = 0; i < nx - 1; i++)
            for (int j = 0; j < nz - 1; j++)
            {
                if (!lid[i, j]) continue;
                int a = V(i, j), b = V(i + 1, j), c = V(i + 1, j + 1), d = V(i, j + 1);
                lidIdx.AddRange(new[] { a, c, b, a, d, c });
            }
        foreach (var fs in target.FaceSets) fs.Indices.AddRange(lidIdx);
        Console.WriteLine($"canopy: added {vertIndex.Count} verts, {lidIdx.Count / 3} tris");
    }

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
        int bone = boneName != null ? (int.TryParse(boneName, out int boneNumber) ? boneNumber : fl.Bones.FindIndex(b => b.Name == boneName)) : fl.Meshes.SelectMany(m => m.Vertices)
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

        // --canopy <radiusMeters>: the model has an open cabin (no glass). Build a top-down height map of the whole car,
        // fill hollows with a morphological closing (dilate then erode), and add a lid wherever that raised the surface.
        int canopyArg = Array.IndexOf(args, "--canopy");
        if (canopyArg >= 0) AddCanopy(meshes[0], meshes, min, max, float.Parse(args[canopyArg + 1], System.Globalization.CultureInfo.InvariantCulture), bone, uvCount, Path.ChangeExtension(outPath, null) + "_canopy.ppm");

        if (Array.IndexOf(args, "--pieces") >= 0) for (int mi = 0; mi < meshes.Count; mi++) ListPieces(meshes[mi], materials[mi].Name);
        // Debug: rear-most geometry (exhaust pipes) in final car space
        foreach (var pm in meshes) { var rear = pm.Vertices.Where(v => v.Position.Z > max.Z - 0.7f && v.Position.Y > 0.3f && v.Position.Y < 1.1f && Math.Abs(v.Position.X) < 0.7f).ToList(); if (rear.Count > 0) foreach (var side in new[] { -1, 1 }) { var s = rear.Where(v => Math.Sign(v.Position.X) == side).ToList(); if (s.Count > 0) Console.WriteLine($"rear side {side}: {s.Count} verts, x {s.Min(v => v.Position.X):F2}..{s.Max(v => v.Position.X):F2} y {s.Min(v => v.Position.Y):F2}..{s.Max(v => v.Position.Y):F2} z {s.Min(v => v.Position.Z):F2}..{s.Max(v => v.Position.Z):F2}"); } }
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
