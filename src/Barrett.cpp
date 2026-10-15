// Barrett M107A1 - GML plugin (com.remarrow.barrett). Personal use: the model and textures come from the
// user's own export of the WTT-Armory mod for SPT and must not be distributed.
//
// Adds a "Barrett M107A1" to the spawner's sniper list: a semi-automatic .50 BMG rifle, built on the game's
// KAC M110 (semi-auto, AR-style controls, the same A2 pistol grip) WITHOUT replacing any game asset - every
// other M110 stays an M110.
//
// 1. Spawner entry: after BP_GameInstance::GetItems builds AvailableGuns, a GunAsset_C constructed at runtime
//    (the M110's receiver, an icon rendered from the Barrett model) is appended to the M110's category.
// 2. Visuals: an M110 receiver spawned from that entry (or rebuilt from a saved loadout slot holding it) gets
//    the Barrett as DynamicMeshComponents built from the OBJs embedded in this DLL, textured with
//    M_DefaultShader instances; the M110 mesh and moving parts are hidden. Its bolt carrier (with the side
//    charging handle), trigger and magazine release move on their own pivots, posed by this plugin every frame
//    from the gun's state. Magazines in a Barrett, and the spares its holder carries, become the Barrett's
//    10-round magazine. The Barrett is fitted on the M110 by its pistol grip (tools\prep), and the M110's
//    muzzle point, magazine well, optic and charging handle grab point are moved onto the Barrett's.
// 3. .50 BMG: the game has no .50 round. AGunReceiver::FireRoundLocal (native) spawns the round from its
//    projectile class's defaults, so while a Barrett fires, the M110 round's defaults are a .50 BMG ball
//    (42 g, 12.95 mm, 853 m/s, its own damage curve, never recycled from the bullet pool) and are put back the
//    moment the shot is out. Every other 7.62 gun keeps its 7.62.
//
// Settings: GML\config\com.remarrow.barrett.cfg. Build: build.bat [install].
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <map>
#include "payload.h"
#include "payload_key.h"  // generated per build by tools\pack (build\gen), never committed
#include "version.h"

using namespace gml;

static const char* kDonor = "/Game/Weapons/Data/DA_KAC_M110SASS.DA_KAC_M110SASS";
static const char* kDisplayName = "Barrett M107A1";

static GUObject* s_gun = nullptr;  // our runtime GunAsset

// ------------------------------------------------------------------ assets

// Decrypts one asset from the payload into `out`. Callers wipe it as soon as the engine has it, so at
// most one asset exists in plain form, and only for that moment.
static bool Unpack(const char* name, std::vector<uint8_t>& out, bool optional = false) {
    static Blob blob = Resource("DATA");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = kPayloadKeyA[i] ^ kPayloadKeyB[i];
    bool ok = blob && payload::Extract(blob.data, blob.size, key, name, out);
    SecureZeroMemory(key, sizeof key);
    if (!ok && !optional) Error("embedded asset '{}' is missing or damaged", name);
    return ok;
}

static GUObject* Icon() {
    static GUObject* icon = nullptr;
    if (icon && API->IsValid(icon)) return icon;
    std::vector<uint8_t> buf;
    if (Unpack("Icon", buf, true)) {
        icon = API->ImportTextureFromMemory(buf.data(), buf.size(), "BarrettIcon");
        payload::Wipe(buf);
        if (icon) API->KeepAlive(icon);
    }
    return icon;
}

// ------------------------------------------------------------------ spawner entry

static GML_TArray* GunList(Object gi, uint8_t category) {
    GFProperty* mapProp = API->FindProperty(gi.Struct(), "AvailableGuns");
    void* map = gi.PropPtr("AvailableGuns");
    if (!mapProp || !map) return nullptr;
    struct Ctx { GML_TArray* found; GUStruct* valueStruct; uint8_t category; } ctx{nullptr, nullptr, category};
    GML_PropInfo mi, vi;
    API->GetPropertyInfo(mapProp, &mi);
    API->GetPropertyInfo(mi.value, &vi);
    ctx.valueStruct = vi.structType;  // FGunAssetList
    API->MapForEach(map, mapProp, [](void* u, void* key, void* value) -> int {
        auto* c = (Ctx*)u;
        if (*(uint8_t*)key != c->category) return 0;
        GFProperty* arr = API->FindProperty(c->valueStruct, "GunAssets");  // GUID-mangled in the BP struct
        GML_PropInfo ai;
        if (!arr || !API->GetPropertyInfo(arr, &ai)) return 1;
        c->found = (GML_TArray*)((uint8_t*)value + ai.offset);
        return 1;
    }, &ctx);
    return ctx.found;
}

// The Barrett goes in the M110's own category (the game's sniper list).
static void AddGun(Object gi) {
    Object donor = FindObject(kDonor);
    GUClass* gunClass = FindClass("GunAsset_C");
    if (!donor || !gunClass) { Error("donor {} or GunAsset_C missing", kDonor); return; }
    uint8_t category = donor.Get<uint8_t>("Category");
    GML_TArray* raw = GunList(gi, category);
    if (!raw) { Warn("AvailableGuns[{}] not found yet", category); return; }
    TArrayView<GUObject*> list{raw};
    if (s_gun && Object(s_gun) && list.Contains(s_gun)) return;  // GetItems ran again; already there

    Object gun = NewObject(gunClass, gi);
    if (!gun) { Error("could not construct GunAsset_C"); return; }
    gun.Set("ReceiverClass", donor.Get<GUClass*>("ReceiverClass"));
    GUObject* icon = Icon();
    gun.Set("Icon", icon ? icon : donor.Get<GUObject*>("Icon"));
    gun.SetBool("bEnabled", donor.GetBool("bEnabled"));
    gun.Set("Category", category);
    uint8_t text[16];
    if (API->MakeText(kDisplayName, text)) std::memcpy(gun.PropPtr("DisplayName"), text, 16);

    int before = list.Num();
    if (list.Add(gun.ptr)) {
        s_gun = gun.ptr;
        Log("*** ADDED '{}' to the spawner (category {}, {} -> {}) as {}{} ***", kDisplayName, category, before, list.Num(), gun.FullName(),
            icon ? "" : " (M110 icon: the Barrett's could not be built)");
    } else {
        Error("append failed");
    }
}

// ------------------------------------------------------------------ visuals

static const char* kShader = "/Game/Art/Common/M_DefaultShader.M_DefaultShader";
// Texture sets / material slots = OBJ `usemtl` groups (tools\prep).
static const char* kParts[] = {"Frame1", "Frame2", "Barrel", "Muzzle", "Mag", "Grip", "Sights", "SightBase"};
static constexpr int kNumParts = 8;

// The Barrett's own moving parts (see "the Barrett's own action" below).
enum class PartKind { Carrier, Trigger, MagRelease };
struct MovingPart { const char* asset; PartKind kind; GUObject* mesh; };
static MovingPart s_movingParts[] = {
    {"barrett_bolt", PartKind::Carrier, nullptr},  // bolt carrier with its charging handle
    {"barrett_trigger", PartKind::Trigger, nullptr},
    {"barrett_magrelease", PartKind::MagRelease, nullptr},
};
static constexpr int kNumMoving = (int)std::size(s_movingParts);

static bool s_ownMovingParts = true, s_ownMagazines = true, s_fitAttachments = true;
static float s_offset[3] = {0, 0, 0};  // cm, a nudge on top of the fit baked into the OBJs
static GUObject* s_mesh = nullptr;        // runtime UDynamicMesh: frame, barrel, bipod, grip, sights
static GUObject* s_muzzleMesh = nullptr;  // the muzzle brake (hidden while a muzzle device is mounted)
static GUObject* s_sightsMesh = nullptr;  // the flip-up iron sights (hidden while an optic is on the top rail)
static GUObject* s_magMesh = nullptr;     // the 10-round magazine, in gun space as if seated
static GUObject* s_mids[kNumParts] = {};  // one M_DefaultShader instance per texture set
static bool s_assetsTried = false;
static GUClass* s_receiverClass = nullptr;

struct Pending { GUObject* actor; ULONGLONG since; };
static std::vector<Pending> s_pending;

static bool BuildVisualAssets() {
    if (s_assetsTried) return s_mesh != nullptr;
    s_assetsTried = true;
    if (API->size < offsetof(GML_API, ImportDynamicMeshFromMemory) + sizeof(void*)) {
        Error("this plugin needs GML 2.2 or newer (assets embedded in the DLL)");
        return false;
    }
    Object shader = LoadObject(kShader);
    if (!shader) { Error("weapon shader {} not found", kShader); return false; }

    std::vector<uint8_t> buf;
    for (int i = 0; i < kNumParts; i++) {
        s_mids[i] = API->CreateMaterialInstance(shader.ptr);
        if (!s_mids[i]) { Error("could not create a material instance for {}", kParts[i]); return false; }
        for (const char* param : {"Diffuse", "Normal", "ORM"}) {
            std::string res = std::string(kParts[i]) + "_" + param;
            GUObject* tex = Unpack(res.c_str(), buf) ? API->ImportTextureFromMemory(buf.data(), buf.size(), res.c_str()) : nullptr;
            payload::Wipe(buf);
            if (!tex || !API->SetMaterialTexture(s_mids[i], param, tex))
                Warn("{}: texture {} missing - that channel keeps the shader default", kParts[i], param);
        }
    }

    GML_MeshImport o{};
    o.size = sizeof o;
    o.axis = GML_AXIS_UNREAL;  // tools\prep writes the game's space, cm, fitted
    std::memcpy(o.offset, s_offset, sizeof s_offset);
    o.flipWinding = -1;
    o.materialCount = kNumParts;
    o.materialNames = kParts;
    o.materials = s_mids;
    auto load = [&](const char* name, bool optional = false) -> GUObject* {
        if (!Unpack(name, buf, optional)) return nullptr;
        GUObject* m = API->ImportDynamicMeshFromMemory(buf.data(), buf.size(), name, &o);
        payload::Wipe(buf);
        if (!m) Error("could not build the Barrett mesh '{}'", name);
        return m;
    };
    s_mesh = load("barrett");
    s_muzzleMesh = load("barrett_muzzle");
    s_sightsMesh = load("barrett_sights");
    s_magMesh = load("barrett_mag");
    for (auto& p : s_movingParts) p.mesh = load(p.asset, true);
    if (s_mesh) Log("Barrett visuals ready: {}", Object(s_mesh).FullName());
    return s_mesh != nullptr;
}

