# Barrett M107A1 for Geronimo (personal use)

A GML plugin that adds a **Barrett M107A1** to the gun station's **Snipers** list: a semi-automatic .50 BMG rifle with
its 10-round magazine. It is built at runtime on the game's KAC M110 (semi-auto, AR-style controls, the same A2
pistol grip), without replacing any game asset: every other M110 stays an M110.

**Personal use only.** The model and textures are the Barrett M107A1 from the WTT-Armory mod for SPT (Single
Player Tarkov), exported by the user from their own install with TarkinItemExporter. They ship only inside
`Barrett.dll`, encrypted (`tools\pack`, a new key each build), and are never committed or shared.

## What it does

- **Spawner entry:** after `BP_GameInstance::GetItems` builds the gun lists, a runtime `GunAsset` (the M110's
  receiver, an icon rendered from the Barrett model) goes into the M110's own category (2, Snipers).
- **Looks:** an M110 spawned from that entry, or rebuilt from a saved loadout slot holding it, gets the Barrett's
  meshes (`DynamicMeshComponent`s from the embedded OBJs, `M_DefaultShader` instances); the M110's mesh and
  moving parts are hidden. The model is fitted on the M110 by the A2 pistol grip (trimmed ICP, 3.2 mm RMS).
- **Its own action:** the Barrett's bolt carrier with its side charging handle slides with the M110's carrier bone
  (`Root_BoltCarrierGroup`, 11.35 cm at full travel); its trigger turns 10° on its pin with the trigger pull; its
  magazine release lever turns 12° while pressed.
- **The M110's points, moved onto the Barrett:**
  - the charging handle grab box (and its stored start copy) onto the knob on the right side of the receiver;
  - the fire location onto the start of the muzzle brake (bullets, and a muzzle device in place of the brake);
  - the chamber (and its round, load and eject points) onto the Barrett's chamber, 11 cm further forward;
  - the top rails up 2.25 cm onto the Barrett's rail; the side and bottom rails onto the front of its receiver;
  - the support hand round the front of the receiver; the stock-butt anchor behind the Barrett's butt pad.
- **Iron sights:** the flip-up front and rear sights are hidden while an optic is on the top rail, where the raised
  rear sight would stand in its view.
