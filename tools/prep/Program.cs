// prep - turns the Barrett M107A1 exported from SPT's WTT-Armory mod (glTF + PNGs, Escape from Tarkov's
// weapon rig) into the plugin's Assets\:
//   barrett.obj            frame (upper and lower receiver, stock), barrel, bipod, pistol grip, sights (static)
//   barrett_muzzle.obj     the muzzle brake (hidden while a muzzle device is mounted)
//   barrett_sights.obj     the flip-up iron sights, raised (hidden while an optic is on the top rail)
//   barrett_bolt.obj       bolt carrier with its charging handle                -> slides with the M110's carrier
//   barrett_trigger.obj / barrett_magrelease.obj
//   barrett_mag.obj        the 10-round magazine, where it sits seated in the gun
//   <Part>_{Diffuse,Normal,ORM}.png for the game's M_DefaultShader (Part = Frame1, Frame2, Barrel, Muzzle, Mag,
//   Grip, Sights, SightBase), at most kMaxSize[part] pixels a side
//
//   prep <export dir> <Assets dir> <m110 static points.obj> [preview dir]
//
// Space: the game's (X forward, Y right, Z up, cm). In the Tarkov weapon frame -Y is forward, +Z up and +X
// the gun's left, in metres, so UE = (-y, -x, z) * 100. That is a mirror (right-handed glTF to left-handed
// UE), which with GML's no-flip OBJ import keeps faces the right way out, as GML's Blender-OBJ path does.
//
// Fit: the Barrett's pistol grip is an AR-15 A2 grip, the same kind as on the game's KAC M110. A
// translation-only trimmed ICP of the Barrett's grip onto the M110 mesh puts the Barrett where the M110's
// hands are; the offset is baked into the OBJs. The M110's points come from its cooked static mesh
// (Utils\UAssetParser uexppos).
using System.Globalization;
using System.Numerics;
using System.Text;
using System.Text.Json;
using SixLabors.ImageSharp;
using SixLabors.ImageSharp.PixelFormats;
using SixLabors.ImageSharp.Processing;

CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
if (args.Length < 3) { Console.WriteLine("usage: prep <export dir> <Assets dir> <m110 points.obj> [preview dir]"); return 1; }
string src = args[0], outDir = args[1];
string? previewDir = args.Length > 3 ? args[3] : null;
Directory.CreateDirectory(outDir);

var weapon = Gltf.Load(Directory.GetFiles(src, "*Barrett M107A1*.gltf").Single());

// texture set of each glTF material
var partOfMaterial = new Dictionary<string, string> {
    ["m107_frame (Instance)"] = "Frame1", ["m107_frame"] = "Frame1",
    ["m107_frame 2 (Instance)"] = "Frame2", ["m107_frame 2"] = "Frame2", ["M107 Bipod (Instance)"] = "Frame2",
    ["m107_barrel (Instance)"] = "Barrel",
    ["m107_muzzle (Instance)"] = "Muzzle",
    ["mag_m82_barrett_m82a1_127x99_10_LOD0"] = "Mag",
    ["pistolgrip_ar15_colt_a2_LOD0"] = "Grip",
    ["m107_sights_up"] = "Sights",
    ["m107_rs_base"] = "SightBase",
};
// polymer parts are not metallic; the rest is steel and aluminium
var metalParts = new HashSet<string> { "Frame1", "Frame2", "Barrel", "Muzzle", "Mag", "Sights", "SightBase" };
// texture size cap per set: the export's 4096 maps are uncompressed once imported at runtime
var maxSize = new Dictionary<string, int> {
    ["Frame1"] = 2048, ["Frame2"] = 2048, ["Barrel"] = 2048, ["Muzzle"] = 1024, ["Mag"] = 1024, ["Grip"] = 512,
    ["Sights"] = 512, ["SightBase"] = 512,
};
// mesh node -> OBJ file
var fileOfNode = new Dictionary<string, string> {
    ["m107_frame"] = "barrett",
    ["m107_barrel"] = "barrett",
    ["m107_bipod"] = "barrett", ["m107_bipod_lleg"] = "barrett", ["m107_bipod_rleg"] = "barrett",
    ["pistolgrip_ar15_colt_a2_LOD0"] = "barrett",
    ["m107_rs_base"] = "barrett",
    ["m107_fs_up"] = "barrett_sights", ["m107_rs_up"] = "barrett_sights",  // the flip-up sights: hidden under an optic
    ["m107_muzzle"] = "barrett_muzzle",
    ["m107_bolt"] = "barrett_bolt",
    ["m107_trigger"] = "barrett_trigger",
    ["m107_release"] = "barrett_magrelease",
    ["mag_m82_barrett_m82a1_127x99_10_LOD0"] = "barrett_mag",
};

