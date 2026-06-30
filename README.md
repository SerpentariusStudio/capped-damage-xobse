# CappedDamage (xOBSE)

An [xOBSE](https://github.com/llde/xOBSE) plugin for the **original 32-bit
Oblivion** (`Oblivion.exe 1.2.0.416`) that caps how much health any
**non-player actor** can lose in a short rolling time window. It prevents
one-shot kills: an overkill hit is clamped *before* it is applied, so a
full-health enemy always survives the first blow.

This is a port of an OBSE64 (64-bit Oblivion Remastered) "capped player-to-enemy
damage" plugin back to the original engine + xOBSE.

## How it works

For every health-damaging hit on an NPC or creature, the plugin allows at most
a configurable fraction of that actor's **maximum** health to be lost per
window. The window opens on the first health loss and does **not** refresh;
once it elapses, the next hit opens a fresh window.

- A full-health enemy can never be one-shot — an overkill hit leaves it at
  `(1 - fraction)` of max HP.
- The **player is exempt**, both structurally (the player's vtable slot is never
  hooked) and via a runtime `actor != player` guard.
- Defaults: cap = **1/3 of max health per 3 seconds**.

### Mechanism

The plugin installs a vtable-slot detour on the `Actor::DamageAV_F` float-damage
function at vtable index `0xA9`. In 32-bit Oblivion 1.2.0.416, `Character`
(NPCs) and `Creature` have distinct implementations of this slot, so both are
hooked; `PlayerCharacter`'s override is left untouched.

| vtable | address | `[0xA9]` target |
|---|---|---|
| `Character` (NPC) | `0x00A6FC9C` | `FUN_005E2BE0` |
| `Creature` | `0x00A710F4` | `FUN_00625710` |
| `PlayerCharacter` | `0x00A73A0C` | `FUN_0065E530` (not hooked) |

All addresses are verified against `Oblivion.exe 1.2.0.416` (image base
`0x400000`). See [`ghidra-mcp-modifications.md`](ghidra-mcp-modifications.md) for
the reverse-engineering tooling notes. The hook aborts safely if the vtable
slot doesn't match the expected original (e.g. after a game update).

## Installation

1. Install [xOBSE](https://github.com/llde/xOBSE).
2. Copy `cappeddamage.dll` (from [`build/`](build/) or a
   [release](../../releases)) into:
   ```
   <Oblivion>\Data\OBSE\Plugins\cappeddamage.dll
   ```
3. Launch the game through `obse_loader.exe`. On first run the plugin writes a
   default `cappeddamage.ini` next to the DLL.

## Configuration — `cappeddamage.ini`

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | `1` | Master switch. `0` = hook not installed; game runs unchanged. |
| `WindowSeconds` | `3.0` | Length of the damage-cap window, in seconds. |
| `HealthFraction` | `0.333` | Max fraction of max health lost per window (0.0–1.0). |
| `EnableLogging` | `0` | Write `obse_cappeddamage.log` to the game root for debugging. |

The INI is read from the same folder as the DLL and is never overwritten once
it exists.

## Building

Requires the MSVC toolchain (x64 host → **x86 target**) and the Windows SDK.
[`build.bat`](build.bat) compiles a static-CRT (`/MT`) DLL and deploys it to the
Plugins folder.

> The tool/SDK paths near the top of `build.bat` are hard-coded to one machine —
> edit `MSVC_BIN`/`MSVC_INC`/`MSVC_LIB` and the SDK paths to match your install
> before running it.

```bat
build.bat
```

Output: `build\cappeddamage.dll`.

## License

[MIT](LICENSE) © 2025 varla
