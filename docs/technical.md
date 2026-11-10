# Technical notes

> **Note:** this fix, its analysis and all code in this repository were created
> entirely by an AI assistant.

Target: `scarface.exe` **1.0.0.2**, time stamp `0x455CAD51`, image base
`0x400000`, `SizeOfImage` `0x5FC000`.

Addresses below are **RVAs** (add `0x400000` for virtual addresses). The
on-disk image maps 1:1 to its run-time layout.

The plugin never writes to `scarface.exe`; every change is an in-memory patch
applied when the ASI loads.

---

## The failure chain

The crash reported on mission loading screens (`0xC0000094` or `0xC0000005`)
is one chain, not several unrelated bugs.

### 1. `RandomRange` divides by zero

`RVA 0x1330` is the engine helper `RandomRange(lo, hi)`:

```
1330  mov  ecx, [0x831660]      ; RNG state pointer
1336  mov  eax, [ecx]
...
1342  imul edi, edi, 0x9069
1350  mov  [ecx], edi           ; state[0] = ...
1367  add  ecx, eax
1369  mov  [edx], ecx           ; state[1] = ...
1371  shl  eax, 16
137A  add  eax, ecx             ; value
137C  mov  ecx, [esp+0xC]       ; lo
1380  sub  esi, ecx             ; hi - lo
1382  add  esi, 1               ; hi - lo + 1
1387  div  esi                  ; <-- 0xC0000094 if esi == 0
138B  mov  eax, edx             ; remainder
138D  add  eax, ecx             ; lo + value % (hi - lo + 1)
138F  ret
```

`hi - lo + 1 == 0` (an empty range) faults. The helper has **56 direct call
sites** in the exe.

### 2. The ambient spawner calls it with an empty range

The ambient population spawner picks a candidate at `RVA 0x204640`:

```
204663  mov  eax, [esi+0x24]    ; candidates.end
204666  mov  edi, [esi+0x20]    ; candidates.begin
204669  cmp  edi, eax
20466F  je   2046DD
204671  mov  ecx, [edi]         ; <-- 0xC0000005 if begin is bad
204673  mov  al, [ecx+0x16]     ; candidate type/flags
...
20471C  call 0x401330           ; RandomRange(0, total - excluded - 1)
```

When every candidate is excluded, `hi == -1`, the divisor is 0, and the game
dies in `RandomRange`. Live log evidence:

```
BAD RANGE  lo=0  hi=-1  caller=scarface.exe+0x204721
```

(`0x204721` is the return address of the call at `0x20471C`.)

### 3. The pick returns NULL and the callers dereference it

Once `RandomRange` is guarded, the picker returns `NULL` for that state, and
two call sites dereference it without checking:

```
205A10  mov [esi+0x94], eax
205A16  mov edx, [eax+4]        ; <-- 0xC0000005 read of 0x4
...
205A6B  mov [esi+0x94], eax
205A71  mov ecx, [eax+4]        ; <-- same
```

A third caller of the same picker has the same problem:

```
1FB1B8  call 0x604640
1FB1BD  test byte [eax+0x1F], bl ; <-- 0xC0000005 read of 0x1F
```

### 4. During a reload the candidate array itself can be stale

On the post-death mission reload the selector's array can be left inconsistent.
Observed on the loading screen:

```
CRASH ACCESS VIOLATION at scarface.exe+0x204671  read of 0x0
          eax=130C8874 (end)   edi=00000000 (begin)
```

and in another run:

```
INVALID PICKER LIST (bad size) begin=0x34A69046 end=0xB34E307B
```

`0xB34E307B` is not a valid user-mode address, so the whole structure had been
overwritten/recycled — consistent with the spawner being used while the world
is being torn down and rebuilt.

---

## Owning object

The destructor of the containing class loads vtable `0x75D1F4`, whose MSVC RTTI
names it:

```
.?AVWorldAmbienceSpawnObject@@
   -> .?AVSpawnObject@@ -> .?AVGameObject@@ -> .?AVGameSetObject@@
   -> .?AVScriptObject@@ -> .?AVConsoleObject@@
```

Field layout (as used by the code above):

| Offset | Meaning |
|---|---|
| `+0x94` | currently selected candidate |
| `+0x98` | candidate selector object |
| `+0x84` | value copied from the selected candidate |
| `+0xB0..B2` | spawn time window (units of 10) |
| `+0xB3`, `+0xB4` | state flags (`bit 0` = have selection) |
| selector `+0x20` / `+0x24` | candidate array `begin` / `end` (pointers) |
| selector `+0x30` | candidate limit / threshold |
| candidate `+0x16` | type / flags byte |