// ---- geometry, in UE space before the fit
var meshes = new Dictionary<string, Mesh>();
Mesh Out(string file) => meshes.TryGetValue(file, out var m) ? m : meshes[file] = new Mesh();
int weaponNode = weapon.FindNode("weapon");
var wGlobal = weapon.Globals(weaponNode);
var used = new HashSet<string>();
var nodeBounds = new Dictionary<string, (Vector3 lo, Vector3 hi)>();
foreach (var (node, m) in wGlobal) {
    string name = weapon.NodeName(node);
    if (!weapon.HasMesh(node) || !fileOfNode.TryGetValue(name, out var file)) continue;
    var target = Out(file);
    int before = target.V.Count;
    weapon.AddMesh(node, m, target, partOfMaterial);
    var added = target.V.Skip(before).ToArray();
    nodeBounds[name] = (added.Aggregate(new Vector3(1e9f), Vector3.Min), added.Aggregate(new Vector3(-1e9f), Vector3.Max));
    used.Add(name);
}
foreach (var k in fileOfNode.Keys.Where(k => !used.Contains(k))) Console.WriteLine($"! mesh node {k} not found");

Vector3 NodeUE(string name) => Gltf.ToUE(wGlobal[weapon.FindNode(name)].Translation);

// ---- fit: the Barrett's A2 grip onto the M110's
var m110 = File.ReadLines(args[2]).Where(l => l.StartsWith("v ")).Select(l => {
    var p = l.Split(' ', StringSplitOptions.RemoveEmptyEntries);
    return new Vector3(float.Parse(p[1]), float.Parse(p[2]), float.Parse(p[3]));
}).ToArray();
var grip = meshes["barrett"].PositionsOf("Grip");
// start: the grips' centres together (the M110's grip: the points behind and under its trigger guard)
var m110Grip = m110.Where(p => p.X > -15 && p.X < -2 && p.Z < -3.5f && p.Z > -13).ToArray();
var offset = Centre(m110Grip) - Centre(grip);
Console.WriteLine($"start offset (grip centres together): {V(offset)}; M110 grip region {m110Grip.Length} points, centre {V(Centre(m110Grip))}");
for (int it = 0; it < 200; it++) {
    var lo = grip.Aggregate(new Vector3(1e9f), (a, p) => Vector3.Min(a, p + offset)) - new Vector3(3);
    var hi = grip.Aggregate(new Vector3(-1e9f), (a, p) => Vector3.Max(a, p + offset)) + new Vector3(3);
    var target = m110.Where(p => p.X >= lo.X && p.Y >= lo.Y && p.Z >= lo.Z && p.X <= hi.X && p.Y <= hi.Y && p.Z <= hi.Z).ToArray();
    var d = grip.Select(p => { var q = p + offset; var t = target.MinBy(t => Vector3.DistanceSquared(t, q)); return t - q; })
                .OrderBy(v => v.LengthSquared()).Take(grip.Length * 7 / 10).ToArray();
    var step = new Vector3(Median(d.Select(v => v.X)), Median(d.Select(v => v.Y)), Median(d.Select(v => v.Z)));
    offset += step;
    if (step.Length() < 1e-4f || it == 199) {
        float rms = MathF.Sqrt(d.Average(v => v.LengthSquared()));
        Console.WriteLine($"fit after {it + 1} steps: offset {V(offset)}, rms {rms:F3} cm over {d.Length} of {grip.Length} grip vertices ({target.Length} M110 points)");
        break;
    }
}
foreach (var m in meshes.Values) m.Move(offset);

