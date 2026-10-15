// BarrettDev - automated in-game test of the Barrett M107A1 plugin. Not shipped; installed only for a run.
// Loads the team room and, at the gun station, through the spawner's own menu event:
//   1. spawns a stock KAC M110 and logs its layout (components, attach points, movables bones, magazine, charging
//      handle, round), to fit the Barrett's points on;
//   2. spawns the Barrett, holds it still in the air and logs it (meshes on, the M110's hidden, moving parts,
//      magazine, recoil), renders it with a scene capture of its own to [Test] ShotDir;
//   3. fires it through the game's own FireRoundLocal and logs every round that spawns (one .50 BMG), and that
//      the round's defaults are the M110's again afterwards;
//   4. pulls its charging handle (half, full, home) and renders the action;
//   5. saves it as a loadout gun and rebuilds the loadout, as an operation does.
#include <GML/GML.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>

using namespace gml;

GML_PLUGIN("com.remarrow.barrettdev", "BarrettDev", "0.1.0", {{"com.remarrow.barrett", "0.1.0", GML_DEPENDENCY_SOFT}});

struct Vec { double x, y, z; };
struct Rot { double pitch, yaw, roll; };

static const char* kItemPress =
    "BndEvt__WB_CustomizationItem_Button_46_K2Node_ComponentBoundEvent_1_OnButtonPressedEvent__DelegateSignature";
static const char* kConfirm =
    "BndEvt__WB_GunCustomization_WB_ConfirmWindow_K2Node_ComponentBoundEvent_0_OnConfirm__DelegateSignature";
static const char* kDonor = "/Game/Weapons/Data/DA_KAC_M110SASS.DA_KAC_M110SASS";

static std::string s_shotDir;
static Object s_station, s_widget, s_barrett, s_donor, s_gun, s_rt, s_cap, s_gear;
static int s_step = -1, s_wait = 0, s_fallbacks = 0;
static GUObject* s_before = nullptr;
static Object s_target;  // the entry being spawned
static Vec s_holdAt{};
static bool s_holding = false;
static int s_rounds = 0;  // rounds that began play since the last reset

static std::string TextOf(Object o, const char* prop) {
    char b[128] = "";
    if (void* t = o.PropPtr(prop)) API->TextToString(t, b, sizeof b);
    return b;
}
static std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string FStr8(Object o, const char* prop) {
    char b[256] = "";
    if (auto* s = (const GML_FString*)o.PropPtr(prop)) API->FStringToUtf8(s, b, sizeof b);
    return b;
}
static std::string NameStr(uint64_t n) {
    char b[96] = "";
    API->NameToString(n, b, sizeof b);
    return b;
}
static std::string ClassName(Object o) { return o ? Object((GUObject*)o.Class()).Name() : "-"; }
static Object Pawn() {
    Params p(Lib("GameplayStatics"), "GetPlayerPawn");
    p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("PlayerIndex", (int32_t)0).Call();
    return p.Return<GUObject*>();
}
static Vec ActorLocation(Object a) {
    Params p(a, "K2_GetActorLocation");
    p.Call();
    return p.Return<Vec>();
}
static std::vector<Object> Components(Object actor, const char* cls) {
    std::vector<Object> out;
    Params gc(actor, "K2_GetComponentsByClass");
    gc.Set("ComponentClass", FindClass(cls)).Call();
    auto* arr = (GML_TArray*)gc.Ptr("ReturnValue");
    for (int i = 0; arr && i < arr->Num; i++) out.push_back(((GUObject**)arr->Data)[i]);
    return out;
}
static bool Visible(Object c) {
    Params v(c, "IsVisible");
    v.Call();
    return v.Return<bool>();
}
static Vec ToActor(Object actor, Vec world) {
    Params gt(actor, "GetTransform");
    gt.Call();
    Params inv(Lib("KismetMathLibrary"), "InverseTransformLocation");
    GML_PropInfo ri;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(actor.Struct(), "GetTransform"), "ReturnValue"), &ri);
    std::memcpy(inv.Ptr("T"), gt.Ptr("ReturnValue"), ri.size);
    inv.Set("Location", world).Call();
    return inv.Return<Vec>();
}
static Vec CompLocation(Object c) {
    Params loc(c, "K2_GetComponentLocation");
    loc.Call();
    return loc.Return<Vec>();
}
static Rot CompRotationInActor(Object actor, Object c) {  // world rotation of c, the actor held unrotated
    Params r(c, "K2_GetComponentRotation");
    r.Call();
    return r.Return<Rot>();
}

// Hold the gun still in the air in front of our capture camera (the station eases it back to the table).
static void Hold() {
    if (!s_holding || !s_gun) return;
    Params mv(s_gun, "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", s_holdAt).Set("NewRotation", Rot{0, 0, 0}).Set("bSweep", false).Set("bTeleport", true).Call();
}
static void Freeze(Object gun) {
    if (Object root = gun.GetObj("StaticMeshComponent")) {
        Params p(root, "SetSimulatePhysics");
        p.Set("bSimulate", false).Call();
    }
}

static void EnsureCapture() {
    if (s_rt) return;
    Params rt(Lib("KismetRenderingLibrary"), "CreateRenderTarget2D");
    rt.SetObj("WorldContextObject", Object(API->WorldContext())).Set("Width", (int32_t)1800).Set("Height", (int32_t)800)
        .Set("Format", (uint8_t)2).Call();  // RTF_RGBA8
    s_rt = rt.Return<GUObject*>();
    Params mk(Lib("KismetMathLibrary"), "MakeTransform");
    mk.Set("Scale", Vec{1, 1, 1}).Call();
    GML_PropInfo xi;
    API->GetPropertyInfo(API->FindProperty((GUStruct*)API->FindFunction(Lib("KismetMathLibrary").Struct(), "MakeTransform"), "ReturnValue"), &xi);
    Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
    b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", FindClass("SceneCapture2D")).Set("CollisionHandlingOverride", (uint8_t)1);
    std::memcpy(b.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), xi.size);
    b.Call();
    Object actor = b.Return<GUObject*>();
    Params f(Lib("GameplayStatics"), "FinishSpawningActor");
    f.SetObj("Actor", actor);
    std::memcpy(f.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), xi.size);
    f.Call();
    s_cap = actor.GetObj("CaptureComponent2D");
    s_cap.Set<GUObject*>("TextureTarget", s_rt.ptr);
    s_cap.SetBool("bCaptureEveryFrame", false);
    s_cap.Set("FOVAngle", 40.0f);
}