// ------------------------------------------------------------------ transforms (raw FTransform bytes)

using Xf = std::vector<uint8_t>;
static size_t XfSize() {
    static size_t size = 0;
    if (!size) {
        GML_PropInfo ri;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &ri);
        size = ri.size;
    }
    return size;
}
static Xf TakeXf(Params& p, const char* name = "ReturnValue") {
    auto* s = (uint8_t*)p.Ptr(name);
    return Xf(s, s + XfSize());
}
static void PutXf(Params& p, const char* name, const Xf& x) { std::memcpy(p.Ptr(name), x.data(), x.size()); }
static Xf XfCall(const char* fn, std::initializer_list<std::pair<const char*, const Xf*>> args) {
    Params p(Lib("KismetMathLibrary"), fn);
    for (auto& [n, x] : args) PutXf(p, n, *x);
    p.Call();
    return TakeXf(p);
}
static Xf Compose(const Xf& a, const Xf& b) { return XfCall("ComposeTransforms", {{"A", &a}, {"B", &b}}); }  // a then b
static Xf Invert(const Xf& t) { return XfCall("InvertTransform", {{"T", &t}}); }
static Xf RelativeTo(const Xf& a, const Xf& to) { return XfCall("MakeRelativeTransform", {{"A", &a}, {"RelativeTo", &to}}); }
static Xf WorldXf(Object obj) {  // actor or scene component
    Params p(obj, obj.IsA("Actor") ? "GetTransform" : "K2_GetComponentToWorld");
    p.Call();
    return TakeXf(p);
}
static void SetRelativeXf(Object comp, const Xf& x) {
    Params p(comp, "K2_SetRelativeTransform");
    PutXf(p, "NewTransform", x);
    p.Set("bSweep", false).Set("bTeleport", true).Call();
}

// A bone's reference-pose transform in component space: its local ref transforms composed up the
// parent chain. The rest pose, whatever the gun is doing right now (bolt back, trigger held).
static Xf RefPoseComponent(Object skel, const char* bone) {
    Xf acc;
    uint64_t name = API->MakeName(bone);
    for (int guard = 0; guard < 64 && name; guard++) {
        Params bi(skel, "GetBoneIndex");
        *(uint64_t*)bi.Ptr("BoneName") = name;
        bi.Call();
        int idx = bi.Return<int32_t>();
        if (idx < 0) break;
        Params rp(skel, "GetRefPoseTransform");
        rp.Set("BoneIndex", (int32_t)idx).Call();
        Xf local = TakeXf(rp);
        acc = acc.empty() ? local : Compose(acc, local);  // child first, then its parent
        Params pb(skel, "GetParentBone");
        *(uint64_t*)pb.Ptr("BoneName") = name;
        pb.Call();
        name = pb.Return<uint64_t>();
    }
    return acc;
}

static void HideComponent(Object c) {
    Params v(c, "SetVisibility");
    v.Set("bNewVisibility", false).Set("bPropagateToChildren", false).Call();  // attached children stay visible
}

struct Vec3 { double x, y, z; };

// The M110 receiver's points moved onto the Barrett's (actor space, cm; the M110's from the stock gun, the
// Barrett's from tools\prep's fit). Each point is found by its component name; one not on the receiver is left
// alone. The Barrett's bore is 1.5 cm higher than the M110's, its chamber 11 cm further forward.
struct AttachPoint { const char* name; double x, y, z; Vec3 m110; };
static const AttachPoint kAttachPoints[] = {
    {"Fire Location", 92.011, -0.044, 3.804, {59.88, 0, 2.33}},       // shot origin, muzzle-device mount: the brake's start
    {"FireChamber", 15.335, -0.042, 3.822, {4.37, 0, 2.33}},          // the chamber (and the round's load / eject points)
    {"ReceiverAttachmentSpline", -6.77, 0, 7.95, {-6.77, 0, 5.70}},   // the top rail (optics): the Barrett's is 2.25 higher
    {"ReceiverAttachmentSplineHG12", 11.26, 0, 7.96, {11.26, 0, 5.70}},
    {"ReceiverAttachmentSplineHG9", 30.0, 3.05, 4.34, {14.24, 2.78, 2.33}},   // the side and bottom rails: on the front of the
    {"ReceiverAttachmentSplineHG3", 30.0, -3.05, 4.34, {14.24, -2.78, 2.33}}, // Barrett's receiver (6 cm wide, Z 0.7 to 8)
    {"ReceiverAttachmentSplineHG6", 30.0, 0, 0.70, {15.24, 0, -0.45}},
    {"SecondaryHandSocket", 32.0, 0, 4.34, {28.45, 0, 2.34}},         // the support hand round the front of the receiver
    {"DefaultStockButtPosition", -43.35, 0, -5.41, {-43.44, 0, -7.07}},  // behind the bottom of the butt pad, as the M110's
};

static Vec3 AttachTarget(const AttachPoint& p) { return {p.x + s_offset[0], p.y + s_offset[1], p.z + s_offset[2]}; }

// Actor transform <-> world, via KismetMathLibrary on the actor's FTransform.
struct ActorFrame {
    Params gt;
    size_t size;
    explicit ActorFrame(Object actor) : gt(actor, "GetTransform") {
        gt.Call();
        GML_PropInfo xf;
        API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &xf);
        size = xf.size;
    }
    Vec3 Call(const char* fn, const char* arg, Vec3 v) {
        Params p(Lib("KismetMathLibrary"), fn);
        std::memcpy(p.Ptr("T"), gt.Ptr("ReturnValue"), size);
        p.Set(arg, v).Call();
        return p.Return<Vec3>();
    }
    Vec3 ToWorld(Vec3 local) { return Call("TransformLocation", "Location", local); }
    Vec3 ToLocal(Vec3 world) { return Call("InverseTransformLocation", "Location", world); }
    Vec3 DirToWorld(Vec3 local) { return Call("TransformDirection", "Direction", local); }
};

static const AttachPoint* FindAttachPoint(const std::string& componentName) {
    for (auto& p : kAttachPoints)
        if (componentName == p.name) return &p;
    return nullptr;
}

// Attachments already on the gun (a loadout restores them) move with the point they hang on: one attached to the
// point's component itself went along with it; one held elsewhere for that point moves by as much as it did.
static int SnapMountedAttachments(Object gun, ActorFrame& frame) {
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
    auto* arr = (GML_TArray*)ga.Ptr("OutActors");
    int snapped = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object att(((GUObject**)arr->Data)[i]);
        if (!att.IsA("GunAttachment")) continue;
        Object point = att.GetObj("AttachablePoint"), parent = att.GetObj("RootComponent").GetObj("AttachParent");
        const AttachPoint* p = point ? FindAttachPoint(point.Name()) : nullptr;
        if (!p && parent) p = FindAttachPoint(parent.Name());
        if (!p || (parent && FindAttachPoint(parent.Name()) == p)) continue;  // none of ours, or it moved with its point
        Vec3 to = AttachTarget(*p);
        Vec3 d = frame.DirToWorld({to.x - p->m110.x, to.y - p->m110.y, to.z - p->m110.z});
        Params mv(att, "K2_AddActorWorldOffset");
        mv.Set("DeltaLocation", d).Set("bSweep", false).Set("bTeleport", true).Call();
        snapped++;
    }
    return snapped;
}

static void FitAttachPoints(Object actor) {
    ActorFrame frame(actor);
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("SceneComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    int moved = 0;
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (const AttachPoint* p = FindAttachPoint(c.Name())) {
            Params sw(c, "K2_SetWorldLocation");
            sw.Set("NewLocation", frame.ToWorld(AttachTarget(*p))).Set("bSweep", false).Set("bTeleport", true).Call();
            moved++;
        }
    }
    if (moved != (int)std::size(kAttachPoints))
        Warn("{}: moved {} of {} attach points - receiver layout differs from the M110's", actor.Name(), moved, std::size(kAttachPoints));
    if (int n = SnapMountedAttachments(actor, frame)) Log("{}: {} mounted attachment(s) moved onto the Barrett", actor.Name(), n);
}

