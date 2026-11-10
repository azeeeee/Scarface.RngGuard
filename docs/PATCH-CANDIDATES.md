# Patch candidates — what could go into the ASI

The goal "fix everything GangstaPatch fixes" means reproducing ~110 byte edits and
inline hooks from a closed-source patch, many of which depend on handler code in
their DLL. Blind-porting them is how you ship crashes, so this file records what
exists, what it touches, and what is safe to do next.

**Policy for this repository:** a patch ships only if (a) the original bytes are
verified on the target build, (b) the semantics are understood well enough to
explain the change, and (c) it is behind an ini toggle and testable in isolation.
Everything else stays on this list.

---

## Already covered (shipped)

| Issue | Where | Status |
|---|---|---|
| `RandomRange` divide-by-zero (`0xC0000094`) | RVA `0x1330` | replaced function, verified by fuzzing |
| Ambience picker NULL result | `0x205A10`, `0x205A6B` | caller guards |
| Ambience picker corrupt list | `0x204640` | entry validator |
| Third picker caller NULL | `0x1FB1B8` | caller guard |
| Audio scale divide-by-zero | `0x2F14B9` | optional guard (`GuardAudioDivZero`) |
| 3D listener pitch scaling | `0x2EF21C`, `0x2EF226` | optional (`Fix3DAudioPitch`) |

Everything below is **not** shipped.

---

## Candidate map (extracted from `GangstaPatch.asi`)

`in function` = nearest preceding `int3` boundary; `class` = MSVC RTTI of the
function if it appears in a vtable.

| RVA | in function | class | instruction at target |
|---|---|---|---|
| 0x001167 | 0x001010 |  | `call 0x5e1bb0` |
| 0x00D723 | 0x00D670 |  | `cmp al, 0xfa` |
| 0x02DE8E | 0x02DD50 | `ControllerFlowClient@ControllerManager` | `js 0x42defd` |
| 0x02DE96 | 0x02DD50 | `ControllerFlowClient@ControllerManager` | `mov eax,[0x806d98]` |
| 0x03274F | - |  | data |
| 0x0371BD | 0x037120 |  | `call 0x437620` |
| 0x0372CC | 0x037120 |  | `mov ax,[0x7355a8]` |
| 0x03774C | 0x037580 |  | `mov ecx,[esp+0x10]` |
| 0x056FD0 | 0x056FD0 |  | `sub esp,0x24` |
| 0x057090 | 0x057090 |  | `mov ecx,[0x7bf72c]` |
| 0x05E270 | 0x05E240 |  | `je 0x45e323` |
| 0x05E6E9 | 0x05E680 | `Display_List@renderer` | `add edi,edi` |
| 0x0B1B38 | 0x0B1AC0 |  | `jne 0x4b1b4b` |
| 0x0BD08A | 0x0BCFB0 |  | `mov ecx,[edi+4]` |
| 0x0D50E0 | 0x0D4BE0 |  | `test ah,5` |
| 0x1636F6 | 0x1636F0 |  | `cmp byte [edi+0x2a2],0` |
| 0x163F75 | 0x163D30 |  | `call 0x6605d0` |
| 0x165104 | 0x164FA0 |  | `jne 0x5651d0` |
| 0x165768 | 0x165640 | `GenericCharacterCamera` | `xor bl,bl` |
| 0x1660B5 | 0x165FD0 |  | `add ecx,ebx` |
| 0x167A88 | - |  | data |
| 0x17AB9F | 0x17AB60 |  | `call 0x57e310` |
| 0x18B110 | 0x18AFF0 |  | `mov eax,2` |
| 0x1ABA2B | 0x1AB890 |  | `test eax,eax` |
| 0x1CD320 | 0x1CD320 |  | `cmp byte [esp+4],0` |
| 0x1E4299 | 0x1E4270 | `PauseMenuPageOptions` | `call 0x491a70` |
| 0x1E42E9 | 0x1E42C0 | `PauseMenuPageOptions` | `call 0x491a70` |
| 0x1E4540 | 0x1E44C0 | `PauseMenuPageOptions` | `add esp,0x2c` |
| 0x1E48FD | 0x1E48D0 |  | `add esp,0x1c` |
| 0x2071F3 | 0x207100 | `MortarObject` | `fsub [eax+0x188]` |
| 0x24B971 | 0x24B960 | `d3dContext@pure3d` | shader/render path |
| 0x24FB64 | 0x24FA20 |  | `mov edx,[edi+4]` |
| 0x250364 | 0x24FF10 | `d3dContext@pure3d` | `mov eax,[eax+8]` |
| 0x251330 | 0x250EA0 |  | `sub esp,0x78` |
| 0x25163E | - |  | data |
| 0x2523E0/1 | - |  | data |
| 0x254058 | 0x253FF0 | `d3dDisplay@pure3d` | `cmp ebx,0x3c` |
| 0x254071 | 0x253FF0 | `d3dDisplay@pure3d` | `mov eax,ebx` |
| 0x25490B | 0x2545D0 | `d3dDisplay@pure3d` | `mov [ebp+0x1c],0x15` |
| 0x254965 | 0x2545D0 | `d3dDisplay@pure3d` | `push 0x44` |
| 0x25496B | 0x2545D0 | `d3dDisplay@pure3d` | `call [edx+0x40]` |
| 0x257346 | 0x257270 |  | `inc eax` |
| 0x2A0B85 | 0x2A0940 |  | `mov eax,[esi+0x208]` |
| 0x2A84E0/2A84F0/2A8510 | 0x2A84E0 |  | time/scale helpers |
| 0x2B7C30/70/B0, 0x2B7ED0, 0x2B89C0, 0x2BD2A0 |  |  |  |
| 0x2DC190, 0x2DD670, 0x2DD7B0, 0x2DD8AD, 0x2DDB15/1A, 0x2DEAAE |  |  | audio/stream |
| 0x2DEFB0 | 0x2DEFB0 |  | `push esi` |
| 0x2DF734 | 0x2DF730 | `MemoryAllocatorMalloc@core` | `test eax,eax` |
| 0x2EF21C/226 | 0x2EF190 |  | 3D listener floats |
| 0x302BAC | 0x302A00 |  | `call 0x401010` |
| 0x303A20 | 0x303980 |  | `push 0x2000` |
| 0x30791B | 0x3078E0 |  | `sub al,al` |
| 0x3079EF | 0x307980 | `d3dCharacterShader@pure3d` | `call 0x65bc40` |
| 0x308899 | 0x3087B0 |  | `xor bl,al` |
| 0x33356C, 0x358D70, 0x367708, 0x3D6EEC, 0x3FC5F8 | - |  | data / vtable slots |