// From `from` toward `at` (both actor space; the gun is held unrotated at s_holdAt), base colour, the gun and its
// magazine only.
static void CaptureFrom(const std::string& file, Vec from, Vec at) {
    EnsureCapture();
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);  // PRM_UseShowOnlyList
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        Params ga(s_gun, "GetAttachedActors");
        ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
        std::vector<Object> actors{s_gun, s_gun.GetObj("CurrentMagazine")};
        if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
            for (int i = 0; i < arr->Num; i++) actors.push_back(((GUObject**)arr->Data)[i]);
        for (Object a : actors)
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Vec f{s_holdAt.x + from.x, s_holdAt.y + from.y, s_holdAt.z + from.z}, t{s_holdAt.x + at.x, s_holdAt.y + at.y, s_holdAt.z + at.z};
    Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
    look.Set("Start", f).Set("Target", t).Call();
    Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", f).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
    s_cap.Set("CaptureSource", (uint8_t)7);  // SCS_BaseColor
    Params cs(s_cap, "CaptureScene");
    cs.Call();
    Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
    ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
        .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(file + ".png")).Call();
}
// The whole gun from its right, left and above, centred on X `cx`.
static void CaptureGun(const std::string& file, double cx) {
    CaptureFrom(file + "_right", {cx, 230, 10}, {cx, 0, 0});
    CaptureFrom(file + "_left", {cx, -230, 10}, {cx, 0, 0});
    CaptureFrom(file + "_top", {cx, 20, 230}, {cx, 0, 0});
    Log("SHOT {}", file);
}

// Everything on the receiver: components (class, parent, socket, where in actor space), movables bones,
// magazine, charging handle, the round.
static void DumpGun(const char* what) {
    Object g = s_gun;
    Log("DUMP [{}] {} class {} BulletData {} rpm {:.0f}/{:.0f} mode {} category? ", what, g.Name(), ClassName(g),
        Object((GUObject*)g.Get<GUClass*>("BulletData")).Name(), g.Get<float>("FireRateRPMMin"), g.Get<float>("FireRateRPMMax"),
        g.Get<uint8_t>("FireMode"));
    Vec rps = g.Get<Vec>("RotationPerShot"), lps = g.Get<Vec>("LocationPerShot"), mr = g.Get<Vec>("MaxRotation"), ml = g.Get<Vec>("MaxLocation");
    Log("DUMP   recoil per shot rot ({:.2f},{:.2f},{:.2f}) loc ({:.2f},{:.2f},{:.2f}) max rot ({:.2f},{:.2f},{:.2f}) loc ({:.2f},{:.2f},{:.2f}); sound {}",
        rps.x, rps.y, rps.z, lps.x, lps.y, lps.z, mr.x, mr.y, mr.z, ml.x, ml.y, ml.z, g.GetObj("AudioData").Name());
    for (auto& c : Components(g, "SceneComponent")) {
        Params sn(c, "GetAttachSocketName");
        sn.Call();
        Object parent = c.GetObj("AttachParent");
        Vec l = ToActor(g, CompLocation(c)), rel = c.Get<Vec>("RelativeLocation");
        Rot rr = c.Get<Rot>("RelativeRotation");
        std::string extra;
        if (c.IsA("StaticMeshComponent")) extra = " mesh " + c.GetObj("StaticMesh").Name();
        if (c.IsA("SkinnedMeshComponent")) extra = " mesh " + c.GetObj("SkeletalMesh").Name() + c.GetObj("SkinnedAsset").Name();
        if (c.IsA("BoxComponent")) { Vec e = c.Get<Vec>("BoxExtent"); extra += std::format(" box extent ({:.2f},{:.2f},{:.2f})", e.x, e.y, e.z); }
        Log("DUMP   {:<34} [{}] parent {} socket '{}' at ({:.2f},{:.2f},{:.2f}) rel ({:.2f},{:.2f},{:.2f}) rot ({:.1f},{:.1f},{:.1f}) visible {}{}",
            c.Name(), ClassName(c), parent.Name(), NameStr(sn.Return<uint64_t>()), l.x, l.y, l.z, rel.x, rel.y, rel.z, rr.pitch, rr.yaw, rr.roll,
            Visible(c), extra);
    }
    if (Object mov = g.GetObj("Movables")) {
        Params nb(mov, "GetNumBones");
        nb.Call();
        int n = nb.Return<int32_t>();
        std::string bones;
        for (int i = 0; i < n; i++) {
            Params bn(mov, "GetBoneName");
            bn.Set("BoneIndex", (int32_t)i).Call();
            uint64_t name = bn.Return<uint64_t>();
            Params bl(mov, "GetSocketLocation");
            *(uint64_t*)bl.Ptr("InSocketName") = name;
            bl.Call();
            Vec l = ToActor(g, bl.Return<Vec>());
            bones += std::format(" {}({:.2f},{:.2f},{:.2f})", NameStr(name), l.x, l.y, l.z);
        }
        Log("DUMP   movables {} bones:{}", n, bones);
    }
    if (GFProperty* hs = API->FindProperty(g.Struct(), "ChargingHandleSettings")) {
        GML_PropInfo si{};
        API->GetPropertyInfo(hs, &si);
        std::string f;
        for (GFProperty* p = si.structType ? API->NextProperty(si.structType, nullptr) : nullptr; p; p = API->NextProperty(si.structType, p)) {
            GML_PropInfo i{};
            char name[64] = "";
            API->GetPropertyInfo(p, &i);
            API->GetPropertyName(p, name, sizeof name);
            if (!strcmp(i.type, "FloatProperty")) f += std::format(" {}={:.3f}", name, *(float*)((uint8_t*)g.ptr + si.offset + i.offset));
        }
        Log("DUMP   ChargingHandleSettings:{}", f);
    }
    Object mag = g.GetObj("CurrentMagazine");
    if (mag) {
        Vec ml2 = ToActor(g, ActorLocation(mag));
        Log("DUMP   magazine {} [{}] capacity {} ammo {} BulletData {} at ({:.2f},{:.2f},{:.2f})", mag.Name(), ClassName(mag), mag.Get<int32_t>("AmmoCapacity"),
            mag.Get<int32_t>("CurrentAmmo"), Object((GUObject*)mag.Get<GUClass*>("BulletData")).Name(), ml2.x, ml2.y, ml2.z);
        for (auto& c : Components(mag, "SceneComponent")) {
            Vec l = ToActor(g, CompLocation(c));
            Log("DUMP     {:<30} [{}] at ({:.2f},{:.2f},{:.2f}) visible {}", c.Name(), ClassName(c), l.x, l.y, l.z, Visible(c));
        }
    }
    auto* acc = (GML_TArray*)g.PropPtr("AcceptedMagazines");
    for (int i = 0; acc && i < acc->Num; i++) Log("DUMP   accepts {}", Object(((GUObject**)acc->Data)[i]).Name());
    Params ga(g, "GetAttachedActors");
    ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
    if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
        for (int i = 0; i < arr->Num; i++) {
            Object a(((GUObject**)arr->Data)[i]);
            Vec l = ToActor(g, ActorLocation(a));
            Log("DUMP   attached actor {} [{}] at ({:.2f},{:.2f},{:.2f}) tag '{}'", a.Name(), ClassName(a), l.x, l.y, l.z,
                a.IsA("GunAttachment") ? NameStr(a.Get<uint64_t>("AttachPointTag")) : std::string());
        }
    if (GUClass* round = g.Get<GUClass*>("BulletData")) {
        Object d = DefaultObject(round);
        Log("DUMP   round defaults {}: Mass {:.4f} Diameter {:.3f} velocity {:.0f}-{:.0f} FormFactor {:.3f} penetration {:.2f}-{:.2f} pooling {} DamageCurve {}",
            Object((GUObject*)round).Name(), d.Get<float>("Mass"), d.Get<float>("Diameter"), d.Get<float>("MuzzleVelocityMin"),
            d.Get<float>("MuzzleVelocityMax"), d.Get<float>("FormFactor"), d.Get<float>("MinPenetration"), d.Get<float>("MaxPenetration"),
            d.GetBool("EnablePooling"), d.GetObj("DamageCurve").Name());
    }
}