// An optic on the top rail: the Barrett's raised iron sights would stand in its view, so they are hidden. An
// attachment counts if it hangs on a top-rail spline, or sits on top of the rail (above Z 7.5 in the gun's space:
// side-rail and bottom-rail items, and muzzle devices, sit lower).
static bool OpticMounted(Object gun) {
    Params ga(gun, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
    auto* arr = (GML_TArray*)ga.Ptr("OutActors");
    ActorFrame frame(gun);
    for (int i = 0; arr && i < arr->Num; i++) {
        Object att(((GUObject**)arr->Data)[i]);
        if (!att.IsA("GunAttachment") || !att.GetBool("bAttached")) continue;
        for (Object c : {att.GetObj("AttachablePoint"), att.GetObj("RootComponent").GetObj("AttachParent")}) {
            std::string n = c ? c.Name() : "";
            if (n == "ReceiverAttachmentSpline" || n == "ReceiverAttachmentSplineHG12") return true;
        }
        Params al(att, "K2_GetActorLocation");
        al.Call();
        Vec3 at = frame.ToLocal(al.Return<Vec3>());
        if (at.z > 7.5 + s_offset[2] && at.x > -20 && at.x < 60) return true;
    }
    return false;
}

// ------------------------------------------------------------------ the gun

static int s_capacity = 10;
static float s_recoilScale = 2.5f;

// The receiver as a Barrett: its recoil is the M110's times [Gun] RecoilScale (a .50 BMG kicks far harder than
// a 7.62, even through the Barrett's brake and recoiling barrel). Everything else (handling, semi-auto) stays the
// M110's.
static void ConfigureGun(Object gun) {
    if (GUClass* m110 = gun.Class()) {
        Object ref = DefaultObject(m110);
        for (const char* v : {"RotationPerShot", "LocationPerShot"}) {
            Vec3 x = ref.Get<Vec3>(v);
            gun.Set(v, Vec3{x.x * s_recoilScale, x.y * s_recoilScale, x.z * s_recoilScale});
        }
        for (const char* v : {"MaxRotation", "MaxLocation"}) {
            Vec3 x = ref.Get<Vec3>(v);
            double k = (std::max)(1.0f, s_recoilScale);
            gun.Set(v, Vec3{x.x * k, x.y * k, x.z * k});
        }
    }
}

// A magazine for the Barrett: 10 rounds (the receiver takes the round class from the magazine it seats).
static void LoadMagazine(Object mag) {
    if (!mag) return;
    if (mag.Get<int32_t>("AmmoCapacity") != s_capacity) mag.Set<int32_t>("AmmoCapacity", s_capacity);
    if (mag.Get<int32_t>("CurrentAmmo") > s_capacity) mag.Set<int32_t>("CurrentAmmo", s_capacity);
}

// ------------------------------------------------------------------ .50 BMG
//
// AGunReceiver::FireRoundLocal spawns its BulletData's round through AEBBullet::Spawn, which reads the class
// defaults: a new round copies the defaults' mass, size, drag, penetration and damage curve. While a Barrett's
// FireRoundLocal runs, those defaults are a .50 BMG ball; pooling is off for that moment so the round is a new
// actor with the .50's values (and is destroyed, not pooled, when it stops). Then the defaults go back, so every
// other gun firing the same round keeps it.

// Prologue of AGunReceiver::FireRoundLocal(const FVector&, const FVector&) in this build (RVA 0x5048110);
// "48 8B 99 A0 05 00 00" is its read of BulletData.
static const char* kFireRoundLocalSig =
    "40 55 53 56 57 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 00 "
    "48 8B 99 A0 05 00 00 49 8B F0 4C 8B FA 48 8B F9";
using FireRoundLocalFn = void (*)(GUObject* self, const void* direction, const void* origin);
static FireRoundLocalFn s_fireRoundLocal = nullptr;
static bool s_bmg = true;
static float s_bmgDamage = 600, s_bmgVelocity = 853, s_bmgMass = 42, s_bmgPenetration = 2.5f;

static bool IsBarrett(GUObject* receiver);

// A linear speed (cm/s) -> damage curve for the .50, like the game's Dmg_* curves.
static GUObject* BmgDamageCurve() {
    static GUObject* curve = nullptr;
    if (curve && API->IsValid(curve)) return curve;
    Object c = NewObject(FindClass("CurveFloat"), Object(API->WorldContext()));
    GFProperty* fc = c ? API->FindProperty(c.Struct(), "FloatCurve") : nullptr;
    GML_PropInfo fi{}, ki{};
    if (!fc || !API->GetPropertyInfo(fc, &fi) || !fi.structType) return nullptr;
    GFProperty* kp = API->FindProperty(fi.structType, "Keys");
    if (!kp || !API->GetPropertyInfo(kp, &ki)) return nullptr;
    struct Key { uint8_t interp, tangentMode, tangentWeightMode, pad; float time, value, arrive, arriveWeight, leave, leaveWeight; };
    static_assert(sizeof(Key) == 0x1C);  // FRichCurveKey
    auto* keys = (GML_TArray*)((uint8_t*)c.ptr + fi.offset + ki.offset);
    auto* k = (Key*)API->ArrayAdd(keys, sizeof(Key), 2);
    if (!k) return nullptr;
    k[0] = {0, 0, 0, 0, 10000.f, s_bmgDamage * 0.15f};               // RCIM_Linear; 100 m/s
    k[1] = {0, 0, 0, 0, s_bmgVelocity * 100.f, s_bmgDamage};          // the muzzle
    API->KeepAlive(c.ptr);
    curve = c.ptr;
    Params check(c, "GetFloatValue");
    check.Set("InTime", s_bmgVelocity * 100.f).Call();
    Params half(c, "GetFloatValue");
    half.Set("InTime", s_bmgVelocity * 50.f).Call();
    Log(".50 BMG damage curve: {:.0f} at {:.0f} m/s, {:.0f} at {:.0f} m/s", check.Return<float>(), s_bmgVelocity, half.Return<float>(),
        s_bmgVelocity / 2);
    return curve;
}

struct RoundValue { const char* prop; float bmg; float saved; };

static void FireRoundLocalDetour(GUObject* self, const void* direction, const void* origin) {
    if (!s_bmg || !IsBarrett(self)) { s_fireRoundLocal(self, direction, origin); return; }
    Object gun(self);
    GUClass* round = gun.Get<GUClass*>("BulletData");
    Object cdo = round ? DefaultObject(round) : Object();
    GUObject* curve = BmgDamageCurve();
    if (!cdo || !curve) { s_fireRoundLocal(self, direction, origin); return; }

    float pen = cdo.Get<float>("MaxPenetration") * s_bmgPenetration;
    RoundValue values[] = {
        {"MuzzleVelocityMin", s_bmgVelocity * 100.f}, {"MuzzleVelocityMax", s_bmgVelocity * 100.f},
        {"Mass", s_bmgMass / 1000.f},  // kg
        {"Diameter", 1.295f},          // cm
        {"MinPenetration", cdo.Get<float>("MinPenetration") * s_bmgPenetration}, {"MaxPenetration", pen},
    };
    for (auto& v : values) { v.saved = cdo.Get<float>(v.prop); cdo.Set<float>(v.prop, v.bmg); }
    bool shotgun = cdo.GetBool("Shotgun"), pooling = cdo.GetBool("EnablePooling");
    GUObject* damage = cdo.Get<GUObject*>("DamageCurve");
    cdo.SetBool("Shotgun", false);
    cdo.SetBool("EnablePooling", false);
    cdo.Set<GUObject*>("DamageCurve", curve);

    s_fireRoundLocal(self, direction, origin);

    for (auto& v : values) cdo.Set<float>(v.prop, v.saved);
    cdo.SetBool("Shotgun", shotgun);
    cdo.SetBool("EnablePooling", pooling);
    cdo.Set<GUObject*>("DamageCurve", damage);
}

// The same function once another plugin hooked it (the Saiga-12K does too): GML put a 5-byte jmp in place of its
// first five bytes, to a relay that jumps on to that plugin's detour.
static const char* kFireRoundLocalHookedSig =
    "E9 ?? ?? ?? ?? 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 00 "
    "48 8B 99 A0 05 00 00 49 8B F0 4C 8B FA 48 8B F9";
static void* FollowJumps(void* p) {  // jmp rel32 / jmp [rip+0] chains, to the code they end at
    for (int i = 0; i < 16; i++) {
        auto* b = (uint8_t*)p;
        if (b[0] == 0xE9) p = b + 5 + *(int32_t*)(b + 1);
        else if (b[0] == 0xFF && b[1] == 0x25) p = *(void**)(b + 6 + *(int32_t*)(b + 2));
        else break;
    }
    return p;
}

static void InstallBmg() {
    if (!s_bmg) { Log(".50 BMG off: the Barrett fires the M110's 7.62"); return; }
    void* target = API->FindPattern(kFireRoundLocalSig);
    if (!target) {
        // Hooked by another plugin first: hook the end of its jumps (its detour), so each plugin's detour runs in
        // turn and passes on the guns that aren't its own.
        if (void* hooked = API->FindPattern(kFireRoundLocalHookedSig)) {
            target = FollowJumps(hooked);
            Log(".50 BMG: FireRoundLocal (exe+0x{:X}) is already hooked by another plugin - chaining after it",
                (uintptr_t)hooked - (uintptr_t)API->ImageBase());
        }
    }
    if (!target) {
        Warn("AGunReceiver::FireRoundLocal not found (game updated?) - the Barrett fires the M110's 7.62");
        return;
    }
    if (!API->HookNative(target, (void*)&FireRoundLocalDetour, (void**)&s_fireRoundLocal) || !s_fireRoundLocal) {
        Error("could not hook FireRoundLocal - the Barrett fires the M110's 7.62");
        s_fireRoundLocal = nullptr;
        return;
    }
    Log(".50 BMG: FireRoundLocal hooked at exe+0x{:X}", (uintptr_t)target - (uintptr_t)API->ImageBase());
}

// ------------------------------------------------------------------ magazines
//
// The M110 takes the game's SR-25 20-round magazine. A magazine that goes into a Barrett - and the spare ones
// you carry while you have one - becomes the Barrett's: 10 rounds, and the Barrett's mesh in place of the
// SR-25's. The Barrett magazine's geometry is in gun space as if seated, so it hangs on the magazine at the
// inverse of where a seated magazine sits in the gun (measured once from the first seated magazine).

static std::vector<GUObject*> s_dressedMags;
static Xf s_magInGun;  // magazine actor transform relative to the gun, fully inserted

// Only this plugin gives an M110 receiver or its magazine DynamicMeshComponents (the Barrett's meshes).
static bool IsDressed(Object actor) {
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("DynamicMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    return arr && arr->Num > 0;
}

// The game draws the rounds (instanced meshes) for its magazine; the Barrett's is solid, so they stay hidden.
static void HideRounds(Object mag) {
    Params gc(mag, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass("InstancedStaticMeshComponent")).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) {
        Object c(((GUObject**)arr->Data)[i]);
        if (c.GetBool("bVisible")) HideComponent(c);
    }
}

static void FitMagGrip(Object mag);

static void DressMagazine(Object mag, Object insertedIn) {
    if (!mag) return;
    LoadMagazine(mag);
    if (!s_ownMagazines || !s_magMesh || IsDressed(mag)) return;
    if (insertedIn && s_magInGun.empty()) {
        // The reference comes from a magazine seated in the gun (its CurrentMagazine) that sits in the
        // same place on two checks a quarter second apart - not one still sliding in or out.
        static Xf candidate;
        if (insertedIn.GetObj("CurrentMagazine").ptr != mag.ptr) return;
        Xf now = RelativeTo(WorldXf(mag), WorldXf(insertedIn));
        bool steady = false;
        if (!candidate.empty()) {
            Params eq(Lib("KismetMathLibrary"), "NearlyEqual_TransformTransform");
            PutXf(eq, "A", candidate);
            PutXf(eq, "B", now);
            eq.Set("LocationTolerance", 0.05f).Set("RotationTolerance", 0.2f).Set("Scale3DTolerance", 0.001f).Call();
            steady = eq.Return<bool>();
        }
        candidate = now;
        if (!steady) return;
        s_magInGun = now;
        Log("magazine reference taken from {} seated in {}", mag.Name(), insertedIn.Name());
    }
    if (s_magInGun.empty()) return;  // no reference yet: dressed once one has been seen seated
    Object root = mag.GetObj("RootComponent");
    Object comp = API->AddDynamicMeshComponent(mag.ptr, s_magMesh, s_mids, kNumParts);
    if (!root || !comp) return;
    SetRelativeXf(comp, Invert(s_magInGun));
    HideComponent(root);
    HideRounds(mag);
    FitMagGrip(mag);
    s_dressedMags.push_back(mag.ptr);
    Log("magazine {} is now a Barrett magazine ({} rounds)", mag.Name(), mag.Get<int32_t>("CurrentAmmo"));
}

// ------------------------------------------------------------------ dressing

struct DressedGun { GUObject* gun; GUObject* muzzle; GUObject* sights; };
static std::vector<DressedGun> s_dressedGuns;  // also what the .50 hook checks

static bool IsBarrett(GUObject* receiver) {
    for (auto& d : s_dressedGuns)
        if (d.gun == receiver) return API->IsValid(receiver);
    return false;
}

// ------------------------------------------------------------------ the Barrett's own action
//
// The Barrett's moving parts are its own meshes, posed every frame by this plugin about the Barrett's own pivots
// (the Tarkov rig's node origins, fitted by tools\prep), from the state the game keeps on the receiver:
//   bolt carrier + charging handle  slides as far as the game moves the M110's carrier bone (firing cycles,
//                                   a pulled charging handle)
//   trigger                         back kTriggerPull degrees on its pin with the trigger pull (CurrentTrigger)
//   magazine release                its lever swings kReleasePress degrees while the release is pressed
// The M110's movables stay hidden; only its carrier bone is read.

struct Q { double x, y, z, w; };
static Q AxisAngle(int axis, double deg) {
    double h = deg * 3.14159265358979 / 360;
    Q q{0, 0, 0, std::cos(h)};
    (&q.x)[axis] = std::sin(h);
    return q;
}
static Vec3 Rotate(const Q& q, Vec3 v) {  // FQuat::RotateVector
    Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

// An FTransform as the engine lays it out (doubles): rotation quaternion, translation, scale.
struct XfD { double q[4]; double t[3]; double pad0; double s[3]; double pad1; };
static_assert(sizeof(XfD) == 0x60);

static bool XfLayoutOk() {
    static int ok = -1;
    if (ok < 0) {
        Params mk(Lib("KismetMathLibrary"), "MakeTransform");
        struct { double pitch, yaw, roll; } yaw90{0, 90, 0};
        mk.Set("Location", Vec3{1, 2, 3}).Set("Rotation", yaw90).Set("Scale", Vec3{1, 1, 1}).Call();
        XfD x{};
        ok = XfSize() == sizeof x;
        if (ok) {
            std::memcpy(&x, mk.Ptr("ReturnValue"), sizeof x);
            ok = x.t[0] == 1 && x.t[1] == 2 && x.t[2] == 3 && x.s[0] == 1 && std::fabs(std::fabs(x.q[2]) - 0.7071068) < 1e-5 &&
                 std::fabs(x.q[3]) > 0.7;
        }
        if (!ok) Warn("FTransform layout is not the expected one - the Barrett's moving parts stay still");
    }
    return ok == 1;
}

// The magazine's grip (its hand socket, the hand you take it by) is made for the SR-25's 20-round magazine: a fist the
// magazine passes through, its fingers round the magazine's front. The Barrett's magazine is twice as deep front to
// back, so that fist's fingers sank 2 to 2.5 cm into it. On a Barrett magazine the grip is turned (about 26 degrees)
// and moved 6.7 cm forward, so the hand closes round the magazine's front edge: fingers at 80% of their curl, the
// palm, thumb and first two fingers 2.5 to 4.5 mm off it, no part of the hand higher on it than the SR-25 grip's
// thumb. The game mirrors the socket for the other hand, which fits the same way. tests\BarrettDev gripped a spare
// magazine with the player's own hands (held where the arm reaches its IK target, as in VR) at several curls and
// wrote the hands out; gripfit searched one turn, move and curl for both hands.
static const Q kMagGripTurn{0.15001, 0.16787, -0.05115, 0.97298};  // about the left palm, on the magazine's root
static const Vec3 kMagGripPivot{-2.1481, -4.1205, -13.7990};
static const Vec3 kMagGripMove{6.700, 1.500, -1.800};
static constexpr double kMagGripCurl = 0.8;
static void FitMagGrip(Object mag) {
    Object sock = mag.GetObj("HandSocket");
    if (!sock || !XfLayoutOk()) return;
    Vec3 l = sock.Get<Vec3>("RelativeLocation");
    Vec3 r = Rotate(kMagGripTurn, {l.x - kMagGripPivot.x, l.y - kMagGripPivot.y, l.z - kMagGripPivot.z});
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Location", Vec3{r.x + kMagGripPivot.x + kMagGripMove.x, r.y + kMagGripPivot.y + kMagGripMove.y, r.z + kMagGripPivot.z + kMagGripMove.z})
        .Set("Rotation", sock.Get<Vec3>("RelativeRotation")).Set("Scale", sock.Get<Vec3>("RelativeScale3D")).Call();
    XfD x;
    std::memcpy(&x, mk.Ptr("ReturnValue"), sizeof x);
    const Q& a = kMagGripTurn;  // turn after the socket's own rotation: a * q
    double q0[4] = {x.q[0], x.q[1], x.q[2], x.q[3]};
    x.q[3] = a.w * q0[3] - a.x * q0[0] - a.y * q0[1] - a.z * q0[2];
    x.q[0] = a.w * q0[0] + a.x * q0[3] + a.y * q0[2] - a.z * q0[1];
    x.q[1] = a.w * q0[1] - a.x * q0[2] + a.y * q0[3] + a.z * q0[0];
    x.q[2] = a.w * q0[2] + a.x * q0[1] - a.y * q0[0] + a.z * q0[3];
    Params st(sock, "K2_SetRelativeTransform");
    std::memcpy(st.Ptr("NewTransform"), &x, sizeof x);
    st.Set("bSweep", false).Set("bTeleport", true).Call();
    // the fingers: each joint's turn from the open hand, kMagGripCurl of it (FBPVRHandPoseBonePair: name, pad, FQuat)
    struct Pair { uint64_t bone, pad; double q[4]; uint8_t rest[0x10]; };
    GML_PropInfo ai{}, ei{};
    GFProperty* deltas = API->FindProperty(sock.Struct(), "CustomPoseDeltas");
    if (!deltas || !API->GetPropertyInfo(deltas, &ai) || !ai.inner || !API->GetPropertyInfo(ai.inner, &ei) || ei.size != (int)sizeof(Pair)) {
        Warn("{}: the magazine's grip has no finger poses in the expected layout - its fingers keep their curl", mag.Name());
        return;
    }
    if (auto* d = (GML_TArray*)sock.PropPtr("CustomPoseDeltas"))
        for (int i = 0; i < d->Num; i++) {
            double* q = ((Pair*)d->Data)[i].q;
            double sgn = q[3] < 0 ? -1 : 1, half = std::acos(std::clamp(std::fabs(q[3]), 0.0, 1.0)), s = std::sin(half);
            if (s < 1e-9) continue;
            double k = std::sin(half * kMagGripCurl) / s;
            for (int j = 0; j < 3; j++) q[j] *= sgn * k;
            q[3] = std::cos(half * kMagGripCurl);
        }
}

// x -> q (x - pivot) + pivot + move, as the part's relative transform on the gun's root (actor space).
static void PosePart(GUObject* part, Q q, Vec3 pivot, Vec3 move) {
    Vec3 r = Rotate(q, pivot);
    XfD x{{q.x, q.y, q.z, q.w}, {pivot.x - r.x + move.x, pivot.y - r.y + move.y, pivot.z - r.z + move.z}, 0, {1, 1, 1}, 0};
    Params p(Object(part), "K2_SetRelativeTransform");
    std::memcpy(p.Ptr("NewTransform"), &x, sizeof x);
    p.Set("bSweep", false).Set("bTeleport", true).Call();
}

// Actor space, cm (tools\prep, fitted), before the [Visuals] offset nudge.
static const Vec3 kTriggerPivot{-2.077, 0, -1.084}, kReleasePivot{2.131, 0, -4.240};
static constexpr double kTriggerPull = 10, kReleasePress = 12, kReleaseSpeed = 400;
static const char* kCarrierBones[] = {"Root_BoltCarrierGroup", "RootBoltCarrierGroup"};

// The charging handle's grab box: the receiver's ChargingHandle, a GunHandleComponent with the hand poses for
// racking it attached under it. It moves from the M110's T-handle onto the Barrett's knob, which sticks out of
// the right side of the receiver above the magazine (tools\prep: the carrier's outermost part).
static const Vec3 kHandleBox{19.70, 6.80, 5.60};

struct Rig {
    GUObject* gun;
    GUObject* mov;
    GUObject* part[kNumMoving];
    uint64_t carrierBone;
    Vec3 carrierRest;
    double release;  // animated angle, degrees
    double applied[kNumMoving][2];
    ULONGLONG tick;
    GUObject* handle = nullptr;  // the grab box, moved onto the Barrett's knob
    Vec3 handleFrom{}, handleShift{};
    int handleFixes = 0;  // times the game put it back on the M110's T-handle
};
static std::vector<Rig> s_rigs;

static Vec3 Nudged(Vec3 p) { return {p.x + s_offset[0], p.y + s_offset[1], p.z + s_offset[2]}; }

static void SetRelativeLocation(GUObject* c, Vec3 v) {
    Params p(Object(c), "K2_SetRelativeLocation");
    p.Set("NewLocation", v).Set("bSweep", false).Set("bTeleport", true).Call();
}

// Moves the grab box onto the Barrett's knob, with any copy the component keeps of where it sits (unreflected,
// past GrippableBoxComponent's properties: where its travel starts).
static GUObject* FitHandle(Object actor, Vec3& from, Vec3& shift) {
    Object h = actor.GetObj("ChargingHandle");
    if (!h) { Warn("{} has no ChargingHandle - the grab point stays the M110's", actor.Name()); return nullptr; }
    Vec3 now = h.Get<Vec3>("RelativeLocation"), to = Nudged(kHandleBox);
    Vec3 d{to.x - now.x, to.y - now.y, to.z - now.z};
    from = now;
    shift = d;
    int start = API->StructSize((GUStruct*)FindClass("GrippableBoxComponent")), end = API->StructSize(h.Struct());
    int copies = 0;
    for (int o = (start + 7) & ~7; start > 0 && o + 24 <= end; o += 8) {
        auto* v = (double*)((uint8_t*)h.ptr + o);
        if (std::fabs(v[0] - now.x) > 1e-6 || std::fabs(v[1] - now.y) > 1e-6 || std::fabs(v[2] - now.z) > 1e-6) continue;
        v[0] += d.x; v[1] += d.y; v[2] += d.z;
        Log("{}: the charging handle keeps its spot at +0x{:X} too - moved", actor.Name(), o);
        copies++;
        o += 16;
    }
    SetRelativeLocation(h.ptr, to);
    Vec3 got = h.Get<Vec3>("RelativeLocation");
    if (std::fabs(got.x - to.x) + std::fabs(got.y - to.y) + std::fabs(got.z - to.z) > 0.001)
        Error("{}: the charging handle grab point did not move (at {:.3f},{:.3f},{:.3f})", actor.Name(), got.x, got.y, got.z);
    Log("{}: charging handle grab point moved from ({:.2f},{:.2f},{:.2f}) onto the Barrett's knob ({} stored cop{})", actor.Name(), now.x,
        now.y, now.z, copies, copies == 1 ? "y" : "ies");
    return h.ptr;
}

// If the game puts the grab box back on the M110's T-handle (on the line its travel runs along), it goes back
// onto the Barrett's knob, wherever along the travel it is.
static void KeepHandle(Rig& r) {
    if (!r.handle || !API->IsValid(r.handle)) return;
    Vec3 d = r.handleShift, at = Object(r.handle).Get<Vec3>("RelativeLocation");
    if (std::fabs(d.y) + std::fabs(d.z) < 0.01 || std::fabs(at.y - r.handleFrom.y) > 0.01 || std::fabs(at.z - r.handleFrom.z) > 0.01) return;
    SetRelativeLocation(r.handle, {at.x + d.x, at.y + d.y, at.z + d.z});
    if (r.handleFixes++ % 1000 == 0)
        Log("{}: the game put the charging handle grab point back on the M110's (x{}) - moved onto the Barrett's again",
            Object(r.gun).Name(), r.handleFixes);
}

static void PoseRig(Rig& r) {
    Object gun(r.gun);
    ULONGLONG now = GetTickCount64();
    double dt = (std::min)(0.1, (now - r.tick) / 1000.0);
    r.tick = now;

    // the carrier: where the game has the M110's carrier bone now, against its rest
    Vec3 carrier{0, 0, 0};
    {
        Params bt(Object(r.mov), "GetBoneTransformByName");
        *(uint64_t*)bt.Ptr("BoneName") = r.carrierBone;
        bt.Set("BoneSpace", (uint8_t)1).Call();  // component space
        XfD x{};
        std::memcpy(&x, bt.Ptr("ReturnValue"), sizeof x);
        carrier = {x.t[0] - r.carrierRest.x, 0, 0};
    }
    double d = (gun.GetBool("bIsMagReleasePressed") ? kReleasePress : 0) - r.release, s = kReleaseSpeed * dt;
    r.release = std::fabs(d) <= s ? r.release + d : r.release + (d > 0 ? s : -s);
    double trigger = std::clamp((double)gun.Get<float>("CurrentTrigger"), 0.0, 1.0) * kTriggerPull;

    for (int i = 0; i < kNumMoving; i++) {
        if (!r.part[i]) continue;
        double a = 0;
        Q q{0, 0, 0, 1};
        Vec3 pivot{0, 0, 0}, move{0, 0, 0};
        switch (s_movingParts[i].kind) {
            case PartKind::Carrier: a = carrier.x; move = carrier; break;
            case PartKind::Trigger: a = trigger; q = AxisAngle(1, trigger); pivot = Nudged(kTriggerPivot); break;
            case PartKind::MagRelease: a = r.release; q = AxisAngle(1, r.release); pivot = Nudged(kReleasePivot); break;
        }
        if (std::fabs(a - r.applied[i][0]) < 1e-4) continue;
        r.applied[i][0] = a;
        PosePart(r.part[i], q, pivot, move);
    }
    KeepHandle(r);
}

static void PoseRigs() {  // every frame
    for (size_t i = 0; i < s_rigs.size();) {
        Rig& r = s_rigs[i];
        if (!API->IsValid(r.gun) || !API->IsValid(r.mov)) { s_rigs.erase(s_rigs.begin() + i); continue; }
        PoseRig(r);
        i++;
    }
}

static void HangMovingParts(Object actor) {
    Object mov = actor.GetObj("Movables");  // M110 moving parts: a skeletal mesh the gun code poses by bone name
    if (!mov) { Warn("{} has no Movables component - keeping the M110's moving parts", actor.Name()); return; }
    if (!XfLayoutOk()) return;
    Rig r{actor.ptr, mov.ptr, {}, 0, {}, 0, {}, GetTickCount64()};
    for (const char* bone : kCarrierBones) {
        Xf rest = RefPoseComponent(mov, bone);
        if (rest.empty()) continue;
        XfD x{};
        std::memcpy(&x, rest.data(), sizeof x);
        r.carrierBone = API->MakeName(bone);
        r.carrierRest = {x.t[0], x.t[1], x.t[2]};
        break;
    }
    if (!r.carrierBone) Warn("{}: no carrier bone on the M110's movables - the Barrett's carrier stays home", actor.Name());
    int hung = 0;
    for (int i = 0; i < kNumMoving; i++) {
        r.part[i] = s_movingParts[i].mesh ? API->AddDynamicMeshComponent(actor.ptr, s_movingParts[i].mesh, s_mids, kNumParts) : nullptr;
        for (auto& v : r.applied[i]) v = 1e9;  // posed on the first frame
        hung += r.part[i] != nullptr;
    }
    r.handle = FitHandle(actor, r.handleFrom, r.handleShift);
    s_rigs.push_back(r);
    if (r.carrierBone) PoseRig(s_rigs.back());
    // Stop drawing the M110's parts but keep the bones posed: the carrier's is read every frame.
    mov.Set("VisibilityBasedAnimTickOption", (uint8_t)0);  // AlwaysTickPoseAndRefreshBones
    HideComponent(mov);
    Log("{}: {} Barrett moving parts, posed by the plugin", actor.Name(), hung);
}

static void Mark(Object receiver);

static void DressAsBarrett(Object actor) {
    if (IsDressed(actor)) return;
    if (!BuildVisualAssets()) return;
    Object root = actor.GetObj("StaticMeshComponent");  // the M110 mesh; also collision + physics
    if (!root) { Error("{} has no StaticMeshComponent", actor.FullName()); return; }
    Object comp = API->AddDynamicMeshComponent(actor.ptr, s_mesh, s_mids, kNumParts);
    if (!comp) { Error("could not add the Barrett mesh to {}", actor.FullName()); return; }
    Object muzzle = s_muzzleMesh ? Object(API->AddDynamicMeshComponent(actor.ptr, s_muzzleMesh, s_mids, kNumParts)) : Object();
    Object sights = s_sightsMesh ? Object(API->AddDynamicMeshComponent(actor.ptr, s_sightsMesh, s_mids, kNumParts)) : Object();
    HideComponent(root);
    if (s_ownMovingParts) HangMovingParts(actor);
    if (s_fitAttachments) FitAttachPoints(actor);
    ConfigureGun(actor);
    s_dressedGuns.push_back({actor.ptr, muzzle.ptr, sights.ptr});
    DressMagazine(actor.GetObj("CurrentMagazine"), actor);
    Log("*** {} dressed as a Barrett M107A1: {}, {} rounds ***", actor.Name(), s_bmg && s_fireRoundLocal ? ".50 BMG" : "7.62 (the M110's)",
        s_capacity);
}

// Topmost actor this one is attached to (a magazine in a pouch on a player -> the player).
static GUObject* AttachRoot(Object a) {
    for (int guard = 0; guard < 16 && a; guard++) {
        Params p(a, "GetAttachParentActor");
        p.Call();
        Object up = p.Return<GUObject*>();
        if (!up) break;
        a = up;
    }
    return a.ptr;
}

// ------------------------------------------------------------------ multiplayer
//
// Each machine dresses guns itself, and only the one whose player picked the Barrett (or loaded it from a save
// slot) knows the M110 it got is one. It tells the others through the receiver's own ColorComponent: its
// ColorOption is an FName the game replicates to every player and keeps in a saved loadout. The plugin sets it
// to kMarker with the game's own colour RPC, and any machine with the plugin that sees an M110 receiver with
// that colour dresses it. A machine without the plugin ignores the unknown colour and shows an M110. Untested in
// a real multiplayer session.

static constexpr char kMarker[] = "BarrettM107";
static const char* kSelectHook =
    "WB_GunCustomization_C:BndEvt__WB_GunCustomization_WB_Database_K2Node_ComponentBoundEvent_34_OnItemSelected__DelegateSignature";
static std::map<GUObject*, GUObject*> s_picks;  // gun station -> gun last picked at it on this machine

static bool HasAuthority(Object actor) {
    Params p(actor, "HasAuthority");
    p.Call();
    return p.Return<bool>();
}

// A .50 that arrives on another machine. In multiplayer the host spawns every round (a client's shot reaches it as
// FireRoundServer, and the hook above makes it a .50 there, so its damage is right), and the other machines get a
// copy of it with only the replicated values: there it would be the M110's 7.62 with that round's mass, size and
// penetration, and fly and strike like one. A round of the Barrett's round class fired by the player who owns a
// Barrett gets the .50's values.
static void DressArrivedRound(Object round) {
    for (auto& d : s_dressedGuns) {
        Object gun(d.gun);
        if (!API->IsValid(gun.ptr) || gun.Get<GUClass*>("BulletData") != round.Class()) continue;
        if (!s_bmg || HasAuthority(round)) return;
        Params gi(round, "GetInstigator");
        gi.Call();
        Params go(gun, "GetOwner");
        go.Call();
        Params hc(gun, "GetHoldingCharacter");
        hc.Call();
        GUObject* by = gi.Return<GUObject*>();
        GUObject* holder = hc.Return<GUObject*>();  // the gun in the shooter's hands (else, unknown here: theirs)
        if (!by || (holder ? holder != by : go.Return<GUObject*>() != by)) continue;
        Object cdo = DefaultObject(round.Class());
        round.Set<float>("Mass", s_bmgMass / 1000.f);
        round.Set<float>("Diameter", 1.295f);
        round.Set<float>("MinPenetration", cdo.Get<float>("MinPenetration") * s_bmgPenetration);
        round.Set<float>("MaxPenetration", cdo.Get<float>("MaxPenetration") * s_bmgPenetration);
        round.SetBool("Shotgun", false);
        if (GUObject* curve = BmgDamageCurve()) round.Set<GUObject*>("DamageCurve", curve);
        return;
    }
}

static std::string ColorOf(Object receiver) {
    Object cc = receiver ? receiver.GetObj("ColorComponent") : Object();
    char b[128] = "";
    if (cc) API->NameToString(cc.Get<uint64_t>("ColorOption"), b, sizeof b);
    return b;
}

static bool IsMarked(Object receiver) {
    return receiver.Class() == s_receiverClass && _stricmp(ColorOf(receiver).c_str(), kMarker) == 0;
}

// This machine's player owns the actor (it was spawned for them, or they hold it): the server takes RPCs on it
// from here.
static bool LocallyOwned(Object actor) {
    Object o = actor;
    for (int guard = 0; guard < 4 && o; guard++) {
        if (o.IsA("Pawn")) {
            Params lc(o, "IsLocallyControlled");
            lc.Call();
            return lc.Return<bool>();
        }
        if (o.IsA("PlayerController")) {
            Params lc(o, "IsLocalController");
            lc.Call();
            return lc.Return<bool>();
        }
        Params go(o, "GetOwner");
        go.Call();
        o = go.Return<GUObject*>();
    }
    return false;
}

// Names the gun a Barrett for every player: on the server the colour applies at once and replicates; from the
// client that owns the gun it goes to the server first. At most once a second per gun.
static void Mark(Object receiver) {
    static std::map<GUObject*, ULONGLONG> asked;
    Object cc = receiver.GetObj("ColorComponent");
    if (!cc || IsMarked(receiver)) return;
    bool authority = HasAuthority(receiver);
    if (!authority && !LocallyOwned(receiver)) return;  // someone else's gun: not ours to name
    ULONGLONG now = GetTickCount64();
    if (auto it = asked.find(receiver.ptr); it != asked.end() && now - it->second < 1000) return;
    asked[receiver.ptr] = now;
    Params ac(cc, "ApplyColor_Server");
    ac.Set("ColorName", API->MakeName(kMarker)).Call();
    Log("{}: named a Barrett for every player{}", receiver.Name(), authority ? "" : " (asked the server)");
}

// Every quarter second: the magazine in each Barrett, the local player's spare magazines while they have a
// Barrett, each Barrett's muzzle brake (hidden while a muzzle device replaces it).
static void WatchDressedGuns() {
    static ULONGLONG next = 0;
    if (s_dressedGuns.empty() || GetTickCount64() < next) return;
    next = GetTickCount64() + 250;
    Object pawn;
    {
        Params p(Lib("GameplayStatics"), "GetPlayerPawn");
        p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
        pawn = p.Return<GUObject*>();
    }
    bool playerHasBarrett = false;
    GML_TArray* accepted = nullptr;
    for (size_t i = 0; i < s_dressedGuns.size();) {
        Object gun(s_dressedGuns[i].gun);
        if (!gun || !API->IsValid(gun.ptr) || !IsDressed(gun)) { s_dressedGuns.erase(s_dressedGuns.begin() + i); continue; }
        DressMagazine(gun.GetObj("CurrentMagazine"), gun);
        if (pawn && AttachRoot(gun) == pawn.ptr) playerHasBarrett = true;
        accepted = (GML_TArray*)gun.PropPtr("AcceptedMagazines");
        Mark(gun);  // repainted at a station since, or the first try didn't reach the server: name it again
        if (Object muzzle = s_dressedGuns[i].muzzle; muzzle && API->IsValid(muzzle.ptr)) {
            bool device = false;
            Params ga(gun, "GetAttachedActors");
            ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", false).Call();
            auto* arr = (GML_TArray*)ga.Ptr("OutActors");
            for (int k = 0; arr && k < arr->Num && !device; k++) {
                Object att(((GUObject**)arr->Data)[k]);
                char tag[64] = "";
                if (att.IsA("GunAttachment")) API->NameToString(att.Get<uint64_t>("AttachPointTag"), tag, sizeof tag);
                device = !strcmp(tag, "MuzzleAttachLocation") && att.GetBool("bAttached");
            }
            Params v(muzzle, "SetVisibility");
            v.Set("bNewVisibility", !device).Set("bPropagateToChildren", false).Call();
        }
        if (Object sights = s_dressedGuns[i].sights; sights && API->IsValid(sights.ptr)) {
            bool show = !OpticMounted(gun);
            if (sights.GetBool("bVisible") != show) {
                Params v(sights, "SetVisibility");
                v.Set("bNewVisibility", show).Set("bPropagateToChildren", false).Call();
                Log("{}: iron sights {}", gun.Name(), show ? "up (no optic)" : "hidden under the optic");
            }
        }
        i++;
    }
    for (size_t i = 0; i < s_dressedMags.size();) {  // keep their rounds hidden and their load 10
        if (!API->IsValid(s_dressedMags[i]) || !IsDressed(Object(s_dressedMags[i]))) { s_dressedMags.erase(s_dressedMags.begin() + i); continue; }
        HideRounds(s_dressedMags[i]);
        LoadMagazine(s_dressedMags[i]);
        i++;
    }
    if (playerHasBarrett && accepted && pawn) {
        for (int k = 0; k < accepted->Num; k++) {
            Object cls(((GUObject**)accepted->Data)[k]);
            for (auto& mag : FindAllOf(cls.Name().c_str()))
                if (AttachRoot(mag) == pawn.ptr) DressMagazine(mag, {});
        }
    }
}

// ------------------------------------------------------------------ loadouts (team room, operations)
//
// The game saves each carried gun as a GunSerializable in SaveGames\SavedGun_<slot>.sav, with its GunAsset
// stored as an object path, and rebuilds it from there at team-room start and in every operation. The Barrett's
// GunAsset is created at runtime, so its path only resolves in the session that wrote it. So once the game has
// saved the Barrett, the plugin re-saves that slot (with the game's own SaveGameToSlot) with the M110 donor as
// GunAsset - same receiver and attachments - and keeps the file's hash in its config. A loadout gun whose slot
// still holds exactly that file, or was saved with the Barrett's colour name, is the Barrett. Without the plugin
// the slot simply loads as an M110.

static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}

static std::wstring Widen8(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

static std::string ReadFileBytes(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD n = 0;
    if (!ReadFile(f, s.data(), (DWORD)s.size(), &n, nullptr)) n = 0;
    CloseHandle(f);
    s.resize(n);
    return s;
}

static std::string HashHex(const std::string& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : bytes) h = (h ^ c) * 0x100000001b3ull;
    return std::format("{:016x}-{}", h, bytes.size());
}

static std::wstring SaveGamesDir() {
    static std::wstring dir;
    if (dir.empty()) {
        Params p(Lib("KismetSystemLibrary"), "GetProjectSavedDirectory");
        p.Call();
        char b[1024] = "";
        if (auto* s = (const GML_FString*)p.Ptr("ReturnValue")) API->FStringToUtf8(s, b, sizeof b);
        if (*b) dir = Widen8(b) + L"SaveGames\\";
    }
    return dir;
}

// [Loadouts] <slot> = <hash of the file while it holds the Barrett>, in the plugin's .cfg.
static ConfigEntry<std::string> SlotRecord(const std::string& slot) {
    return Config.Bind<std::string>("Loadouts", slot.c_str(), "", "Written by the plugin: this save slot holds the Barrett (hash of its file).");
}
static std::string RecordedHash(const std::string& slot) { return SlotRecord(slot).Value(); }
static void RecordHash(const std::string& slot, const std::string& hash) { SlotRecord(slot).Set(hash); }

// A field of FGunSetupStruct inside a loaded GunSerializable.
static void* SetupField(Object save, const char* name) {
    GFProperty* setupProp = save ? API->FindProperty(save.Struct(), "GunSetup") : nullptr;
    GML_PropInfo setup{}, field{};
    if (!setupProp || !API->GetPropertyInfo(setupProp, &setup) || !setup.structType) return nullptr;
    GFProperty* fieldProp = API->FindProperty(setup.structType, name);
    if (!fieldProp || !API->GetPropertyInfo(fieldProp, &field)) return nullptr;
    return (uint8_t*)save.ptr + setup.offset + field.offset;
}
static GUObject** GunAssetField(Object save) { return (GUObject**)SetupField(save, "GunAsset"); }

// The receiver colour a setup was saved with: SaveGunSetup keeps the receiver's ColorOption, which the plugin
// sets to kMarker on every Barrett (see "multiplayer").
static bool SavedWithMarker(Object save) {
    auto* name = (uint64_t*)SetupField(save, "SelectedColor");
    char b[128] = "";
    if (name) API->NameToString(*name, b, sizeof b);
    return _stricmp(b, kMarker) == 0;
}

struct SavedGun { GUObject* asset; bool marked; };

// The gun a gear manager saved in slot `slotProp`, read with the game's own GetGunSetup.
static SavedGun SavedGunSetup(Object gear, const char* slotProp) {
    std::string slot = FStr8(gear, slotProp);
    if (slot.empty()) return {};
    Params gg(gear, "GetGunSetup");
    gg.SetString("SlotName", Widen8(slot.c_str())).Call();
    Object save = gg.Get<GUObject*>("GunSetup");
    GUObject** f = GunAssetField(save);
    return {f ? *f : nullptr, SavedWithMarker(save)};
}

// SaveGunSetup names a gun by the first GunAsset in AvailableGuns with the receiver's class, so a Barrett is
// normally saved as the M110 donor, with the Barrett's colour name; one saved as our runtime asset (this session)
// or recorded by its hash (re-saved by the plugin) is the Barrett too.
static bool IsBarrettSlot(const std::string& slot, SavedGun saved) {
    if (saved.asset && saved.asset == s_gun) return true;  // written this session, not re-saved yet
    Object donor = FindObject(kDonor);
    if (!saved.asset || saved.asset != donor.ptr) return false;
    if (saved.marked) return true;
    std::string h = RecordedHash(slot);
    return !h.empty() && h == HashHex(ReadFileBytes(SaveGamesDir() + Widen8(slot.c_str()) + L".sav"));
}

// Re-save any gun slot the game wrote with the Barrett (or, from an earlier session, with a runtime GunAsset path
// that no longer resolves) so it names the donor; remember the new file's hash.
static void WatchLoadouts() {
    static ULONGLONG next = 0;
    static std::map<std::wstring, uint64_t> seen;  // file -> last write time
    if (!s_gun || GetTickCount64() < next) return;
    next = GetTickCount64() + 1000;
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"SavedGun_*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        uint64_t stamp = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (seen[file] == stamp) continue;
        seen[file] = stamp;
        std::wstring stemW = file.substr(0, file.size() - 4);
        char slotBuf[260] = "";
        WideCharToMultiByte(CP_UTF8, 0, stemW.c_str(), -1, slotBuf, sizeof slotBuf, nullptr, nullptr);
        std::string slot = slotBuf;
        std::string raw = ReadFileBytes(dir + file);
        Params lg(Lib("GameplayStatics"), "LoadGameFromSlot");
        lg.SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
        Object save = lg.Return<GUObject*>();
        GUObject** asset = GunAssetField(save);
        if (!asset) continue;
        // A runtime GunAsset path in the file (saved while this plugin wasn't watching) is the Barrett only if the
        // slot held the Barrett before: other plugins (the Noveske, the Saiga) make runtime GunAssets too.
        size_t t = raw.find("/Engine/Transient.");
        bool staleRuntime = !*asset && t != std::string::npos && raw.find(".GunAsset_C_", t) != std::string::npos;
        bool staleBarrett = staleRuntime && !RecordedHash(slot).empty();
        if (*asset == s_gun || staleBarrett) {
            Object donor = FindObject(kDonor);
            if (!donor) continue;
            *asset = donor.ptr;
            Params sv(Lib("GameplayStatics"), "SaveGameToSlot");
            sv.SetObj("SaveGameObject", save).SetString("SlotName", stemW).Set("UserIndex", (int32_t)0).Call();
            if (!sv.Return<bool>()) { Error("could not re-save loadout slot {}", slot); continue; }
            std::string hash = HashHex(ReadFileBytes(dir + file));
            RecordHash(slot, hash);
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + file).c_str(), GetFileExInfoStandard, &a))
                seen[file] = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            Log("loadout slot {} holds the Barrett: re-saved with a stable reference{}", slot, staleBarrett ? " (repaired a save from an earlier session)" : "");
        } else if (!RecordedHash(slot).empty() && !IsBarrettSlot(slot, {*asset, SavedWithMarker(save)})) {
            RecordHash(slot, "");  // the game saved a different gun there since
            Log("loadout slot {} no longer holds the Barrett", slot);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// A receiver the gear manager rebuilt from your saved loadout (team room start, operations): it is ours if it
// became a player's PrimaryGun/SecondaryGun and that slot holds the Barrett.
static bool ResolveLoadout(Object actor) {
    for (auto& gear : FindAllOf("BP_GearManagerComponent_C")) {
        Object pawn(API->GetOuter(gear.ptr));
        if (pawn.IsA("Pawn")) {
            Params lc(pawn, "IsLocallyControlled");
            lc.Call();
            if (!lc.Return<bool>()) continue;
        }
        const char* slotProp = gear.GetObj("PrimaryGun").ptr == actor.ptr     ? "PrimaryGunSetupSlotName"
                               : gear.GetObj("SecondaryGun").ptr == actor.ptr ? "SecondaryGunSetupSlotName"
                                                                               : nullptr;
        if (!slotProp) continue;
        SavedGun saved = SavedGunSetup(gear, slotProp);
        std::string slot = FStr8(gear, "GunSetupSlotName") + "_" + FStr8(gear, slotProp);  // SavedGun_MainPrimary
        bool barrett = IsBarrettSlot(slot, saved);
        Log("{} loaded from {} ({}): saved gun '{}'{} -> {}", actor.Name(), slot, Object(API->GetOuter(gear.ptr)).Name(),
            saved.asset ? Object(saved.asset).Name() : std::string("?"), saved.marked ? " named a Barrett" : "", barrett ? "Barrett" : "not the Barrett");
        if (barrett) {
            DressAsBarrett(actor);
            Mark(actor);
        }
        return true;
    }
    return false;
}

// The spawner's menu items are WB_CustomizationItemGun widgets; the gun they stand for is their WeaponItem.
static GUObject* GunOf(GUObject* itemOrGun) {
    Object o(itemOrGun);
    if (o && o.IsA("WB_CustomizationItemGun_C")) return o.GetObj("WeaponItem").ptr;
    return itemOrGun;
}

static GUObject* s_lastPressedGun = nullptr;  // any spawner menu item pressed, most recent
static ULONGLONG s_lastPressedAt = 0;

// A gun station whose player is someone else's: this machine's menu presses say nothing about it.
static bool ForeignStation(Object station) {
    Object player = station.GetObj("Player");
    if (!player) return false;
    Params lc(player, "IsLocallyControlled");
    lc.Call();
    return !lc.Return<bool>();
}

// An M110 receiver began play. It is a Barrett if another machine named it one, if the gun station now holding it
// as CurrentReceiver was last asked for the Barrett entry on this machine, or if it is a loadout gun from a Barrett
// slot. The station sets CurrentReceiver right after the spawn returns, so this is checked on the following
// frames.
static void ResolvePending() {
    if (s_pending.empty()) return;
    auto stations = FindAllOf("CustomizationStation_Gun_C");
    for (size_t i = 0; i < s_pending.size();) {
        Pending& p = s_pending[i];
        Object actor(p.actor);
        bool done = !actor || !API->IsValid(actor.ptr);
        if (!done && IsMarked(actor)) {
            Log("{} is named a Barrett by the game it came from", actor.Name());
            DressAsBarrett(actor);
            done = true;
        }
        for (auto& st : stations) {
            if (done || st.GetObj("CurrentReceiver").ptr != p.actor) continue;
            done = true;
            if (ForeignStation(st)) break;
            GUObject* picked = nullptr;
            const char* how = "?";
            if (auto pk = s_picks.find(st.ptr); pk != s_picks.end()) { picked = pk->second; how = "selection"; }
            if (!picked && GetTickCount64() - s_lastPressedAt < 30000) { picked = s_lastPressedGun; how = "last menu press"; }
            Log("{} spawned at {}: picked '{}' (via {}) -> {}", actor.Name(), st.Name(), picked ? Object(picked).Name() : std::string("?"),
                how, picked == s_gun ? "Barrett" : "stock M110");
            if (picked == s_gun) {
                DressAsBarrett(actor);
                Mark(actor);
            }
        }
        if (!done) done = ResolveLoadout(actor);
        if (!done && GetTickCount64() - p.since > 5000) done = true;  // a world or enemy M110 stays one
        if (done) s_pending.erase(s_pending.begin() + i);
        else i++;
    }
}

// A level started: build the assets while it loads, and forget what was dressed (a new level's actors can reuse
// old addresses).
static void NewLevel() {
    BuildVisualAssets();
    BmgDamageCurve();
    std::erase_if(s_dressedGuns, [](const DressedGun& d) { return !API->IsValid(d.gun) || !IsDressed(Object(d.gun)); });
    std::erase_if(s_dressedMags, [](GUObject* m) { return !API->IsValid(m) || !IsDressed(Object(m)); });
    s_picks.clear();
}

// The level changed if the game state did (GML's WORLD_BEGIN_PLAY comes from the game mode, which only the host
// has; the game state is on every machine).
static void WatchLevel() {
    static ULONGLONG next = 0;
    static std::string level;
    if (GetTickCount64() < next) return;
    next = GetTickCount64() + 500;
    Params gs(Lib("GameplayStatics"), "GetGameState");
    gs.SetObj("WorldContextObject", Object(API->WorldContext())).Call();
    Object state = gs.Return<GUObject*>();
    if (!state) return;
    std::string now = state.FullName();
    if (now == level) return;
    level = now;
    NewLevel();
}

static void QueueReceiver(GUObject* actor) {
    for (auto& pe : s_pending)
        if (pe.actor == actor) return;
    s_pending.push_back({actor, GetTickCount64()});
}

static void InstallVisuals() {
    // The spawner's selection. On a client the station asks the server for the gun with the GunAsset it is
    // handed, and the server can't resolve this machine's runtime Barrett asset: for that call the menu item stands
    // for the M110 donor (same receiver), and this machine remembers the Barrett was picked.
    static Object swapped;
    Hook(kSelectHook,
         [](Object widget, GUFunction* fn, void* params) {
             Object item(Param<GUObject*>(fn, params, "Item"));
             if (!item || !item.IsA("WB_CustomizationItemGun_C")) return false;
             GUObject* gun = item.GetObj("WeaponItem").ptr;
             Object station = widget.GetObj("OwningStation");
             if (station) s_picks[station.ptr] = gun;
             if (gun != s_gun || !station || HasAuthority(station)) return false;
             Object donor = FindObject(kDonor);
             if (!donor) return false;
             item.Set<GUObject*>("WeaponItem", donor.ptr);
             swapped = item;
             Log("{}: asking the server for the M110 donor; this machine dresses it as the Barrett", station.Name());
             return false;
         },
         [](Object widget, GUFunction* fn, void* params) {
             if (swapped) {
                 swapped.Set<GUObject*>("WeaponItem", s_gun);
                 swapped = {};
             }
             Log("spawner selection in {}: {}", widget.Name(), Object(GunOf(Param<GUObject*>(fn, params, "Item"))).Name());
         });
    HookAfter("WB_CustomizationItemGun_C:BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature",
              [](Object item, GUFunction*, void*) {
                  if (GUObject* gun = GunOf(item.ptr)) {
                      s_lastPressedGun = gun;
                      s_lastPressedAt = GetTickCount64();
                  }
              });
    HookAfter("*:ReceiveBeginPlay", [](Object self, GUFunction*, void*) {
        if (!s_receiverClass) {
            Object donor = FindObject(kDonor);
            if (donor) s_receiverClass = donor.Get<GUClass*>("ReceiverClass");
        }
        if (s_receiverClass && self.Class() == s_receiverClass) QueueReceiver(self.ptr);
        else if (!s_dressedGuns.empty()) DressArrivedRound(self);
    });
    for (const char* f : {"ColorComponent:OnRep_ColorOption", "ColorComponent:ApplyColor_Server", "ColorComponent:ApplyColor"})
        HookAfter(f, [](Object cc, GUFunction*, void*) {
            Params o(cc, "GetOwner");
            o.Call();
            Object actor = o.Return<GUObject*>();
            if (!actor || !s_receiverClass || actor.Class() != s_receiverClass) return;
            if (IsDressed(actor) || !IsMarked(actor)) return;
            QueueReceiver(actor.ptr);
        });
    On(GML_EVENT_TICK, [](void*) {
        WatchLevel();
        ResolvePending();
        WatchLoadouts();
        WatchDressedGuns();
        PoseRigs();
    });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) { NewLevel(); });
}

