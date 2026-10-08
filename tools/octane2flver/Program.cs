// Replaces the mesh of an Elden Ring character model (template chrbnd) with a model loaded from FBX.
// The new mesh is rigidly bound to one bone, keeping the template's skeleton, material and face-set flags.
// Usage: octane2flver <template.chrbnd.dcx> <model.fbx> <out.chrbnd.dcx> [lengthMeters]
using Assimp;
using SoulsFormats;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;

class Program
{
    static int Main(string[] args)
    {
        string templatePath = args[0], fbxPath = args[1], outPath = args[2];
        float targetLength = args.Length > 3 ? float.Parse(args[3]) : 0;

        var bnd = BND4.Read(templatePath);
        var flverFile = bnd.Files.First(f => f.Name.EndsWith(".flver", StringComparison.OrdinalIgnoreCase));
        var fl = FLVER2.Read(flverFile.Bytes);
        var template = fl.Meshes[0];
        Console.WriteLine($"template bbox {fl.Header.BoundingBoxMin} .. {fl.Header.BoundingBoxMax}, mesh verts {template.Vertices.Count}, facesets {template.FaceSets.Count}");

        var scene = new AssimpContext().ImportFile(fbxPath,
            PostProcessSteps.Triangulate | PostProcessSteps.JoinIdenticalVertices | PostProcessSteps.GenerateSmoothNormals |
            PostProcessSteps.CalculateTangentSpace | PostProcessSteps.PreTransformVertices | PostProcessSteps.FlipUVs);

        // Gather all triangles into one list (positions already in scene space after PreTransformVertices)
        var pos = new List<Vector3>(); var nrm = new List<Vector3>(); var tan = new List<Vector3>(); var uv = new List<Vector2>(); var idx = new List<int>();
        foreach (var m in scene.Meshes)
        {
            int baseIndex = pos.Count;
            for (int i = 0; i < m.VertexCount; i++)
            {
                var p = m.Vertices[i]; pos.Add(new Vector3(p.X, p.Y, p.Z));
                var n = m.HasNormals ? m.Normals[i] : new Vector3D(0, 1, 0); nrm.Add(new Vector3(n.X, n.Y, n.Z));
                var t = m.HasTangentBasis ? m.Tangents[i] : new Vector3D(1, 0, 0); tan.Add(new Vector3(t.X, t.Y, t.Z));
                var u = m.HasTextureCoords(0) ? m.TextureCoordinateChannels[0][i] : new Vector3D(); uv.Add(new Vector2(u.X, u.Y));
            }
            foreach (var f in m.Faces) if (f.IndexCount == 3) { idx.Add(baseIndex + f.Indices[0]); idx.Add(baseIndex + f.Indices[1]); idx.Add(baseIndex + f.Indices[2]); }
        }

        // Assimp hands back Y-up with the car's length on X. Elden Ring is Y-up and left-handed with forward on Z:
        // swapping X and Z both turns the car and mirrors it into left-handed space, so flip winding.
        Vector3 Conv(Vector3 v) => new Vector3(v.Z, v.Y, v.X);
        var P = pos.Select(Conv).ToList(); var N = nrm.Select(v => Vector3.Normalize(Conv(v))).ToList(); var T = tan.Select(v => Vector3.Normalize(Conv(v))).ToList();
        for (int i = 0; i < idx.Count; i += 3) (idx[i + 1], idx[i + 2]) = (idx[i + 2], idx[i + 1]);

        var min = new Vector3(P.Min(v => v.X), P.Min(v => v.Y), P.Min(v => v.Z));
        var max = new Vector3(P.Max(v => v.X), P.Max(v => v.Y), P.Max(v => v.Z));
        Console.WriteLine($"model bbox {min} .. {max}, verts {P.Count}, tris {idx.Count / 3}");
        var size = max - min;
        float scale = targetLength > 0 ? targetLength / Math.Max(size.X, size.Z) : 1;
        var offset = new Vector3((min.X + max.X) / 2, min.Y, (min.Z + max.Z) / 2);
        Console.WriteLine($"size W {size.X:F2} H {size.Y:F2} L {size.Z:F2}");
        P = P.Select(v => (v - offset) * scale).ToList();
        min = new Vector3(P.Min(v => v.X), P.Min(v => v.Y), P.Min(v => v.Z));
        max = new Vector3(P.Max(v => v.X), P.Max(v => v.Y), P.Max(v => v.Z));
        Console.WriteLine($"scaled bbox {min} .. {max} (scale {scale})");

        // Pick the template's skinned layout so the vertex carries bone indices/weights
        int layoutIndex = fl.BufferLayouts.FindIndex(l => l.Any(s => s.Semantic == FLVER.LayoutSemantic.BoneWeights));
        int uvCount = fl.BufferLayouts[layoutIndex].Count(s => s.Semantic == FLVER.LayoutSemantic.UV);
        // Bind to the bone that carries most of the original visible geometry: the game draws it and culls by its box
        int bone = fl.Meshes.SelectMany(m => m.Vertices)
            .Select(v => { int best = 0; for (int k = 1; k < 4; k++) if (v.BoneWeights[k] > v.BoneWeights[best]) best = k; return v.BoneIndices[best]; })
            .GroupBy(b => b).OrderByDescending(g => g.Count()).First().Key;
        // Optional 5th arg: bind to a named bone instead (e.g. a fixed root, so the car doesn't tilt with a swiveling part)
        if (args.Length > 4) bone = fl.Bones.FindIndex(b => b.Name == args[4]);
        template.DefaultBoneIndex = bone;

        var verts = new List<FLVER.Vertex>(P.Count);
        for (int i = 0; i < P.Count; i++)
        {
            var v = new FLVER.Vertex(uvCount * 2, 1, 1);
            v.Position = P[i];
            v.Normal = N[i];
            v.NormalW = 127;
            v.Tangents.Add(new Vector4(T[i], 1));
            v.BoneIndices[0] = bone;
            v.BoneWeights[0] = 1;
            v.Colors.Add(new FLVER.VertexColor(1f, 1f, 1f, 1f));
            for (int k = 0; k < uvCount * 2; k++) v.UVs.Add(new Vector3(uv[i], 0));
            verts.Add(v);
        }

        template.Vertices = verts;
        template.VertexBuffers = new List<FLVER2.VertexBuffer> { new FLVER2.VertexBuffer(layoutIndex) };
        foreach (var fs in template.FaceSets) { fs.Indices = new List<int>(idx); fs.TriangleStrip = false; }
        if (template.BoundingBox != null) { template.BoundingBox.Min = min; template.BoundingBox.Max = max; }
        fl.Meshes = new List<FLVER2.Mesh> { template }; // the car replaces every part of the original model
        fl.Header.BoundingBoxMin = Vector3.Min(fl.Header.BoundingBoxMin, min); fl.Header.BoundingBoxMax = Vector3.Max(fl.Header.BoundingBoxMax, max);
        Console.WriteLine($"bone {bone} box before {fl.Bones[bone].BoundingBoxMin} .. {fl.Bones[bone].BoundingBoxMax}");
        fl.Bones[bone].BoundingBoxMin = min; fl.Bones[bone].BoundingBoxMax = max;

        flverFile.Bytes = fl.Write();
        Directory.CreateDirectory(Path.GetDirectoryName(outPath));
        bnd.Write(outPath);
        Console.WriteLine($"wrote {outPath} ({new FileInfo(outPath).Length} bytes), layout {layoutIndex}, uvs {uvCount}, bone {bone} {fl.Bones[bone].Name}");
        return 0;
    }
}