// Opens a category's page as the VR pointer does: its button's bound event (which calls the station's
// ShowWeapons with the category). 2 = Snipers, the M110's.
static void OpenCategory(uint8_t category) {
    static const char* buttons[] = {
        "BndEvt__WB_GunCustomization_CategoryBtn_Body_K2Node_ComponentBoundEvent_24_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Submachines_K2Node_ComponentBoundEvent_25_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Snipers_K2Node_ComponentBoundEvent_28_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Shotguns_K2Node_ComponentBoundEvent_27_OnPressed__DelegateSignature",
        "BndEvt__WB_GunCustomization_CategoryBtn_Pistols_K2Node_ComponentBoundEvent_26_OnPressed__DelegateSignature",
    };
    if (category >= std::size(buttons)) { Error("no button for category {}", category); return; }
    Params b(s_widget, buttons[category]);
    b.Call();
}

static bool Select(Object asset) {
    s_before = s_station.GetObj("CurrentReceiver").ptr;
    s_target = asset;
    s_fallbacks = 0;
    Object item;
    std::string seen;
    for (auto& it : FindAllOf("WB_CustomizationItemGun_C")) {
        seen += TextOf(it.GetObj("WeaponItem"), "DisplayName") + ", ";
        if (it.GetObj("WeaponItem").ptr == asset.ptr) item = it;
    }
    Log("menu items: {}", seen);
    if (!item) { Error("no menu item for '{}'", TextOf(asset, "DisplayName")); return false; }
    Params press(item, kItemPress);
    press.Call();
    Log("pressed menu item {} for '{}'", item.Name(), TextOf(asset, "DisplayName"));
    return true;
}

// Waits for the station's new receiver (confirming, then the station's own SpawnWeapon if the request was gated:
// no headset at the station). Returns it once there.
static Object Spawned() {
    Object cur = s_station.GetObj("CurrentReceiver");
    if (cur && cur.ptr != s_before) return cur;
    if (s_fallbacks == 0) {
        Log("no new receiver yet - confirming");
        Params c(s_widget, kConfirm);
        c.Call();
    } else if (s_fallbacks < 3) {
        Log("FALLBACK: calling the station's SpawnWeapon");
        Params sp(s_station, "SpawnWeapon");
        sp.SetObj("GunAsset", s_target).Call();
    } else {
        Error("spawn did not happen");
        s_step = 99;
    }
    s_fallbacks++;
    s_wait = 90;
    return {};
}

static void SetHandle(float progress) {
    Object h = s_gun.GetObj("ChargingHandle");
    if (!h) return;
    if (h.GetBool("bIsLocked")) {
        Params u(h, "UnlockHandle");
        u.Call();
    }
    Params p(h, "SetProgress");
    p.Set("Progress", progress).Call();
    s_gun.Set("CurrentBoltProgress", progress);
}
static void LogParts(const char* when) {
    static const char* names[] = {"body", "muzzle", "sights", "carrier", "trigger", "mag release"};
    auto parts = Components(s_gun, "DynamicMeshComponent");
    Object h = s_gun.GetObj("ChargingHandle");
    Vec box = h ? ToActor(s_gun, CompLocation(h)) : Vec{};
    Log("ACTION [{}] FireMode {} CurrentTrigger {:.2f} release {} bolt progress {:.2f}; handle progress {:.2f} box at ({:.2f},{:.2f},{:.2f})", when,
        s_gun.Get<uint8_t>("FireMode"), s_gun.Get<float>("CurrentTrigger"), s_gun.GetBool("bIsMagReleasePressed"), s_gun.Get<float>("CurrentBoltProgress"),
        h ? h.Get<float>("CurrentProgress") : -1.f, box.x, box.y, box.z);
    for (size_t i = 3; i < parts.size() && i < std::size(names); i++) {
        Vec l = parts[i].Get<Vec>("RelativeLocation");
        Rot r = parts[i].Get<Rot>("RelativeRotation");
        Log("ACTION   {:<12} location ({:7.3f},{:7.3f},{:7.3f}) rotation pitch {:7.2f} yaw {:6.2f} roll {:7.2f}", names[i], l.x, l.y, l.z, r.pitch,
            r.yaw, r.roll);
    }
}
static void LogRoundDefaults(const char* when) {
    GUClass* cls = s_gun ? s_gun.Get<GUClass*>("BulletData") : nullptr;
    if (!cls) return;
    Object d = DefaultObject(cls);
    Log("ROUND DEFAULTS [{}] {} Mass {:.4f} Diameter {:.3f} velocity {:.0f}-{:.0f} penetration {:.2f}-{:.2f} pooling {} DamageCurve {}", when,
        Object((GUObject*)cls).Name(), d.Get<float>("Mass"), d.Get<float>("Diameter"), d.Get<float>("MuzzleVelocityMin"), d.Get<float>("MuzzleVelocityMax"),
        d.Get<float>("MinPenetration"), d.Get<float>("MaxPenetration"), d.GetBool("EnablePooling"), d.GetObj("DamageCurve").Name());
}

