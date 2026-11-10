# AGENTS.md — Scarface reverse-engineering notes

Notes collected while building `Scarface.RngGuard.asi`. Written for a future
agent (or human) who needs to find, verify and patch code in
`scarface.exe` without repeating the same dead ends.

---

## 1. Target identity

| | |
|---|---|
| File | `scarface.exe` |
| Version | 1.0.0.2 (patch) |
| TimeDateStamp | `0x455CAD51` |
| ImageBase | `0x00400000` |
| SizeOfImage | `0x005FC000` |
| File size | 6,434,817 bytes |
| SHA-256 (this copy) | `aee792ee4552e28c533abab7992dd5ad542e8ff12806cbcedab300842d86f6f5` |

`Scarface.old` in the game folder is the older 1.0.0.0 executable (8.4 MB,
SecuROM sections `age`/`agis`/`quod`/`.securom`). Do not mix offsets between
the two.

### PE quirks (important)

The 1.0.0.2 executable is a rebuilt/unprotected image with a scrambled section
table:

```
.rdata  VA 0x001000  (3.3 MB, actually contains the engine code)
.data   VA 0x32F000
.bss    VA 0x3B7000
.rsrc   VA 0x45E000
.text   VA 0x460000
.idata  VA 0x5CE000
.rsrc   VA 0x5D2000
.rdata  VA 0x5D5000
.data   VA 0x5ED000
.bss    VA 0x5EE000
.idata  VA 0x5F8000
.mcz    VA 0x5F9000
```

- Section names repeat and do not match their contents.
- Characteristics are odd (`CNT_CODE|MEM_READ|MEM_WRITE`, no `MEM_EXECUTE`),
  yet the code runs (DEP is not enforced for this image).
- **Because Ghidra honours those flags, it marks the first section
  non-executable and only auto-analyses `.text` (0x460000+).** Queries for
  addresses in the 0x401000–0x72EADC range return `outside_segments`.

**Consequence:** use Ghidra/REA for the DLLs and for `.text`, but analyse the
first 3.3 MB (which is most of the game) with a linear disassembler (capstone)
or by poking Ghidra into creating functions manually.

### Address mapping

Every section maps 1:1 (RawOffset == VirtualAddress), so:

```
file_offset = RVA
VA          = RVA + 0x400000
RVA         = VA - 0x400000
```

No rebasing, no relocations to worry about. This is true for all 12 sections.

---

## 2. Tooling

- **Python 3 + `capstone`** (`CS_ARCH_X86`, `CS_MODE_32`) is the fastest way to
  inspect this image; `struct` is enough to parse the PE and imports.
- **REA/Ghidra** is excellent for decompiling small helpers and for the
  accessory DLLs (`GangstaPatch.asi`, loaders) but see the section-map caveat
  above.
- **MSVC x86** (`vcvars32.bat`) for the ASI itself. Output is a 32-bit DLL.
- A tiny 32-bit host executable that `LoadLibrary`s the ASI is a cheap smoke
  test: the plugin should log `NOT INSTALLED: main module ...` and exit cleanly.

### Always disassemble from a real boundary

Misaligned decoding caused two wrong conclusions during this project. Before
trusting a listing, find the function start by scanning back for `CC CC CC CC`
padding, or start from a known entry. Capstone will happily decode data.

---

## 3. Reading an Event Viewer crash

The WER/Event Viewer `Fault offset` is an **RVA**:

| Report | RVA | Instruction |
|---|---|---|
| `0xc0000094` | `0x1387` | `div esi` inside `RandomRange` |
| `0xc0000005` | `0x204671` | `mov ecx,[edi]` in the ambience picker |
| `0xc0000005` | `0x205A16` | `mov edx,[eax+4]` NULL candidate |
| `0xc0000005` | `0x1B0067` | `mov eax,[ecx]` in `MissionHUD` (NULL layer) |

The registers in the WER line are often enough to confirm the exact
instruction. Example: `ECX=0xD8` matched `24 * 9` in the audio scale helper,
which proved the table index was 9.

Recommended flow:

1. Take the fault offset as RVA.
2. Disassemble from the containing function's start.
3. Match the instruction to the exception code (`div` → 0xC0000094, memory
   access → 0xC0000005).
4. Use the register values to identify which pointer/size was bad.
5. Only then decide whether a patch is possible; verify with a runtime logger
   before touching bytes.