// ------------------------------------------------------------------ entry

GML_PLUGIN("com.remarrow.barrett", "Barrett M107A1", BARRETT_VERSION);

GML_AWAKE() {
    Message(BARRETT_MODEL_CREDIT);
    s_capacity = std::clamp(Config.Bind("Gun", "Capacity", 10, "Rounds per magazine (the Barrett's holds 10).").Value(), 1, 100);
    s_recoilScale = std::clamp(Config.Bind("Gun", "RecoilScale", 2.5f, "Recoil per shot, as a share of the game's M110's.").Value(), 0.1f, 10.0f);
    s_bmg = Config.Bind("Round", "Enabled", true, "Fire .50 BMG (false = the M110's 7.62).").Value();
    s_bmgDamage = std::clamp(Config.Bind("Round", "Damage", 600.0f, "Damage at the muzzle (the game's 7.62x39 does 95).").Value(), 1.0f, 5000.0f);
    s_bmgVelocity = std::clamp(Config.Bind("Round", "Velocity", 853.0f, "Muzzle velocity, m/s.").Value(), 100.0f, 1500.0f);
    s_bmgMass = std::clamp(Config.Bind("Round", "Mass", 42.0f, "Bullet mass, grams.").Value(), 5.0f, 100.0f);
    s_bmgPenetration = std::clamp(Config.Bind("Round", "Penetration", 2.5f, "Penetration, as a multiple of the M110 round's.").Value(), 0.1f, 20.0f);
    s_offset[0] = Config.Bind("Visuals", "OffsetX", 0.0f, "cm, + = toward the muzzle (the model is fitted on the M110's grip)").Value();
    s_offset[1] = Config.Bind("Visuals", "OffsetY", 0.0f, "cm, + = to the gun's right").Value();
    s_offset[2] = Config.Bind("Visuals", "OffsetZ", 0.0f, "cm, + = up").Value();
    s_fitAttachments = Config.Bind("Visuals", "FitAttachments", true, "Move the M110's muzzle point and mounts onto the Barrett's.").Value();
    s_ownMovingParts = Config.Bind("Visuals", "OwnMovingParts", true, "The Barrett's own bolt carrier, trigger and mag release.").Value();
    s_ownMagazines = Config.Bind("Visuals", "OwnMagazines", true, "Magazines in a Barrett, and the spares you carry, look like the Barrett's.").Value();

    HookAfter("BP_GameInstance_C:GetItems", [](Object gi, GUFunction*, void*) { AddGun(gi); });
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void*) {
        if (Object gi = FindFirstOf("BP_GameInstance_C")) AddGun(gi);  // GetItems ran before we could see it
    });
    InstallVisuals();
    InstallBmg();
    return 0;
}