// ---- where the Barrett's parts are (actor space, cm), for the plugin's numbers
Console.WriteLine("\nBarrett points (actor space, cm, fitted):");
foreach (var n in new[] { "fireport", "mod_muzzle", "patron_in_weapon", "shellport", "mod_magazine", "weapon_trigger", "weapon_trigger_group",
                          "weapon_hammer", "weapon_release", "weapon_bolt_assembly", "weapon_bolt_carrier", "weapon_bolt", "mod_charge",
                          "weapon_spring_buffer_000", "mod_pistol_grip", "mod_reciever", "mod_scope", "mod_sight_rear", "mod_sight_front",
                          "mod_align_rear", "mod_align_front", "mod_aim_camera", "mod_barrel", "mod_bipod", "mod_tactical", "mod_tactical_001",
                          "mod_stock_000", "weapon_stock", "weapon_L_hand_marker", "weapon_R_hand_marker" })
    Console.WriteLine($"  {n,-26} {V(NodeUE(n) + offset)}");
foreach (var (file, m) in meshes.OrderBy(k => k.Key)) {
    var (lo, hi) = m.Bounds();
    Console.WriteLine($"  {file + ".obj",-26} X[{lo.X:F2}..{hi.X:F2}] Y[{lo.Y:F2}..{hi.Y:F2}] Z[{lo.Z:F2}..{hi.Z:F2}]  {m.V.Count} verts, {m.TriCount} tris");
}
Console.WriteLine("\nparts (fitted bounds, cm):");
foreach (var (name, (lo, hi)) in nodeBounds.OrderBy(k => k.Key))
    Console.WriteLine($"  {name,-40} X[{lo.X + offset.X:F2}..{hi.X + offset.X:F2}] Y[{lo.Y + offset.Y:F2}..{hi.Y + offset.Y:F2}] Z[{lo.Z + offset.Z:F2}..{hi.Z + offset.Z:F2}]");
{
    var knob = meshes["barrett_bolt"].V.Where(v => v.Y > 2.5f).ToArray();
    if (knob.Length > 0) {
        var lo = knob.Aggregate(new Vector3(1e9f), Vector3.Min); var hi = knob.Aggregate(new Vector3(-1e9f), Vector3.Max);
        Console.WriteLine($"  charging handle (carrier verts with Y > 2.5): X[{lo.X:F2}..{hi.X:F2}] Y[{lo.Y:F2}..{hi.Y:F2}] Z[{lo.Z:F2}..{hi.Z:F2}] centre {V((lo + hi) / 2)}");
    }
}

// ---- OBJs
foreach (var (file, m) in meshes) File.WriteAllText(Path.Combine(outDir, file + ".obj"), m.ToObj(file));

// ---- textures
var done = new HashSet<string>();
foreach (var mat in weapon.Materials()) {
    if (!partOfMaterial.TryGetValue(mat.Name, out var part) || !done.Add(part)) continue;
    Textures.Write(weapon.Dir, mat, part, metalParts.Contains(part), maxSize[part], outDir);
    Console.WriteLine($"textures: {part} <- {mat.Name}");
}

Render.Icon(Path.Combine(outDir, "Icon.png"), meshes.Select(m => m.Value), outDir);
if (previewDir != null) {
    Directory.CreateDirectory(previewDir);
    Preview.Draw(Path.Combine(previewDir, "fit.png"), m110, meshes);
    Render.Views(previewDir, meshes, outDir, m110);
}
Console.WriteLine("DONE");
return 0;

static float Median(IEnumerable<float> xs) { var a = xs.OrderBy(x => x).ToArray(); return a[a.Length / 2]; }
static Vector3 Centre(Vector3[] ps) => ps.Aggregate(Vector3.Zero, (a, p) => a + p) / ps.Length;
static string V(Vector3 v) => $"({v.X:F3}, {v.Y:F3}, {v.Z:F3})";