---

## Grouped by effort/risk

### A. Safe to port next (small, self-contained)

- **Audio buffer flags** (`0x303A20`, `0x254965`) — two immediates in the
  `d3dDisplay`/audio setup. Need a runtime A/B because their exact meaning is
  not documented; keep behind a separate key.
- **`0x2EF21C`/`0x2EF226`** — already shipped.

### B. Needs its own analysis session

- **`MortarObject` `0x2071F3`** — "mortar up-force wrong above 30 FPS". Requires
  reconstructing the intended delta-time math, not a byte swap.
- **`d3dDisplay` `0x254058`/`0x254071`/`0x25490B`** — refresh-rate and
  resolution handling. Interacts with the Direct3D wrapper and FusionFix.
- **`d3dContext` `0x24B971`/`0x250364`** — render-state / shader path,
  overlaps SilentPatch and GangstaPatch's reverse-Z work.

### C. Out of scope for an ASI patch

- Reverse-Z, glass shader, dynamic blood, widescreen HUD, post-process FX,
  particle emission — these are renderer features, not crash fixes, and they
  need their own multi-file work.
- Controller remapping, captions persistence, tape/caption saving — UI and
  save-format work.

---

## How to add one safely

1. Pick exactly one target from group A or B.
2. Disassemble the containing function in `scarface.exe`, identify what the
   bytes control, and write down the intended behaviour.
3. Add an ini key under `[Fixes]`, verify the original bytes before patching,
   and log both the install result and any mismatch with the bytes found.
4. Test with the probe/log approach used for the ambience crash: confirm it
   fires, confirm the game still boots, then remove the probe.
5. Update this file and `docs/technical.md`.

No patch ships without step 3's byte verification and step 4's boot test.