- **Magazines:** magazines in a Barrett, and the spares you carry while you have one, become the Barrett's 10-round
  magazine (its mesh; the SR-25 magazine's rounds hidden).
- **Magazine grip:** the hand you take a magazine by is the SR-25 magazine's: a fist round a magazine half as deep,
  whose fingers sank 2 to 2.5 cm into the Barrett's. It is turned about 26° and moved 6.7 cm forward, fingers at 80%
  of their curl, so the hand closes round the magazine's front edge: palm, thumb and first two fingers 3 to 5 mm off
  it, both hands (the game mirrors the socket for the right). Fitted on the player's own body hand (see Testing).
- **.50 BMG:** the game has no .50. `AGunReceiver::FireRoundLocal` (native, hooked by byte signature) spawns the
  round from its class defaults; while a Barrett's FireRoundLocal runs, the M110 round's defaults are a .50 BMG
  ball: 42 g, 12.95 mm, 853 m/s, its own damage curve (600 at the muzzle), 2.5 times the 7.62 AP's penetration,
  never pooled. They go back right after the shot, so every other 7.62 gun keeps its 7.62.
- **Recoil:** the M110's, times 2.5.
- **Living with the Saiga-12K:** both plugins hook FireRoundLocal. Whichever loads second finds GML's jump in
  place of the function's first five bytes, follows it to the first plugin's detour and hooks that, so both run in
  turn, each passing on guns that aren't its own.
- **Loadouts:** a saved Barrett is re-saved with the M110 as its GunAsset and the receiver colour name
  `BarrettM107` (also what names it a Barrett for other players), and its file hash is kept in the config.
- **Multiplayer:** the colour name replicates, so every machine with the plugin dresses the gun, whoever picked it.
  The host fires every round in the game (a client's shot reaches it as `FireRoundServer`) and the .50 hook runs
  there for anyone's Barrett, so the round that hits, and its damage, is a .50. Every other machine gets a copy of
  that round with only its speed, which would fly and strike as the M110's 7.62: the plugin gives a round of the
  M110's class fired by a player holding a Barrett the .50's mass, size, penetration and damage curve as it
  arrives. Verified in a two-player test (`Utils\MPDev`, 2026-10-13): the Barrett dressed on both machines, the
  host's round for the client's shot a .50 (42 g, 12.9 mm, 853 m/s, the .50 curve), and the copies too.

## Build and install

There is no download: the model comes from your own SPT export with the WTT-Armory mod and can't be shared.
Build it from yours. `prep` also needs [retoc](https://github.com/trumank/retoc) and `UAssetParser` from the
[modding toolkit](https://github.com/RemArrow/geronimo-modding-toolkit) in a `Utils` folder next to this one (see `build.bat`).

```bat
rem 1. Assets\ from the export (BARRETT_EXPORT; default E:\TARKOVE EXPORTS\BarrettM107\BarrettM107 Files)
build.bat prep
rem 2. the plugin (close the game first)
build.bat install
```

`prep` extracts the M110's mesh (`SR25_M110SASS`) with retoc and reads its points with `Utils\UAssetParser
uexppos`, then converts the glTF with `tools\prep`: OBJs in the game's space, `<Part>_{Diffuse,Normal,ORM}.png`
(the export's 4096 maps scaled to 2048 or less), `Icon.png`, and preview renders in `build\preview`.

`install` copies `Barrett.dll` and `CREDITS.txt` to `...\Binaries\Win64\GML\plugins\Barrett\`. Needs GML 2.2+.
With the Saiga-12K installed too, use its build from 2026-10-10 or later (the shared FireRoundLocal hook).

## Settings: `GML\config\com.remarrow.barrett.cfg`

| Key | Default | |
|---|---|---|
| `[Gun] Capacity` | 10 | rounds per magazine |
| `[Gun] RecoilScale` | 2.5 | recoil per shot, as a share of the M110's |
| `[Round] Enabled` | true | false = the M110's 7.62 |
| `[Round] Damage` | 600 | at the muzzle (the game's 7.62x39 does 95) |
| `[Round] Velocity` | 853 | m/s |
| `[Round] Mass` | 42 | grams |
| `[Round] Penetration` | 2.5 | a multiple of the M110 round's |
| `[Visuals] OffsetX/Y/Z` | 0 | cm, a nudge on top of the fitted position |
| `[Visuals] FitAttachments`, `OwnMovingParts`, `OwnMagazines` | true | |

## Testing

`tests\BarrettDev` (built, never installed) loads the team room and, at the gun station:
1. spawns a stock M110 through the spawner's own menu event and logs its layout: components, attach points,
   movables bones, magazine, charging handle settings, round;
2. spawns the Barrett, logs it and renders it (right, left, top, the action);
3. fires it through `FireRoundLocal` and logs every round that spawns, and the round's defaults after;
4. pulls the charging handle half and all the way, pulls the trigger and presses the mag release;
5. saves it as a loadout gun and rebuilds the loadout;
6. takes a spare Barrett magazine with the player's left hand, then the right, through the game's slot grip, and
   measures the body's hand bones against the magazine (a box), at `[Test] GripCurls` (comma separated). The
   controller is held 25 cm forward, 30 down and 15 out from the head, where the arm reaches its IK target.

`[Test] GripOnly = true` runs only the Barrett, its loadout and the hands.

Verified in game (2026-10-10):
- **Spawner:** the Snipers list reads G28, M110 SASS, SR-25, R700 .338LM, MDT ESS .338LM, Barrett M107A1.
- **Looks:** the in-game renders show the Barrett's geometry and textures, the M110 hidden, the magazine seated.
- **.50 BMG:** one shot spawns one round: 0.042 kg, 1.295 cm, 853 m/s, penetration 26.9 to 28.1, the .50's damage
  curve, not pooled. The 7.62 AP's defaults are back afterwards (0.008 kg, 0.782 cm, 910 to 930 m/s, pooled).
- **Action:** the carrier and charging handle travel 5.7 / 11.35 cm with the handle at half / full; the grab box
  follows the knob (19.70 -> 14.02 -> 8.35).
- **Points:** fire location (92.01, -0.04, 3.80), chamber round (21.98, -0.04, 3.82), top rails at Z 7.95, the support
  hand at (32, 0, 4.34).
- **Loadouts:** a saved Barrett comes back as a Barrett; carried spare magazines become Barrett magazines.
- **Magazine grip:** the left hand 3 mm clear (thumb 4, index 5, middle 3, ring 8, palm 4 mm), the right 0 to 3
  (its arm 0.7 cm short of its target in the test); the M110's own grip was 25 mm in.
- **With the Saiga-12K:** its slugs chain after the Barrett's hook and still fire.

## Not done (yet)

- The M110's collision is shorter than the Barrett: its front 40 cm (barrel, brake) don't collide.
- The charging handle's hand pose is the M110's (an AR T-handle pulled from behind), on the Barrett's side knob.
- The chambered round and the ejected cases are the M110's 7.62 meshes.
- The optic test: hiding the iron sights under an optic is untested (the harness can't pick an optic yet).
- The XM109 suppressor in the export isn't used; the bipod is folded and static; the barrel doesn't recoil.