// ---- the player's own hands on a spare Barrett magazine, through the game's grip as in VR: the body's hand bones
// against the magazine (the hand you see in VR), at several finger curls
static Object s_gripCtrl, s_gripMag, s_gripCam;
static Vec s_gripAt{};
static bool s_gripHold = false, s_gripOnly = false;
static std::string s_gripSide = "_l", s_curlsCfg;
static std::vector<double> s_curls;
static size_t s_curlIdx = 0;
static double s_curNow = 1.0;  // the curl the magazine's grip has now (1 = as the plugin makes it)
static int s_spawnedMags = 0;

struct XfD { double q[4]; double t[3]; double pad0; double s[3]; double pad1; };
static XfD CompXf(Object c) {
    Params p(c, "K2_GetComponentToWorld");
    p.Call();
    XfD x;
    std::memcpy(&x, p.Ptr("ReturnValue"), sizeof x);
    return x;
}
static Vec XfLoc(const XfD& t, Vec p, bool inverse) {
    Params f(Lib("KismetMathLibrary"), inverse ? "InverseTransformLocation" : "TransformLocation");
    std::memcpy(f.Ptr("T"), &t, sizeof t);
    f.Set("Location", p).Call();
    return f.Return<Vec>();
}
static Vec BodyBone(const char* bone) {
    Params gl(Pawn().GetObj("Mesh"), "GetSocketLocation");
    *(uint64_t*)gl.Ptr("InSocketName") = API->MakeName(bone);
    gl.Call();
    return gl.Return<Vec>();
}
// The hand as spheres: each finger from its first knuckle to its tip (carried past the last joint by 0.85 of the
// middle segment), 8.5 mm thick (thumb 10), the palm along the metacarpals 13 mm; 15% thicker for the glove. In the
// magazine mesh's frame: the Barrett's magazine is drawn in the gun's space as seated (tools\prep).
struct Ball { Vec c; double r; const char* part; };
static std::vector<Ball> BodyBalls() {
    XfD frame = CompXf(Components(s_gripMag, "DynamicMeshComponent")[0]);
    auto at = [&](const std::string& b) { return XfLoc(frame, BodyBone((b + s_gripSide).c_str()), true); };
    std::vector<Ball> balls;
    auto seg = [&](Vec a, Vec b, double r, const char* part) {
        for (int k = 0; k <= 4; k++) balls.push_back({{a.x + (b.x - a.x) * k / 4, a.y + (b.y - a.y) * k / 4, a.z + (b.z - a.z) * k / 4}, r, part});
    };
    const double thick = 1.15;
    Vec wrist = at("hand");
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky"}) {
        bool thumb = !strcmp(f, "thumb");
        Vec j1 = at(std::format("{}_01", f)), j2 = at(std::format("{}_02", f)), j3 = at(std::format("{}_03", f));
        Vec tip{j3.x + (j3.x - j2.x) * 0.85, j3.y + (j3.y - j2.y) * 0.85, j3.z + (j3.z - j2.z) * 0.85};
        double r = (thumb ? 0.5 : 0.425) * thick;
        seg(j1, j2, r, f); seg(j2, j3, r, f); seg(j3, tip, r * 0.9, f);
        if (!thumb) seg(wrist, j1, 0.65 * thick, "palm");
    }
    return balls;
}
// The Barrett's magazine as a box (gun space as seated, tools\prep): how far a ball reaches into it (cm, + = in).
static constexpr double kMagX0 = 3.0, kMagX1 = 17.8, kMagY0 = -1.97, kMagY1 = 2.14, kMagZ0 = -12.6, kMagZ1 = 2.75;
static double Into(const Ball& b) {
    const Vec& p = b.c;
    double ox = (std::max)({kMagX0 - p.x, p.x - kMagX1, 0.0}), oy = (std::max)({kMagY0 - p.y, p.y - kMagY1, 0.0}),
           oz = (std::max)({kMagZ0 - p.z, p.z - kMagZ1, 0.0});
    double out = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (out == 0) out = -(std::min)({p.x - kMagX0, kMagX1 - p.x, p.y - kMagY0, kMagY1 - p.y, p.z - kMagZ0, kMagZ1 - p.z});
    return b.r - out;
}
// Turns each of the socket's finger deltas by k times its angle (FBPVRHandPoseBonePair: name, pad, FQuat).
static void ScaleDeltas(Object sock, double k) {
    struct Pair { uint64_t bone, pad; double q[4]; uint8_t rest[0x10]; };
    auto* d = (GML_TArray*)sock.PropPtr("CustomPoseDeltas");
    for (int i = 0; d && i < d->Num && k != 1.0; i++) {
        double* q = ((Pair*)d->Data)[i].q;
        double sgn = q[3] < 0 ? -1 : 1, half = std::acos(std::clamp(std::fabs(q[3]), 0.0, 1.0)), s = std::sin(half);
        if (s < 1e-9) continue;
        double f = std::sin(half * k) / s;
        for (int j = 0; j < 3; j++) q[j] *= sgn * f;
        q[3] = std::cos(half * k);
    }
}
// A VRExpansion slot grip, as the game's own: the magazine's hand socket onto the controller.
static bool SlotGrip(Object ctrl, Object mag) {
    Object sock = mag.GetObj("HandSocket");
    Params hs(sock, "GetHandSocketTransform");
    hs.SetObj("QueryController", ctrl).Set("bIgnoreOnlySnapMesh", false).Call();
    Params gt(mag, "GetTransform");
    gt.Call();
    Params rel(Lib("KismetMathLibrary"), "MakeRelativeTransform");
    std::memcpy(rel.Ptr("A"), gt.Ptr("ReturnValue"), sizeof(XfD));
    std::memcpy(rel.Ptr("RelativeTo"), hs.Ptr("ReturnValue"), sizeof(XfD));
    rel.Call();
    Params g(ctrl, "GripObjectByInterface");
    g.SetObj("ObjectToGrip", mag);
    std::memcpy(g.Ptr("WorldOffset"), rel.Ptr("ReturnValue"), sizeof(XfD));
    *(uint64_t*)g.Ptr("OptionalSnapToSocketName") = API->MakeName(sock.Name().c_str());
    g.Set("bWorldOffsetIsRelative", true).Set("bIsSlotGrip", true).Call();
    return g.Return<bool>();
}
static void DropGrip() {
    Params d(s_gripCtrl, "DropObjectByInterface");
    d.SetObj("ObjectToDrop", s_gripMag).Call();
}
// The holding controller 25 cm forward, 30 down and 15 out to its side of the head: where the body's arm reaches
// its IK target (further out, the hand stops short of where it is in VR).
static void SetHoldPoint() {
    Object pawn = Pawn();
    Params fw(pawn, "GetActorForwardVector");
    fw.Call();
    Params rt(pawn, "GetActorRightVector");
    rt.Call();
    Vec f = fw.Return<Vec>(), r = rt.Return<Vec>(), c = CompLocation(s_gripCam);
    double sgn = s_gripSide == "_l" ? -1 : 1;
    s_gripAt = {c.x + f.x * 25 + r.x * 15 * sgn, c.y + f.y * 25 + r.y * 15 * sgn, c.z - 30};
}
static void HoldGrip() {
    if (!s_gripHold || !s_gripCtrl) return;
    Params mv(s_gripCtrl, "K2_SetWorldLocationAndRotation");
    mv.Set("NewLocation", s_gripAt).Set("NewRotation", Rot{0, 0, 0}).Set("bSweep", false).Set("bTeleport", true).Call();
}
// From `from` toward `at` (world), base colour, only these actors.
static void CaptureWorld(const std::string& file, std::initializer_list<Object> actors, Vec from, Vec at) {
    EnsureCapture();
    s_cap.Set<uint8_t>("PrimitiveRenderMode", 2);
    if (auto* only = (GML_TArray*)s_cap.PropPtr("ShowOnlyActors")) {
        only->Num = 0;
        for (Object a : actors)
            if (a) *(GUObject**)API->ArrayAdd(only, sizeof(GUObject*), 1) = a.ptr;
    }
    Params look(Lib("KismetMathLibrary"), "FindLookAtRotation");
    look.Set("Start", from).Set("Target", at).Call();
    Params mv(Object(API->GetOuter(s_cap.ptr)), "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", from).Set("NewRotation", look.Return<Rot>()).Set("bSweep", false).Set("bTeleport", true).Call();
    s_cap.Set("CaptureSource", (uint8_t)7);
    Params cs(s_cap, "CaptureScene");
    cs.Call();
    Params ex(Lib("KismetRenderingLibrary"), "ExportRenderTarget");
    ex.SetObj("WorldContextObject", Object(API->WorldContext())).SetObj("TextureRenderTarget", s_rt)
        .SetString("FilePath", W(s_shotDir)).SetString("Filename", W(file + ".png")).Call();
}
// The hand and the magazine: from the fingertips, both sides of the palm, and from the magazine's own sides and top.
static void GripViews(const std::string& prefix) {
    const std::string& s = s_gripSide;
    Vec w = BodyBone(("hand" + s).c_str()), m = BodyBone(("middle_01" + s).c_str()), i1 = BodyBone(("index_01" + s).c_str()),
        p1 = BodyBone(("pinky_01" + s).c_str());
    auto norm = [](Vec v) { double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return Vec{v.x / l, v.y / l, v.z / l}; };
    Vec f = norm({m.x - w.x, m.y - w.y, m.z - w.z}), side = norm({i1.x - p1.x, i1.y - p1.y, i1.z - p1.z});
    Vec n = norm({f.y * side.z - f.z * side.y, f.z * side.x - f.x * side.z, f.x * side.y - f.y * side.x});
    Vec at{w.x + f.x * 6, w.y + f.y * 6, w.z + f.z * 6};
    Object pawn = Pawn();
    for (auto [name, dir] : std::initializer_list<std::pair<const char*, Vec>>{{"tips", f}, {"palmA", n}, {"palmB", {-n.x, -n.y, -n.z}}})
        CaptureWorld("grip_" + prefix + "_" + name, {pawn, s_gripMag}, {at.x + dir.x * 45, at.y + dir.y * 45, at.z + dir.z * 45}, at);
    XfD mx = CompXf(Components(s_gripMag, "DynamicMeshComponent")[0]);
    Vec c = XfLoc(mx, {10.4, 0, -5}, false);
    for (auto [name, local] : std::initializer_list<std::pair<const char*, Vec>>{
             {"magleft", {10.4, -45, -5}}, {"magright", {10.4, 45, -5}}, {"magfront", {55, 0, -5}}, {"magtop", {10.4, 0, 40}}})
        CaptureWorld("grip_" + prefix + "_" + name, {pawn, s_gripMag}, XfLoc(mx, local, false), c);
    Log("SHOT grip_{}", prefix);
}
// The hand on the magazine now: how far each part reaches into it, the hand's bounds (magazine/gun space), and the
// balls for tools (index, part, centre, radius).
static void MeasureGrip() {
    auto balls = BodyBalls();
    std::string parts;
    double worst = -1e9;
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "palm"}) {
        double w = -1e9;
        for (auto& b : balls)
            if (!strcmp(b.part, f)) w = (std::max)(w, Into(b));
        worst = (std::max)(worst, w);
        parts += std::format(" {} {:.0f}", f, w * 10);
    }
    Vec lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    for (auto& b : balls) {
        lo = {(std::min)(lo.x, b.c.x), (std::min)(lo.y, b.c.y), (std::min)(lo.z, b.c.z)};
        hi = {(std::max)(hi.x, b.c.x), (std::max)(hi.y, b.c.y), (std::max)(hi.z, b.c.z)};
    }
    Object pawn = Pawn();
    bool left = s_gripSide == "_l";
    Object parent = pawn.GetObj(left ? "LeftHandIKTargetParent" : "RightHandIKTargetParent");
    XfD rel = pawn.Get<XfD>(left ? "LeftHandIKTargetRelativeTransform" : "RightHandIKTargetRelativeTransform");
    Vec ik = parent ? XfLoc(CompXf(parent), {rel.t[0], rel.t[1], rel.t[2]}, false) : Vec{}, hand = BodyBone(left ? "hand_l" : "hand_r");
    double reach = std::sqrt((ik.x - hand.x) * (ik.x - hand.x) + (ik.y - hand.y) * (ik.y - hand.y) + (ik.z - hand.z) * (ik.z - hand.z));
    Log("GRIP curl {:.2f}: your {} hand reaches {:.0f} mm into the magazine (mm by part:{}); hand X[{:.1f}..{:.1f}] Y[{:.1f}..{:.1f}] Z[{:.1f}..{:.1f}]; "
        "hand bone {:.1f} cm from its IK target", s_curNow, left ? "left" : "right", worst * 10, parts, lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, reach);
    if (FILE* f = fopen(std::format("{}\\grip_balls{}_c{:.0f}.csv", s_shotDir, s_gripSide, s_curNow * 100).c_str(), "w")) {
        for (size_t i = 0; i < balls.size(); i++) fprintf(f, "%zu,%s,%.4f,%.4f,%.4f,%.4f\n", i, balls[i].part, balls[i].c.x, balls[i].c.y, balls[i].c.z, balls[i].r);
        fclose(f);
    }
    if (s_curlIdx == 0) {
        Object sock = s_gripMag.GetObj("HandSocket");
        Vec sl = sock.Get<Vec>("RelativeLocation");
        Rot sr = sock.Get<Rot>("RelativeRotation");
        Params gp(sock, "GetAttachParent");
        gp.Call();
        Object dm = Components(s_gripMag, "DynamicMeshComponent")[0];
        Vec dl = dm.Get<Vec>("RelativeLocation");
        Rot dr = dm.Get<Rot>("RelativeRotation");
        Log("GRIPFRAME socket on {} at ({:.4f},{:.4f},{:.4f}) rot (P {:.3f} Y {:.3f} R {:.3f}); magazine mesh at ({:.4f},{:.4f},{:.4f}) rot (P {:.3f} Y {:.3f} R {:.3f})",
            Object(gp.Return<GUObject*>()).Name(), sl.x, sl.y, sl.z, sr.pitch, sr.yaw, sr.roll, dl.x, dl.y, dl.z, dr.pitch, dr.yaw, dr.roll);
    }
}