class Mesh {
    public List<Vector3> V = new(), N = new();
    public List<Vector2> T = new();
    public List<(string group, int a, int b, int c)> Tris = new();
    public int TriCount => Tris.Count;
    public Vector3[] PositionsOf(string group) => Tris.Where(t => t.group == group).SelectMany(t => new[] { t.a, t.b, t.c }).Distinct().Select(i => V[i]).ToArray();
    public void Move(Vector3 d) { for (int i = 0; i < V.Count; i++) V[i] += d; }
    public (Vector3, Vector3) Bounds() => (V.Aggregate(new Vector3(1e9f), Vector3.Min), V.Aggregate(new Vector3(-1e9f), Vector3.Max));
    public string ToObj(string name) {
        var sb = new StringBuilder($"# {name}: Barrett M107A1 (WTT-Armory export), Unreal space, cm\n");
        foreach (var v in V) sb.Append($"v {v.X:0.#####} {v.Y:0.#####} {v.Z:0.#####}\n");
        foreach (var t in T) sb.Append($"vt {t.X:0.######} {1 - t.Y:0.######}\n");  // OBJ V runs bottom-up, glTF's top-down
        foreach (var n in N) sb.Append($"vn {n.X:0.#####} {n.Y:0.#####} {n.Z:0.#####}\n");
        foreach (var grp in Tris.GroupBy(t => t.group)) {
            sb.Append($"usemtl {grp.Key}\n");
            foreach (var (_, a, b, c) in grp) sb.Append($"f {a + 1}/{a + 1}/{a + 1} {b + 1}/{b + 1}/{b + 1} {c + 1}/{c + 1}/{c + 1}\n");
        }
        return sb.ToString();
    }
}

record Material(string Name, string? BaseColor, Vector4 BaseColorFactor, string? MetalRough, string? Normal);

class Gltf {
    public JsonElement J;
    public string Dir = "";
    byte[][] buffers = [];