The select/advance step is `0x2059C0`; it is driven by `0x205B30`.

---

## Patches applied by the plugin

| RVA | What | Why |
|---|---|---|
| `0x1330` | `RandomRange` replaced by an identical reimplementation | zero-length range returns `lo` instead of dividing by zero |
| `0x204640` | entry jump to a validator | reject `begin == 0`, `begin > end`, misaligned/oversized/unreadable arrays; return `NULL` |
| `0x205A10`, `0x205A6B` | result handling at the two callers | on `NULL`, clear `+0x94` and `+0xB3 bit 0`, return from the update |
| `0x1FB1B8` | third caller | on `NULL`, skip the loop iteration (continue at `0x1FB259`) |

The `RandomRange` reimplementation was fuzz-checked against the original over
200,000 random state/argument pairs: output is bit-identical for every valid
range, and only the faulting case differs.

Every patch verifies the original bytes first. On a different exe build nothing
is patched and the log says which check failed (including the bytes found).

### Why not fix the root cause

The stale array is produced while the world is being rebuilt; nothing in the
plugin can know which allocation is valid at that moment. Treating the array as
"no candidates" is the only safe recovery, and it simply skips one ambient
spawn. The world repopulates on later ticks.

---

## Optional fixes (off by default)

### `Fix3DAudioPitch` — 3D audio pitch changes

`scarface.exe` initialises its 3D listener at RVA `0x2EF190`. After the
position / velocity / orientation vectors, three floats are set to `1.0`
(globals `0x85D120`, `0x85D124`, `0x85D128`). The mixer uses the first two as a
distance / rolloff scale that modulates the playback pitch of 3D sounds as they
move relative to the camera.

The option rewrites the two immediates:

| RVA | Original | Patched |
|---|---|---|
| `0x2EF21C` | `00 00 80 3F` (1.0f) | `00 00 00 00` (0.0f) |
| `0x2EF226` | `00 00 80 3F` (1.0f) | `00 00 00 00` (0.0f) |

This is the same change GangstaPatch makes for its "3D Audio causing pitch
changes" fix (located by reversing `GangstaPatch.asi`, `FUN_10006c68`).

### `GuardAudioDivZero` — audio sequence scale fault

RVA `0x2F14B9` computes `esi = j * table[24*i + 4]` and then divides by it:

```
2F14B9  33 D2        xor edx,edx
2F14BB  F7 F6        div esi          <- 0xC0000094 when esi == 0
2F14BD  8B 89 ...    mov ecx,[ecx+0x7F6BCC]
```

When the scale collapses to 0 this crashes inside the audio sequence actions
(observed in a `SeqActionStream@audio` call path). The option installs a
5-byte jump to a stub that forces `esi = 1` in that case and otherwise runs the
original instructions, so the game never divides by zero there.

### `MissionProbe` — experimental M_A1A diagnostic (`[Debug]`)

Off by default; needs `Logging=1` to produce output.

`M_A1A` stage 1 runs:

```c
if (getObject("M_A1A_HotGoodsVan")) { ...objective... }
```

`getObject` is the native `FindObject` at RVA `0x444540`. If the van hasn't
spawned yet the whole block is skipped, so the mission appears dead.

The probe:

* hooks `FindObject` at `0x444540` (5-byte jump + a trampoline that replays
  `sub esp,0x104; push ebp` and resumes at `0x444547`);
* logs every lookup whose name contains `M_A1A`, `HotGoods` or `FrontManager_A1`
  together with the returned pointer;
* when `M_A1A_HotGoodsVan` is first queried it starts a 15-second window and
  polls the same lookup every 250 ms (driven from the `PeekMessageA` IAT slot,
  `0x9CE708`, installed lazily on the first `FindObject` call, restricted to
  the game thread), logging only when presence changes.

A failing run then looks like:

```
PROBE lookup "M_A1A_HotGoodsVan" -> 00000000 (absent)
PROBE +0ms M_A1A_HotGoodsVan = absent (00000000)
PROBE +4321ms M_A1A_HotGoodsVan = PRESENT (0C5A1F80)
PROBE window finished (15 s)
```

If the second line is followed by `PRESENT` after a few seconds, the spawn was
simply late (streaming/scheduling). If it stays `absent` for the whole window,
the spawn object never produced the van.

---

## Build

```
build.bat
```

32-bit MSVC toolset (`vcvars32.bat`). Output: `dist\Scarface.RngGuard.asi`.

Build flags: `/LD /MT /O2 /GS- /MACHINE:X86`. The result imports only
`KERNEL32.dll`; `RtlCaptureStackBackTrace` is resolved from `ntdll.dll` at
run time.