static void Step() {
    if (s_wait > 0) { s_wait--; return; }
    switch (s_step) {
        case 0: {
            auto stations = FindAllOf("CustomizationStation_Gun_C");
            Object pawn = Pawn();
            Vec me = ActorLocation(pawn);
            double best = 1e30;
            for (auto& st : stations) {
                Vec l = ActorLocation(st);
                double d = (l.x - me.x) * (l.x - me.x) + (l.y - me.y) * (l.y - me.y);
                if (d < best && st.GetObj("WBGunBuilder")) { best = d; s_station = st; }
            }
            s_widget = s_station.GetObj("WBGunBuilder");
            for (auto& g : FindAllOf("GunAsset_C"))
                if (TextOf(g, "DisplayName") == "Barrett M107A1") s_barrett = g;
            s_donor = FindObject(kDonor);
            Log("{} station(s); using {}; widget {}; barrett entry {} (category {}, icon {}); donor {} '{}' (category {})", stations.size(),
                s_station.Name(), s_widget.Name(), s_barrett.Name(), s_barrett.Get<uint8_t>("Category"), s_barrett.GetObj("Icon").Name(), s_donor.Name(),
                TextOf(s_donor, "DisplayName"), s_donor.Get<uint8_t>("Category"));
            if (!s_widget || !s_barrett || !s_donor) { Error("missing pieces"); s_step = 99; return; }
            Params sl(s_station, "K2_GetActorLocation");
            sl.Call();
            Params mv(pawn, "K2_SetActorLocation");
            mv.Set("NewLocation", sl.Return<Vec>()).Set("bSweep", false).Set("bTeleport", true).Call();
            OpenCategory(s_donor.Get<uint8_t>("Category"));
            s_step = s_gripOnly ? 11 : 1; s_wait = 60;  // [Test] GripOnly: the Barrett, its loadout, then the hands
            return;
        }
        // ---- a stock M110, to read its layout
        case 1:
            if (!Select(s_donor)) { s_step = 10; return; }
            s_step = 2; s_wait = 90;
            return;
        case 2: {
            Object cur = Spawned();
            if (!cur) return;
            s_gun = cur;
            Freeze(s_gun);
            Vec at = ActorLocation(Pawn());
            s_holdAt = {at.x, at.y, at.z + 120};
            s_holding = true;
            s_step = 3; s_wait = 240;
            return;
        }
        case 3:
            DumpGun("stock M110");
            CaptureGun("m110", 13);
            s_holding = false;
            s_step = 10;
            return;
        // ---- the Barrett
        case 10:
            OpenCategory(s_donor.Get<uint8_t>("Category"));
            s_step = 11; s_wait = 30;
            return;
        case 11:
            if (!Select(s_barrett)) { s_step = 99; Log("DEV TEST COMPLETE"); return; }
            s_step = 12; s_wait = 90;
            return;
        case 12: {
            Object cur = Spawned();
            if (!cur) return;
            s_gun = cur;
            Freeze(s_gun);
            Vec at = ActorLocation(Pawn());
            s_holdAt = {at.x, at.y, at.z + 120};
            s_holding = true;
            Log("spawned {} - holding it at ({:.0f},{:.0f},{:.0f})", s_gun.FullName(), s_holdAt.x, s_holdAt.y, s_holdAt.z);
            s_step = 13; s_wait = 240;  // dressing, then the magazine after two steady checks
            return;
        }
        case 13:
            DumpGun("Barrett");
            LogParts("dressed");
            LogRoundDefaults("before firing");
            CaptureGun("barrett", 32);
            CaptureFrom("barrett_action", {12, 70, 5}, {12, 0, 0});
            s_step = s_gripOnly ? 20 : 40; s_wait = 10;
            return;
        // ---- an optic from the station's menu, as a player picks one: where it mounts, the iron sights
        case 40: {
            Object btn = s_widget.GetObj("Optics");
            uint8_t cat = btn ? btn.Get<uint8_t>("Category") : 0;
            Log("OPTIC button {} [{}] category {}", btn.Name(), ClassName(btn), cat);
            Params p(s_widget, "BndEvt__WB_GunCustomization_Optics_K2Node_ComponentBoundEvent_32_OnPressed__DelegateSignature");
            p.Set("Category", cat).Call();
            s_step = 41; s_wait = 60;
            return;
        }
        case 41: {
            Object pick;
            for (auto& it : FindAllOf("WB_CustomizationItemGun_C")) {
                if (Object w = it.GetObj("WeaponItem"); w && w.IsA("GunAsset_C")) continue;  // a gun, not an attachment
                std::string refs;
                for (GFProperty* pr = API->NextProperty(it.Struct(), nullptr); pr; pr = API->NextProperty(it.Struct(), pr)) {
                    GML_PropInfo i{};
                    char name[64] = "";
                    if (!API->GetPropertyInfo(pr, &i) || (strcmp(i.type, "ObjectProperty") && strcmp(i.type, "ClassProperty"))) continue;
                    API->GetPropertyName(pr, name, sizeof name);
                    Object v(*(GUObject**)((uint8_t*)it.ptr + i.offset));
                    if (v && !v.IsA("Widget")) refs += std::format(" {}={}", name, v.Name());
                }
                Params vis(it, "IsVisible");
                vis.Call();
                Log("OPTIC item {} [{}] visible {} outer {}:{}", it.Name(), ClassName(it), vis.Return<bool>(), Object(API->GetOuter(it.ptr)).Name(), refs);
                if (!pick && vis.Return<bool>() && !refs.empty()) pick = it;
            }
            if (!pick) { Log("OPTIC no item to pick"); s_step = 14; return; }
            Params press(pick, kItemPress);
            press.Call();
            Log("OPTIC pressed {}", pick.Name());
            s_step = 42; s_wait = 120;
            return;
        }
        case 42: {
            Params ga(s_gun, "GetAttachedActors");
            ga.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Call();
            if (auto* arr = (GML_TArray*)ga.Ptr("OutActors"))
                for (int i = 0; i < arr->Num; i++) {
                    Object a(((GUObject**)arr->Data)[i]);
                    Vec l = ToActor(s_gun, ActorLocation(a));
                    Object ap = a.IsA("GunAttachment") ? a.GetObj("AttachablePoint") : Object(), par = a.GetObj("RootComponent").GetObj("AttachParent");
                    Log("OPTIC attached {} [{}] at ({:.2f},{:.2f},{:.2f}) tag '{}' point {} parent {} attached {}", a.Name(), ClassName(a), l.x, l.y, l.z,
                        a.IsA("GunAttachment") ? NameStr(a.Get<uint64_t>("AttachPointTag")) : std::string(), ap.Name(), par.Name(), a.GetBool("bAttached"));
                }
            CaptureGun("barrett_optic", 32);
            CaptureFrom("barrett_optic_close", {0, 80, 12}, {0, 0, 9});
            s_step = 14; s_wait = 10;
            return;
        }
        case 14: {  // fire through the game's own FireRoundLocal: each call should spawn one .50 BMG
            s_rounds = 0;
            Params fr(s_gun, "FireRoundLocal");
            fr.Call();
            Log("FIRE called FireRoundLocal once: {} round(s) began play", s_rounds);
            LogRoundDefaults("after firing");
            s_step = 15; s_wait = 30;
            return;
        }
        case 15:
            SetHandle(0.5f);
            s_step = 16; s_wait = 3;
            return;
        case 16:
            LogParts("handle half back");
            SetHandle(1.0f);
            s_step = 17; s_wait = 3;
            return;
        case 17:
            LogParts("handle all the way back");
            CaptureFrom("barrett_boltback", {12, 70, 5}, {12, 0, 0});
            SetHandle(0.0f);
            s_gun.Set("CurrentTrigger", 1.0f);
            s_gun.SetBool("bIsMagReleasePressed", true);
            s_step = 18; s_wait = 30;
            return;
        case 18:
            LogParts("home, trigger pulled, mag release pressed");
            CaptureFrom("barrett_trigger", {0, 45, -2}, {0, 0, -3});
            s_gun.Set("CurrentTrigger", 0.0f);
            s_gun.SetBool("bIsMagReleasePressed", false);
            s_step = 20; s_wait = 10;
            return;
        case 20: {  // save it as the game saves a loadout gun, then rebuild the loadout as an operation does
            for (auto& g : FindAllOf("BP_GearManagerComponent_C"))
                if (API->GetOuter(g.ptr) == Pawn().ptr) s_gear = g;
            if (!s_gear) { Error("no gear manager"); s_step = 30; return; }
            Params sv(s_gear, "SaveGunSetup");
            sv.SetObj("GunReceiver", s_gun).Call();
            Log("LOADOUT saved {} (slot SavedGun_{}_{})", s_gun.Name(), FStr8(s_gear, "GunSetupSlotName"), FStr8(s_gear, "PrimaryGunSetupSlotName"));
            s_step = 21; s_wait = 180;  // the plugin re-saves the slot with a stable reference within a second
            return;
        }
        case 21: {
            s_holding = false;
            Params ld(s_gear, "LoadGunSetups");
            ld.Call();
            Log("LOADOUT LoadGunSetups called");
            s_step = 22; s_wait = 300;
            return;
        }
        case 22: {
            Object pg = s_gear.GetObj("PrimaryGun");
            Log("LOADOUT PrimaryGun after load: {} class {} - Barrett meshes {}, magazine capacity {}", pg.Name(), ClassName(pg),
                Components(pg, "DynamicMeshComponent").size(), pg.GetObj("CurrentMagazine").Get<int32_t>("AmmoCapacity"));
            s_step = 50;
            return;
        }
        // ---- the player's left hand takes a spare Barrett magazine from a pouch, as in VR; then the right
        case 50: {
            Object pawn = Pawn();
            Params gv(pawn, "GetVRComponents");
            gv.Call();
            s_gripCtrl = *(GUObject**)gv.Ptr("LeftController");
            s_gripCam = *(GUObject**)gv.Ptr("Camera");
            s_gripSide = "_l";
            for (auto& m : FindAllOf("Magazine_SR25_Steel_20_C")) {
                if (Components(m, "DynamicMeshComponent").empty()) continue;  // not a Barrett magazine
                Params pa(m, "GetAttachParentActor");
                pa.Call();
                Object parent = pa.Return<GUObject*>();
                if (!parent || parent.IsA("GunReceiver")) continue;
                Object root = parent;
                for (int g = 0; g < 8; g++) {
                    Params up(root, "GetAttachParentActor");
                    up.Call();
                    Object u = up.Return<GUObject*>();
                    if (!u) break;
                    root = u;
                }
                if (root.ptr != pawn.ptr) continue;
                s_gripMag = m;
                Log("GRIP spare magazine {} in {} on {}", m.Name(), parent.Name(), root.Name());
                break;
            }
            if (!s_gripMag && s_spawnedMags > 0 && s_spawnedMags < 6) {  // not dressed yet: wait a little longer
                Object pg = s_gear ? s_gear.GetObj("PrimaryGun") : Object();
                Params pa(pg, "GetAttachParentActor");
                pa.Call();
                Log("GRIP waiting for the plugin to dress the spare ({}): primary {} attached to {}", s_spawnedMags++, pg.Name(), Object(pa.Return<GUObject*>()).Name());
                s_wait = 120;
                return;
            }
            if (!s_gripMag && s_spawnedMags++ == 0) {
                // the room's pouches hold the kit's own magazines: one carried on the player is dressed by the plugin
                // while the player has a Barrett, as a spare would be
                Params mk(Lib("KismetMathLibrary"), "MakeTransform");
                mk.Set("Location", ActorLocation(pawn)).Set("Scale", Vec{1, 1, 1}).Call();
                Params b(Lib("GameplayStatics"), "BeginDeferredActorSpawnFromClass");
                b.SetObj("WorldContextObject", Object(API->WorldContext())).Set("ActorClass", FindClass("Magazine_SR25_Steel_20_C"))
                    .Set("CollisionHandlingOverride", (uint8_t)1);
                std::memcpy(b.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), sizeof(XfD));
                b.Call();
                Object mag = b.Return<GUObject*>();
                Params f(Lib("GameplayStatics"), "FinishSpawningActor");
                f.SetObj("Actor", mag);
                std::memcpy(f.Ptr("SpawnTransform"), mk.Ptr("ReturnValue"), sizeof(XfD));
                f.Call();
                if (Object root = mag.GetObj("RootComponent")) {
                    Params sp(root, "SetSimulatePhysics");
                    sp.Set("bSimulate", false).Call();
                }
                Params at(mag, "K2_AttachToActor");
                at.SetObj("ParentActor", pawn).Set("SocketName", uint64_t(0)).Set("LocationRule", (uint8_t)1).Set("RotationRule", (uint8_t)1)
                    .Set("ScaleRule", (uint8_t)1).Set("bWeldSimulatedBodies", false).Call();
                Log("GRIP spawned {} on the player, for the plugin to dress", mag.Name());
                if (Object pg = s_gear ? s_gear.GetObj("PrimaryGun") : Object()) {  // carried, as a slung gun is
                    Params ag(pg, "K2_AttachToActor");
                    ag.SetObj("ParentActor", pawn).Set("SocketName", uint64_t(0)).Set("LocationRule", (uint8_t)1).Set("RotationRule", (uint8_t)1)
                        .Set("ScaleRule", (uint8_t)1).Set("bWeldSimulatedBodies", false).Call();
                }
                s_wait = 60;
                return;
            }
            if (!s_gripCtrl || !s_gripMag) { Error("GRIP no left controller ({}) or no spare Barrett magazine on the player", s_gripCtrl.Name()); s_step = 30; return; }
            s_gripAt = ActorLocation(s_gripMag);  // the hand rests on it a moment, as the game's grip highlight wants
            s_gripHold = true;
            s_curls.clear();
            for (size_t a = 0; a < s_curlsCfg.size();) {
                size_t b = s_curlsCfg.find(',', a);
                s_curls.push_back(std::stod(s_curlsCfg.substr(a, b - a)));
                a = b == std::string::npos ? s_curlsCfg.size() : b + 1;
            }
            if (s_curls.empty()) s_curls = {1.0};
            s_curlIdx = 0;
            s_step = 51; s_wait = 90;
            return;
        }
        case 51:
            ScaleDeltas(s_gripMag.GetObj("HandSocket"), s_curls[0] / s_curNow);
            s_curNow = s_curls[0];
            Log("GRIP slot grip ({} hand) on the magazine's hand socket: {}", s_gripSide == "_l" ? "left" : "right", SlotGrip(s_gripCtrl, s_gripMag));
            s_gripHold = false;
            s_step = 52; s_wait = 10;
            return;
        case 52:
            SetHoldPoint();
            s_gripHold = true;
            s_step = 53; s_wait = 40;
            return;
        case 53:
            MeasureGrip();
            if (s_curlIdx == 0) GripViews(std::format("{}_c{:.0f}", s_gripSide.substr(1), s_curNow * 100));
            if (++s_curlIdx < s_curls.size()) { DropGrip(); s_step = 60; s_wait = 15; return; }  // let go, wait, take it again
            if (s_gripSide == "_l") {  // the same magazine in the right hand
                DropGrip();
                Params gv(Pawn(), "GetVRComponents");
                gv.Call();
                s_gripCtrl = *(GUObject**)gv.Ptr("RightController");
                s_gripSide = "_r";
                s_curlIdx = 0;
                s_step = 60; s_wait = 15;
                return;
            }
            s_step = 30;
            return;
        case 60:  // the next curl (or hand), taken again after a proper release
            ScaleDeltas(s_gripMag.GetObj("HandSocket"), s_curls[s_curlIdx] / s_curNow);
            s_curNow = s_curls[s_curlIdx];
            if (!SlotGrip(s_gripCtrl, s_gripMag)) { Error("GRIP could not take it at curl {:.2f}", s_curNow); s_step = 30; return; }
            s_step = 52; s_wait = 10;
            return;
        case 30:
            s_gripHold = false;
            Log("DEV TEST COMPLETE");
            s_step = 99;
            return;
    }
}