    public static Gltf Load(string path) {
        var g = new Gltf { J = JsonDocument.Parse(File.ReadAllText(path)).RootElement, Dir = Path.GetDirectoryName(path)! };
        g.buffers = g.J.GetProperty("buffers").EnumerateArray().Select(b => File.ReadAllBytes(Path.Combine(g.Dir, Uri.UnescapeDataString(b.GetProperty("uri").GetString()!)))).ToArray();
        return g;
    }
    JsonElement Node(int i) => J.GetProperty("nodes")[i];
    public string NodeName(int i) => Node(i).TryGetProperty("name", out var n) ? n.GetString()! : "";
    public bool HasMesh(int i) => Node(i).TryGetProperty("mesh", out _);
    public int FindNode(string name) {
        for (int i = 0; i < J.GetProperty("nodes").GetArrayLength(); i++) if (NodeName(i) == name) return i;
        throw new Exception($"node {name} not found");
    }
    Matrix4x4 Local(int i) {
        var n = Node(i);
        var t = n.TryGetProperty("translation", out var tt) ? new Vector3(F(tt[0]), F(tt[1]), F(tt[2])) : Vector3.Zero;
        var r = n.TryGetProperty("rotation", out var rr) ? new Quaternion(F(rr[0]), F(rr[1]), F(rr[2]), F(rr[3])) : Quaternion.Identity;
        var s = n.TryGetProperty("scale", out var ss) ? new Vector3(F(ss[0]), F(ss[1]), F(ss[2])) : Vector3.One;
        return Matrix4x4.CreateScale(s) * Matrix4x4.CreateFromQuaternion(r) * Matrix4x4.CreateTranslation(t);
    }
    static float F(JsonElement e) => e.GetSingle();
    // Every node under `root`, in root's frame (root's own transform left out).
    public Dictionary<int, Matrix4x4> Globals(int root) {
        var d = new Dictionary<int, Matrix4x4>();
        void Walk(int i, Matrix4x4 parent) {
            if (!Node(i).TryGetProperty("children", out var ch)) return;
            foreach (var c in ch.EnumerateArray()) { int k = c.GetInt32(); var m = Local(k) * parent; d[k] = m; Walk(k, m); }
        }
        d[root] = Matrix4x4.Identity;
        Walk(root, Matrix4x4.Identity);
        return d;
    }
    float[] Read(int accessor, out int comps) {
        var a = J.GetProperty("accessors")[accessor];
        var bv = J.GetProperty("bufferViews")[a.GetProperty("bufferView").GetInt32()];
        var buf = buffers[bv.TryGetProperty("buffer", out var b) ? b.GetInt32() : 0];
        int count = a.GetProperty("count").GetInt32(), ct = a.GetProperty("componentType").GetInt32();
        comps = a.GetProperty("type").GetString() switch { "SCALAR" => 1, "VEC2" => 2, "VEC3" => 3, "VEC4" => 4, var x => throw new Exception(x) };
        int size = ct switch { 5126 or 5125 => 4, 5123 => 2, 5121 => 1, _ => throw new Exception($"componentType {ct}") };
        int start = (bv.TryGetProperty("byteOffset", out var o) ? o.GetInt32() : 0) + (a.TryGetProperty("byteOffset", out var ao) ? ao.GetInt32() : 0);
        int stride = bv.TryGetProperty("byteStride", out var st) ? st.GetInt32() : size * comps;
        var r = new float[count * comps];
        for (int i = 0; i < count; i++)
            for (int c = 0; c < comps; c++) {
                int at = start + i * stride + c * size;
                r[i * comps + c] = ct switch { 5126 => BitConverter.ToSingle(buf, at), 5125 => BitConverter.ToUInt32(buf, at), 5123 => BitConverter.ToUInt16(buf, at), _ => buf[at] };
            }
        return r;
    }
    public IEnumerable<Material> Materials() {
        var images = J.GetProperty("images").EnumerateArray().Select(i => i.GetProperty("uri").GetString()!).ToArray();
        var textures = J.GetProperty("textures").EnumerateArray().Select(t => images[t.GetProperty("source").GetInt32()]).ToArray();
        string? Tex(JsonElement e, string key) => e.TryGetProperty(key, out var t) ? textures[t.GetProperty("index").GetInt32()] : null;
        foreach (var m in J.GetProperty("materials").EnumerateArray()) {
            var pbr = m.GetProperty("pbrMetallicRoughness");
            var f = pbr.TryGetProperty("baseColorFactor", out var bf) ? new Vector4(F(bf[0]), F(bf[1]), F(bf[2]), F(bf[3])) : Vector4.One;
            yield return new Material(m.GetProperty("name").GetString()!, Tex(pbr, "baseColorTexture"), f, Tex(pbr, "metallicRoughnessTexture"), Tex(m, "normalTexture"));
        }
    }
    public void AddMesh(int node, Matrix4x4 world, Mesh into, Dictionary<string, string> partOfMaterial) {
        var mats = J.GetProperty("materials");
        foreach (var prim in J.GetProperty("meshes")[Node(node).GetProperty("mesh").GetInt32()].GetProperty("primitives").EnumerateArray()) {
            var at = prim.GetProperty("attributes");
            var pos = Read(at.GetProperty("POSITION").GetInt32(), out _);
            var nrm = Read(at.GetProperty("NORMAL").GetInt32(), out _);
            var uv = Read(at.GetProperty("TEXCOORD_0").GetInt32(), out _);
            var idx = Read(prim.GetProperty("indices").GetInt32(), out _);
            string group = partOfMaterial[mats[prim.GetProperty("material").GetInt32()].GetProperty("name").GetString()!];
            int baseIndex = into.V.Count;
            for (int i = 0; i < pos.Length / 3; i++) {
                into.V.Add(ToUE(Vector3.Transform(new Vector3(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]), world)));
                into.N.Add(DirToUE(Vector3.TransformNormal(new Vector3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]), world)));
                into.T.Add(new Vector2(uv[i * 2], uv[i * 2 + 1]));
            }
            for (int i = 0; i < idx.Length; i += 3)
                into.Tris.Add((group, baseIndex + (int)idx[i], baseIndex + (int)idx[i + 1], baseIndex + (int)idx[i + 2]));
        }
    }
    public static Vector3 ToUE(Vector3 w) => new(-w.Y * 100, -w.X * 100, w.Z * 100);
    static Vector3 DirToUE(Vector3 n) => Vector3.Normalize(new Vector3(-n.Y, -n.X, n.Z));
}