---

## 4. MSVC RTTI

The image keeps full RTTI, which is the single most useful navigation aid.

### Function → class

1. Find dwords equal to the function VA (vtable slots).
2. Walk backwards through the contiguous pointer array to its first entry.
3. `vtable[-1]` is the CompleteObjectLocator (COL).
4. `COL+12` points to the TypeDescriptor; the class name is at `TD+8`
   (NUL-terminated, starts with `.?AV`).

### Class name → vtable

1. Find the `.?AV...@@` string; `TD = string_va - 8`.
2. Find dwords equal to `TD` (that is the COL's `pTypeDescriptor` field);
   `COL = match - 12`.
3. Find dwords equal to `COL`; `vtable = match + 4`.

### Tables discovered

| Class | vtable |
|---|---|
| `WorldAmbienceSpawnObject` | `0x75D1F4` |
| `MissionHUD` | `0x74FBBC` |
| `SeqActionStream@audio` | `0x773768` |
| `SeqActionVarPitch@audio` | `0x7736D8` |
| `SpawnObject` | `0x75CE24` |
| `GenericSpawnObject` | `0x75C1C4` |
| `PersistentVehicleSpawnObject` | `0x75C47C` |
| `VehicleSpawnObject` | `0x75D104` |
| `VehicleObject` | `0x76454C` |

Class hierarchy names (`COL+16` → base class array) give useful context, e.g.
`WorldAmbienceSpawnObject -> SpawnObject -> GameObject -> GameSetObject ->
ScriptObject -> ConsoleObject`.

---

## 5. Confirmed code map (RVAs)

| RVA | What |
|---|---|
| `0x1330` | `RandomRange(lo, hi)` = `lo + rand32() % (hi-lo+1)`; state pointer at `0x831660`; `div` at `0x1387` |
| `0x204640` | ambience candidate picker; reads vector at `+0x20/+0x24`, RNG call at `0x20471C` |
| `0x205A10`, `0x205A6B` | picker call sites that store/deref the result |
| `0x1FB1B8` | third picker caller (`SeqActionStream`-adjacent spawner code) |
| `0x2F1490` | audio time/scale helper; `div esi` at `0x2F14BB` (`esi = j * table[24*i+4]`) |
| `0x6EF190`* | audio 3D listener init; struct at `0x85D0F0` (position/velocity/front/up + 3 floats at `0x85D120/124/128`); pointer stored at `0x85D0E8` |
| `0x444540` | `FindObject(name, mode)` — **two arguments**, cdecl |
| `0x438F20` | thin wrapper `FindObject(name, 0)` |
| `0x1B0000` | `MissionHUD` init (looks up `MS_BlackLayer`, `MS_TextLayer`, `MS_StartLayer`, `DL_MS_ObjectiveText`) |
| `0x1B0067` | `MissionHUD` crash site when a layer lookup returned NULL |

`*` that row is a VA; subtract `0x400000`.

### `FindObject` details (learned the hard way)

```
0x438F20:  mov  eax,[esp+4]     ; name
           push 0               ; arg2
           push eax             ; arg1
           call 0x444540
           add  esp,8
```

- Signature: `void* __cdecl FindObject(const char* name, int mode)`.
- Prologue: `sub esp,0x104; push ebp; push ebx; push esi; push edi`, then
  `mov ebp,[esp+0x10C]` (arg1). `[esp+0x11C]` / `[esp+0x128]` after all pushes
  are arg2.
- It ends with plain `ret` (cdecl), so callers clean their own arguments.
- Callers push 2–7 arguments; the function only reads the first two. Do **not**
  pass fewer than two: arg2 drives the class/namespace filter, and corrupting
  it makes HUD layer lookups return NULL.

---

## 6. Hooking rules for this title

- **5-byte `E9 rel32` hook** at the function entry. First verify the original
  bytes; log the actual bytes on mismatch (a silent mismatch cost a build).
- **Trampoline**: copy whole instructions (e.g. `sub esp,0x104; push ebp` = 7
  bytes), append `E9` back to the first untouched instruction
  (`0x444547` for `FindObject`).
- **rel32 reach**: stub must be within ±2 GB of both the patch site and the
  resume address. Use a static buffer in the DLL if it fits, else
  `VirtualAlloc` near 0x20000000/0x30000000. Check both directions explicitly.
- **Signature matters**: a C hook that calls the trampoline must use the exact
  argument list. Determine it from the game's own callers (count pushes) and by
  mapping every `[esp+X]` read back to an argument after accounting for *all*
  prologue pushes and temporary pushes.
- **IAT is not resolved when an ASI loads.** The loader is still resolving the
  exe's imports while it initialises `vorbisfile.dll` and the ASIs. Reading an
  IAT slot in `DllMain` returns the on-disk value (a bogus in-image dword).
  Hook IAT entries lazily from a call that only happens once the game runs.
- **Threads**: never call engine functions from an arbitrary thread. Pin the
  work to the thread that first calls into the engine (usually the main one).
- **VEH logger**: an `AddVectoredExceptionHandler(1, ...)` that only logs and
  returns `EXCEPTION_CONTINUE_SEARCH` is cheap and does not change behaviour.
  Log RVA, access type, faulting address, registers and a `module+RVA` stack.
  Use raw `CreateFileA`/`WriteFile` (no CRT locks) so it is safe inside a
  handler.

---

## 7. Mission scripting

- Scripts are compiled TorqueScript `.cso` files; decompiled C#-like sources
  are available alongside the game (`money\decompiled`, `extractedfiles`).
- Symbol names are mangled **per file** (`stx...`), so the same logical function
  has a different name in every script. Cross-file name matching is unreliable.
- `mission_manager.cso` holds the mission table: `Name`, `AutoStart`,
  `Deliverer`, `WayPoint`, `WayPointType`, `MissionScreenTitle`, `TonyPos`,
  `RespawnRule`, etc.
- `M_A1A` (Pedro's Pawn Shop) stage 1:

  ```c
  if (stxaomnhgdn("M_A1A_HotGoodsVan")) { ...waypoint + objective... }
  ```

  `stxaomnhgdn` is `FindObject`. There is **no `else`**, so if the van has not
  spawned yet the objective is skipped permanently.
- Measured van spawn delay after stage 1 (from a `FindObject` hook): **0.7 s …
  14.6 s**, varying with streaming. The mission only starts when a leftover van
  from a previous attempt already exists.

---

## 8. GangstaPatch cross-reference (for audio work)

`GangstaPatch.asi` (`FUN_10006c68`) installs a bundle of edits; the
"3D Audio causing pitch changes" fix is the two listener floats:

| RVA | Original | Patched |
|---|---|---|
| `0x2EF21C` | `00 00 80 3F` (1.0f) | `00 00 00 00` |
| `0x2EF226` | `00 00 80 3F` (1.0f) | `00 00 00 00` |

The same function also NOPs a timer call (`0x6DF1F6`), flips `jnz`→`jmp`
(`0x4B1B38`), and changes two audio buffer flags (`0x703A20`, `0x654965`).
Those belong to other fixes — **do not copy a partial bundle**; take all or
none, or isolate carefully.

Their `RandomRange` fix patches the `div` itself with
`test esi,esi; mov edi,1; cmove esi,edi; div esi` (divisor 0 → 1, result = lo).
Our plugin instead replaces the whole function; both are equivalent for valid
ranges.

---

## 9. Lessons / pitfalls

1. **VA vs RVA** — writing `0x404640` instead of `0x604640` made a guard check
   the wrong address and fail silently. Always verify the prologue bytes and
   log the bytes you actually found.
2. **Function signatures** — a one-argument hook on a two-argument function
   corrupted every object lookup in the game and crashed `MissionHUD` at boot.
3. **Partial ports** — copying two edits out of a seven-edit bundle can change
   behaviour in unexpected ways.
4. **Frame-rate dependence** — this engine has several known high-FPS bugs
   (animations, physics, ocean). Timing-related bugs may reproduce differently
   at 30 vs uncapped fps.
5. **Streaming is the dominant variable** in "random" spawn failures; measure
   with a logger instead of guessing at constants.
6. **Everything is verifiable statically** — before patching, confirm the bytes
   in the file match the version you target. Refuse to patch anything else.

---

## 10. Repo quick reference

```
src/RngGuard.cpp        the ASI
dist/                   built ASI + player readme
docs/technical.md       crash analysis for the shipped guards
build.bat               MSVC x86 build
```
