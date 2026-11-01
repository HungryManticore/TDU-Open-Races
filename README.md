# TDU Open Races

An `.asi` plugin for **Test Drive Unlimited** (PC). For any challenge you have **already won**, it:

1. **Lifts the vehicle restriction.** The car-type, brand, model and class requirement is cleared, so any car or bike can enter. The player-level requirement is kept.
2. **Re-rolls the AI opponents.** If the event has opponents, they become random vehicles from **your** class: A–G for cars, mA/mB for bikes. Cars race cars and bikes race bikes. The number of opponents is unchanged, and the game's own random AI-car picker chooses the vehicles.

Challenges you haven't won are left untouched.

> ⚠️ Built from static reverse engineering of the executable and **not yet tested in-game**. If something goes wrong, the log file (below) shows what the plugin saw and did.

## Target executable

The plugin only supports this exact build. It verifies the code at every hook site at startup and disables itself if anything differs.

| | |
|---|---|
| File | `TestDriveUnlimited.exe` (v1.66A) |
| Size | **12,263,424 bytes** (11.70 MiB) |
| MD5 | `2ef3f4e29c554cc69a019e2f576132e5` |
| Image base | `0x00400000` |

## Installation

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `dinput8.dll` in the game folder. On Wine/Proton, set a `dinput8` override to *native, builtin*.
2. Copy `TDU_WonRaceFreedom.asi` and `TDU_WonRaceFreedom.ini` into `<game>/scripts/`.

## Settings (`TDU_WonRaceFreedom.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | Master switch |
| `RequireGold` | 1 | 1 = only a 1st place / gold counts as won; 0 = any medal |
| `UnlockVehicle` | 1 | Lift the vehicle restriction on won challenges |
| `RandomizeOpponents` | 1 | Re-roll AI opponents from the player's class |
| `Log` | 1 | Write `TDU_WonRaceFreedom.log` next to the `.asi` |

## Building

You only need LLVM (`clang`, `lld-link`, `llvm-dlltool`). No Windows SDK or CRT is required.

```sh
./build.sh        # produces TDU_WonRaceFreedom.asi in the repo root
```

## How it works

All addresses are for the exe listed above.

- **Current challenge:** a global object at `0x011F30F8`. Each property's value sits at `+8`:
  - `+0x2F8` `VTyp`: vehicle type (0 any, 1 car, 2 bike)
  - `+0x378` `VSpe`: vehicle spec (0 none, 1 brand, 2 model, 3 class, 4 class range)
  - `+0x19E8` `NbIA`: number of AI opponents
  - `+0x1A80` `IAVT`: AI vehicle type
  - `+0x1B00` `IAS0`: AI vehicle spec, with its single-class sub-property at `+0x2B0`
- **"Already won":**
  - The challenge id is the game's 8-byte additive hash of the upper-cased challenge name at `+4`.
  - The profile result store comes from vtable `+0x2C` on the manager at `[0x0114C15C]`.
  - `0x890380` returns the best result: -1 none, 1 gold/1st, 2 silver, 3 bronze.
- **Player's class:** the current garage car is decrypted with `0x402F00` and its record found in the car DB at `[0x011F2CDC]`. The record's class string is matched against the game's class hashes for A–G and mA/mB.
- **Hooks**, each of which re-applies the changes:
  - both call sites of the spot/challenge loader `0x7601A0`
  - the entry check `0x73A3C0`
  - the per-car check `0x75D4E0`
  - AI opponent setup `0x73CD90`
- **Opponents:** setting `IAVT` = 1 or 2 and `IAS0` = 3 (single class) makes the game's own picker (`0x73C9A0`) choose random AI-capable vehicles of that class.