static class Textures {
    static float ToLinear(float c) => c <= 0.04045f ? c / 12.92f : MathF.Pow((c + 0.055f) / 1.055f, 2.4f);
    static float ToSrgb(float c) => c <= 0.0031308f ? c * 12.92f : 1.055f * MathF.Pow(c, 1 / 2.4f) - 0.055f;
    static byte B(float x) => (byte)Math.Clamp((int)MathF.Round(x * 255), 0, 255);

    public static void Write(string dir, Material m, string part, bool metal, int max, string outDir) {
        void Fit(Image<Rgba32> img) { if (img.Width > max || img.Height > max) img.Mutate(x => x.Resize(Math.Min(img.Width, max), Math.Min(img.Height, max))); }
        // Diffuse: the albedo times the material's base colour factor (glTF: linear), as Tarkov's shader tints it
        using (var img = Image.Load<Rgba32>(Path.Combine(dir, m.BaseColor!))) {
            img.ProcessPixelRows(rows => {
                for (int y = 0; y < rows.Height; y++) {
                    var r = rows.GetRowSpan(y);
                    for (int x = 0; x < r.Length; x++) {
                        var p = r[x];
                        r[x] = new Rgba32(B(ToSrgb(ToLinear(p.R / 255f) * m.BaseColorFactor.X)), B(ToSrgb(ToLinear(p.G / 255f) * m.BaseColorFactor.Y)),
                                          B(ToSrgb(ToLinear(p.B / 255f) * m.BaseColorFactor.Z)), 255);
                    }
                }
            });
            Fit(img);
            img.SaveAsPng(Path.Combine(outDir, part + "_Diffuse.png"));
        }
        // Normal: Unity's (OpenGL, green up) to Unreal's (DirectX, green down)
        using (var img = Image.Load<Rgba32>(Path.Combine(dir, m.Normal!))) {
            img.ProcessPixelRows(rows => {
                for (int y = 0; y < rows.Height; y++) {
                    var r = rows.GetRowSpan(y);
                    for (int x = 0; x < r.Length; x++) r[x] = new Rgba32(r[x].R, (byte)(255 - r[x].G), r[x].B, 255);
                }
            });
            Fit(img);
            img.SaveAsPng(Path.Combine(outDir, part + "_Normal.png"));
        }
        // ORM: no occlusion map (white), glTF roughness (G), metallic by part
        using (var img = Image.Load<Rgba32>(Path.Combine(dir, m.MetalRough!))) {
            byte metallic = metal ? (byte)255 : (byte)0;
            img.ProcessPixelRows(rows => {
                for (int y = 0; y < rows.Height; y++) {
                    var r = rows.GetRowSpan(y);
                    for (int x = 0; x < r.Length; x++) r[x] = new Rgba32(255, r[x].G, metallic, 255);
                }
            });
            Fit(img);
            img.SaveAsPng(Path.Combine(outDir, part + "_ORM.png"));
        }
    }
}

static class Preview {
    // Side view (X right, Z up) over top view (X right, +Y down): the M110's points grey, the Barrett by part.
    public static void Draw(string path, Vector3[] donor, Dictionary<string, Mesh> meshes) {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        const float s = 10, x0 = -50, x1 = 115, z0 = -35, z1 = 15, y0 = -10, y1 = 10;
        int w = (int)((x1 - x0) * s), hSide = (int)((z1 - z0) * s), hTop = (int)((y1 - y0) * s);
        using var img = new Image<Rgba32>(w, hSide + hTop + 10, new Rgba32(255, 255, 255, 255));
        void Dot(Vector3 p, Rgba32 c) {
            int x = (int)((p.X - x0) * s), y = (int)((z1 - p.Z) * s);
            if (x >= 0 && x < w && y >= 0 && y < hSide) img[x, y] = c;
            int yt = hSide + 10 + (int)((p.Y - y0) * s);
            if (x >= 0 && x < w && yt >= hSide + 10 && yt < img.Height) img[x, yt] = c;
        }
        foreach (var p in donor) Dot(p, new Rgba32(150, 150, 150, 255));
        var colors = new Dictionary<string, Rgba32> {
            ["Frame1"] = new(200, 0, 0, 255), ["Frame2"] = new(255, 120, 0, 255), ["Barrel"] = new(0, 150, 0, 255), ["Grip"] = new(0, 0, 255, 255),
            ["Sights"] = new(160, 0, 160, 255), ["SightBase"] = new(0, 160, 160, 255), ["Muzzle"] = new(120, 80, 0, 255), ["Mag"] = new(220, 0, 220, 255),
        };
        foreach (var m in meshes.Values)
            foreach (var t in m.Tris) foreach (var i in new[] { t.a, t.b, t.c }) Dot(m.V[i], colors[t.group]);
        img.SaveAsPng(path);
    }
}