GML_AWAKE() {
    s_shotDir = Config.Bind<std::string>("Test", "ShotDir", "", "Where the captures go.").Value();
    s_gripOnly = Config.Bind("Test", "GripOnly", false, "Only the Barrett, its loadout and the hands on a spare magazine.").Value();
    s_curlsCfg = Config.Bind<std::string>("Test", "GripCurls", "1", "Finger curls to measure the hands at (comma separated; 1 = as the plugin makes the grip).").Value();
    On(GML_EVENT_WORLD_BEGIN_PLAY, [](void* gm) {
        std::string level = Object((GUObject*)gm).Path();
        if (level.find("/Game/Maps/MainMenu/") == 0) {
            Params p(Lib("GameplayStatics"), "OpenLevel");
            p.SetObj("WorldContextObject", Object(API->WorldContext())).Set("LevelName", API->MakeName("/Game/Maps/TeamRoom/Teamroom")).Set("bAbsolute", true).Call();
        } else if (level.find("Teamroom") != std::string::npos) {
            s_step = 0;
            s_wait = 600;  // let the room and the player's kit load
        }
    });
    // Every round that spawns: its class and the values it was spawned with.
    HookAfter("BP_GunProjectile_C:ReceiveBeginPlay", [](Object b, GUFunction*, void*) {
        s_rounds++;
        Vec v = b.Get<Vec>("Velocity");
        Log("ROUND {} [{}] Mass {:.4f} kg Diameter {:.3f} cm speed {:.0f} m/s penetration {:.2f}-{:.2f} DamageCurve {} pooling {}", b.Name(), ClassName(b),
            b.Get<float>("Mass"), b.Get<float>("Diameter"), std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) / 100, b.Get<float>("MinPenetration"),
            b.Get<float>("MaxPenetration"), b.GetObj("DamageCurve").Name(), b.GetBool("EnablePooling"));
    });
    On(GML_EVENT_TICK, [](void*) {
        if (s_step >= 0 && s_step < 99) Step();
        Hold();
        HoldGrip();
    });
    HookAfter("CustomizationStation_Gun_C:ReceiveTick", [](Object, GUFunction*, void*) { Hold(); });
    return 0;
}