// A textured, lit render of the converted meshes (orthographic: right side, left side, top), each with the
// M110's points over it in blue: checks the fit, the faces' winding and the UVs before the game sees them.
// Also the spawner icon, drawn like the game's own thumbnails (right side, transparent, 512 x 512).
static class Render {
    static Dictionary<string, Image<Rgba32>> Textures(IEnumerable<Mesh> meshes, string assets) =>
        meshes.SelectMany(m => m.Tris.Select(t => t.group)).Distinct().ToDictionary(p => p, p => Image.Load<Rgba32>(Path.Combine(assets, p + "_Diffuse.png")));

    // view f: point -> (screen x, screen y down, depth toward the viewer), in cm; s pixels per cm
    static float[] Raster(Image<Rgba32> img, IEnumerable<Mesh> meshes, Dictionary<string, Image<Rgba32>> tex, Func<Vector3, Vector3> f,
                          Vector3 eye, float x0, float y0, float s, bool markBack) {
        int w = img.Width, h = img.Height;
        var depth = new float[w * h];
        Array.Fill(depth, float.NegativeInfinity);
        var light = Vector3.Normalize(eye + new Vector3(0.3f, 0, 0.6f));
        foreach (var m in meshes)
            foreach (var (part, a, b, c) in m.Tris) {
                var pa = f(m.V[a]); var pb = f(m.V[b]); var pc = f(m.V[c]);
                Vector2 A = new((pa.X - x0) * s, (pa.Y - y0) * s), B = new((pb.X - x0) * s, (pb.Y - y0) * s), C = new((pc.X - x0) * s, (pc.Y - y0) * s);
                float area = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
                if (MathF.Abs(area) < 1e-6f) continue;
                int minX = Math.Max(0, (int)MathF.Floor(MathF.Min(A.X, MathF.Min(B.X, C.X)))), maxX = Math.Min(w - 1, (int)MathF.Ceiling(MathF.Max(A.X, MathF.Max(B.X, C.X))));
                int minY = Math.Max(0, (int)MathF.Floor(MathF.Min(A.Y, MathF.Min(B.Y, C.Y)))), maxY = Math.Min(h - 1, (int)MathF.Ceiling(MathF.Max(A.Y, MathF.Max(B.Y, C.Y))));
                var t = tex[part];
                for (int y = minY; y <= maxY; y++)
                    for (int x = minX; x <= maxX; x++) {
                        float px = x + 0.5f, py = y + 0.5f;
                        float wa = ((B.X - px) * (C.Y - py) - (B.Y - py) * (C.X - px)) / area;
                        float wb = ((C.X - px) * (A.Y - py) - (C.Y - py) * (A.X - px)) / area;
                        float wc = 1 - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0) continue;
                        float z = wa * pa.Z + wb * pb.Z + wc * pc.Z;
                        if (z <= depth[y * w + x]) continue;
                        depth[y * w + x] = z;
                        var uv = wa * m.T[a] + wb * m.T[b] + wc * m.T[c];
                        var n = Vector3.Normalize(wa * m.N[a] + wb * m.N[b] + wc * m.N[c]);
                        // a face whose normal points away from the viewer is drawn red: wrong winding or normals
                        bool back = Vector3.Dot(n, eye) < -0.2f;
                        int tx = (int)((uv.X - MathF.Floor(uv.X)) * t.Width) % t.Width, ty = (int)((uv.Y - MathF.Floor(uv.Y)) * t.Height) % t.Height;
                        var c0 = t[tx, ty];
                        float lit = 0.35f + 1.6f * MathF.Max(0, Vector3.Dot(n, back ? -n : light));
                        img[x, y] = back && markBack ? new Rgba32(255, 0, 0, 255)
                            : new Rgba32((byte)Math.Min(255, c0.R * lit), (byte)Math.Min(255, c0.G * lit), (byte)Math.Min(255, c0.B * lit), 255);
                    }
            }
        return depth;
    }

    public static void Views(string dir, Dictionary<string, Mesh> meshes, string assets, Vector3[] donor) {
        var tex = Textures(meshes.Values, assets);
        (string name, Func<Vector3, Vector3> f, Vector3 eye)[] views = [
            ("right", p => new(p.X, -p.Z, p.Y), new(0, 1, 0)),     // seen from +Y (the gun's right)
            ("left", p => new(-p.X, -p.Z, -p.Y), new(0, -1, 0)),
            ("top", p => new(p.X, p.Y, p.Z), new(0, 0, 1)),
        ];
        const float s = 10;
        foreach (var (name, f, eye) in views) {
            float x0 = -45, x1 = 110, y0 = -10, y1 = 32;
            if (name == "left") { x0 = -110; x1 = 45; }
            if (name == "top") { y0 = -12; y1 = 12; }
            using var img = new Image<Rgba32>((int)((x1 - x0) * s), (int)((y1 - y0) * s), new Rgba32(235, 235, 235, 255));
            var depth = Raster(img, meshes.Values, tex, f, eye, x0, y0, s, true);
            foreach (var p in donor) {
                var q = f(p);
                int x = (int)((q.X - x0) * s), y = (int)((q.Y - y0) * s);
                if (x >= 0 && x < img.Width && y >= 0 && y < img.Height && q.Z >= depth[y * img.Width + x] - 0.3f) img[x, y] = new Rgba32(0, 200, 255, 255);
            }
            img.SaveAsPng(Path.Combine(dir, $"render_{name}.png"));
        }
        foreach (var t in tex.Values) t.Dispose();
    }

    // The right side of the region X[x0..x1] Z[z0..z1] (cm), s pixels per cm.
    public static void Region(string path, Dictionary<string, Mesh> meshes, string assets, float x0, float x1, float z0, float z1, float s) {
        var tex = Textures(meshes.Values, assets);
        using var img = new Image<Rgba32>((int)((x1 - x0) * s), (int)((z1 - z0) * s), new Rgba32(235, 235, 235, 255));
        Raster(img, meshes.Values, tex, p => new(p.X, -p.Z, p.Y), new(0, 1, 0), x0, -z1, s, true);
        img.SaveAsPng(path);
        foreach (var t in tex.Values) t.Dispose();
    }

    public static void Icon(string path, IEnumerable<Mesh> meshes, string assets) {
        var list = meshes.ToList();
        var tex = Textures(list, assets);
        var lo = list.Select(m => m.Bounds().Item1).Aggregate(Vector3.Min);
        var hi = list.Select(m => m.Bounds().Item2).Aggregate(Vector3.Max);
        // the game's thumbnails: the gun across ~86% of the width, centred, 4x supersampled
        const int size = 512, ss = 4;
        float s = size * 0.86f / (hi.X - lo.X) * ss;
        float cx = (lo.X + hi.X) / 2, cz = (lo.Z + hi.Z) / 2;
        float half = size * ss / 2f / s;
        using var big = new Image<Rgba32>(size * ss, size * ss, new Rgba32(0, 0, 0, 0));
        Raster(big, list, tex, p => new(p.X, -p.Z, p.Y), new(0, 1, 0), cx - half, -cz - half, s, false);
        using var icon = new Image<Rgba32>(size, size);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                float r = 0, g = 0, b = 0, a = 0;
                for (int j = 0; j < ss; j++)
                    for (int i = 0; i < ss; i++) {
                        var p = big[x * ss + i, y * ss + j];
                        float pa = p.A / 255f;
                        r += p.R * pa; g += p.G * pa; b += p.B * pa; a += pa;
                    }
                icon[x, y] = a > 0 ? new Rgba32((byte)(r / a), (byte)(g / a), (byte)(b / a), (byte)(a / (ss * ss) * 255)) : new Rgba32(0, 0, 0, 0);
            }
        icon.SaveAsPng(path);
        foreach (var t in tex.Values) t.Dispose();
    }
}

