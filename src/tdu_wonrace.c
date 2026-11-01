/*
 * TDU_WonRaceFreedom.asi  -  Test Drive Unlimited (PC, v1.66A exe) plugin
 *
 * When the player goes to a challenge they have already WON:
 *   1. the vehicle restriction is removed (any car or bike can enter), and
 *   2. if the event has AI opponents, they are re-rolled at random from the
 *      player's own class (A..G for cars, mA/mB for bikes): car vs car, bike vs bike.
 *
 * Everything is done by rewriting the game's own "current challenge" data, so
 * the game's own eligibility checks and its own random AI-car picker do the rest.
 *
 * Built freestanding (no CRT): clang --target=i686-pc-windows-msvc + lld-link.
 */
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Minimal Win32 declarations (no SDK needed)                          */
/* ------------------------------------------------------------------ */
typedef int BOOL;
typedef uint32_t DWORD;
typedef void *HANDLE;
typedef void *HMODULE;
#define WINAPI __stdcall
#define DLL_PROCESS_ATTACH 1
#define PAGE_EXECUTE_READWRITE 0x40
#define GENERIC_WRITE 0x40000000
#define FILE_SHARE_READ 1
#define OPEN_ALWAYS 4
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_END 2
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

__declspec(dllimport) BOOL WINAPI VirtualProtect(void *, size_t, DWORD, DWORD *);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);
__declspec(dllimport) BOOL WINAPI FlushInstructionCache(HANDLE, const void *, size_t);
__declspec(dllimport) DWORD WINAPI GetModuleFileNameA(HMODULE, char *, DWORD);
__declspec(dllimport) BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
__declspec(dllimport) unsigned WINAPI GetPrivateProfileIntA(const char *, const char *, int, const char *);
__declspec(dllimport) HANDLE WINAPI CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) DWORD WINAPI SetFilePointer(HANDLE, long, long *, DWORD);
__declspec(dllimport) BOOL WINAPI IsBadReadPtr(const void *, size_t);
__declspec(dllimport) int WINAPI wvsprintfA(char *, const char *, __builtin_va_list);

/* the compiler may emit these */
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
int _fltused = 0;

/* ------------------------------------------------------------------ */
/* Game addresses (TestDriveUnlimited.exe 1.66A, image base 0x400000)   */
/* ------------------------------------------------------------------ */
#define G_CUR_CHALLENGE      0x011F30F8u  /* the global "current challenge" object        */
#define G_PROFILE_MGR_PTR    0x0114C15Cu  /* -> player profile / garage manager             */
#define G_CAR_DB_PTR         0x011F2CDCu  /* -> car database                                 */
#define G_AI_CARS_COUNT      0x011F2BCCu  /* number of AI-capable cars                      */
#define G_AI_CARS_LIST       0x011F262Cu  /* -> AI-capable car list (0x30 stride, +0x20 rec) */
#define G_DESC_COUNT         0x0120BDFCu  /* number of spot/challenge descriptors           */
#define G_DESC_ARRAY         0x0120BE00u  /* -> descriptors (64 bytes each)                  */
#define FN_DECRYPT_CARID     0x00402F00u  /* stdcall uint32 (garageEntry*)                  */

/* challenge object layout (each property: value at +8) */
#define CH_NAME        0x0004   /* char[] IGE name, hashed into the challenge id      */
#define CH_VTYP        0x02F8   /* player vehicle type: 0 any, 1 car, 2 bike          */
#define CH_VSPE        0x0378   /* player vehicle spec: 0 none,1 brand,2 model,3 class,4 class range */
#define CH_NBIA        0x19E8   /* number of AI opponents                             */
#define CH_IAVT        0x1A80   /* AI vehicle type                                    */
#define CH_IAS0        0x1B00   /* AI vehicle spec (same layout as CH_VSPE)           */
#define SPEC_CLASS_A   0x02B0   /* spec sub-property: single class                    */
#define PROP_VALUE     0x08
#define PROP_MIN       0x0C
#define PROP_MAX       0x10
#define PROP_ENUMTBL   0x78

#define TBL_CAR_CLASSES  0x00F941D8u   /* hash table A..G  (7 entries) */
#define TBL_BIKE_CLASSES 0x00F94210u   /* hash table mA,mB (2 entries) */

static const uint32_t kClassHash[9] = {
    0x0467A0E8, 0x0476E328, 0x04862568, 0x049567A8, 0x04A4A9E8, 0x04B3EC28, 0x04C32E68, /* A..G  */
    0x0493E108, 0x04A32348                                                              /* mA,mB */
};
static const char *kClassName[9] = { "A", "B", "C", "D", "E", "F", "G", "mA", "mB" };

/* asm thunks (hooks.S) */
extern int  __cdecl CallGetCup(void *store, uint32_t idLo, uint32_t idHi);
extern void Hook_CanEnter(void);
extern void Hook_CarMatchesCurrent(void);
extern void Hook_AISetup(void);
extern void Wrap_LoadChallenge(void);

/* ------------------------------------------------------------------ */
/* Settings + log                                                       */
/* ------------------------------------------------------------------ */
static int  cfgEnabled = 1, cfgRequireGold = 1, cfgUnlock = 1, cfgOpponents = 1, cfgLog = 1;
static char g_logPath[300];

static void Log(const char *fmt, ...)
{
    if (!cfgLog) return;
    char buf[1100];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);   /* output is capped at 1024 chars */
    __builtin_va_end(ap);
    if (n <= 0) return;
    buf[n++] = '\r'; buf[n++] = '\n';
    HANDLE h = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, 0, FILE_END);
    DWORD w; WriteFile(h, buf, (DWORD)n, &w, 0);
    CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */
#define U8(p)  (*(volatile uint8_t  *)(p))
#define U16(p) (*(volatile uint16_t *)(p))
#define U32(p) (*(volatile uint32_t *)(p))

static int Readable(uint32_t p, size_t n) { return p >= 0x10000 && !IsBadReadPtr((void *)p, n); }

/* challenge id = the game's 8-byte additive hash of the (upper-cased) IGE name */
static int ChallengeId(uint32_t chal, uint32_t *lo, uint32_t *hi, const char **nameOut)
{
    const char *name = (const char *)(chal + CH_NAME);
    uint8_t acc[8] = {0};
    int len = 0;
    while (len < 64 && name[len]) {
        int c = (signed char)name[len];
        if (c >= 'a' && c <= 'z') c -= 0x20;
        acc[len & 7] = (uint8_t)(acc[len & 7] + c);
        len++;
    }
    if (len == 0 || len >= 64) return 0;
    *lo = acc[0] | (acc[1] << 8) | (acc[2] << 16) | ((uint32_t)acc[3] << 24);
    *hi = acc[4] | (acc[5] << 8) | (acc[6] << 16) | ((uint32_t)acc[7] << 24);
    *nameOut = name;
    return 1;
}

typedef void *(__thiscall *VFn0)(void *self);
static void *ProfileMgr(void) { uint32_t m = U32(G_PROFILE_MGR_PTR); return Readable(m, 4) ? (void *)m : 0; }
static void *VCall0(void *obj, int off) { return ((VFn0)(*(uint32_t **)obj)[off / 4])(obj); }

/* best result stored in the profile for this challenge: -1 none, 1 gold, 2 silver, 3 bronze */
static int GetCup(uint32_t lo, uint32_t hi)
{
    void *mgr = ProfileMgr();
    if (!mgr) return -1;
    void *store = VCall0(mgr, 0x2C);
    if (!store) return -1;
    return CallGetCup(store, lo, hi);
}

static uint32_t FindCarRecord(uint32_t carId)
{
    uint32_t db = U32(G_CAR_DB_PTR);
    if (!Readable(db, 0x5A8)) return 0;
    uint32_t base = U32(db + 4), n = U32(db + 0x5A4);
    if (!base || n > 4096) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = base + i * 0x1F8;
        if (U32(r + 0x1F0) == carId) return r;
    }
    return 0;
}

/* The car DB stores a car's class as a pointer into its string table; the game maps
   that pointer back to a hash and the hash to a class.  Resolve the 9 class-string
   pointers once (same lookup as game fn 0x9AE600) and then just compare pointers. */
static uint32_t classStr[9]; static int classStrReady;

static int ResolveClassStrings(void)
{
    if (classStrReady) return 1;
    uint32_t db = U32(G_CAR_DB_PTR);
    if (!Readable(db, 0x100)) return 0;
    uint32_t tbl = db + 0x94;
    uint32_t ents = U32(tbl + 0x40), cnt = U32(tbl + 0x44), sbase = U32(tbl + 0x48);
    if (!ents || !sbase || cnt == 0 || cnt > 500000 || !Readable(ents, cnt * 16)) return 0;
    int got = 0;
    for (int k = 0; k < 9; k++) {
        classStr[k] = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t e = ents + i * 16;
            if (U32(e) == kClassHash[k] && U32(e + 4) == 0) { classStr[k] = sbase + U32(e + 8); got++; break; }
        }
    }
    if (got == 9) { classStrReady = 1; Log("Class table resolved (A..G, mA, mB)"); }
    return classStrReady;
}

/* class index of a car record: 0..6 = A..G (cars), 7..8 = mA/mB (bikes), -1 unknown */
static int ClassOfRecord(uint32_t rec)
{
    if (!ResolveClassStrings()) return -1;
    uint32_t s = U32(rec + 0x20);
    for (int k = 0; k < 9; k++) if (classStr[k] == s) return k;
    return -1;
}

/* class of the car the player is currently driving */
static int PlayerClass(uint32_t *carIdOut)
{
    void *mgr = ProfileMgr();
    if (!mgr) return -1;
    uint32_t cur = (uint32_t)VCall0(mgr, 0x1C);
    uint32_t garage = (uint32_t)VCall0(mgr, 0x24);
    if (!Readable(cur, 0x48) || !Readable(garage, 0x20)) return -1;
    uint32_t idx = U16(cur + 0x44);
    if (idx > 0x280) return -1;
    uint32_t entry = garage + 0x20 + idx * 0x80;
    if (!Readable(entry, 0x80)) return -1;
    uint32_t carId = ((uint32_t(__stdcall *)(uint32_t))FN_DECRYPT_CARID)(entry);
    *carIdOut = carId;
    uint32_t rec = FindCarRecord(carId);
    return rec ? ClassOfRecord(rec) : -1;
}

/* does the game have at least one AI-capable car in this class? */
static int AICarsInClass(int cls)
{
    uint32_t n = U32(G_AI_CARS_COUNT), list = U32(G_AI_CARS_LIST);
    if (!list || n > 4096) return 0;
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rec = U32(list + i * 0x30 + 0x20);
        if (rec && ClassOfRecord(rec) == cls) found++;
    }
    return found;
}

static void PatchDescriptor(uint32_t lo, uint32_t hi)
{
    uint32_t n = U32(G_DESC_COUNT), arr = U32(G_DESC_ARRAY);
    if (!arr || n > 10000) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t d = arr + i * 64;
        if (U32(d) == lo && U32(d + 4) == hi) {
            U8(d + 0x23) = 0;   /* vehicle type: any   */
            U8(d + 0x30) = 0;   /* vehicle spec: none  */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Core                                                                 */
/* ------------------------------------------------------------------ */
static uint32_t lastLo = 0xFFFFFFFF, lastHi = 0xFFFFFFFF; static int lastCls = -99, lastWon = -99;

static void Apply(uint32_t chal, const char *where, int doAI, int fast)
{
    if (!cfgEnabled || !Readable(chal, 0x2000)) return;

    uint32_t lo, hi; const char *name;
    if (!ChallengeId(chal, &lo, &hi, &name)) return;

    int sameChallenge = (lo == lastLo && hi == lastHi);
    int won;
    if (fast && sameChallenge && lastWon != -99) {
        won = lastWon;                      /* car-list check: called per car, reuse result */
    } else {
        int cup = GetCup(lo, hi);
        won = cfgRequireGold ? (cup == 1) : (cup >= 1 && cup <= 3);
        if (!sameChallenge || won != lastWon)
            Log("[%s] challenge '%s' id=%08X%08X best result=%d -> %s", where, name, hi, lo, cup,
                won ? "already WON: applying changes" : "not won yet, left untouched");
        if (!sameChallenge) lastCls = -99;
        lastLo = lo; lastHi = hi; lastWon = won;
    }
    if (!won) return;

    if (cfgUnlock) {
        U32(chal + CH_VTYP + PROP_VALUE) = 0;    /* any vehicle type */
        U32(chal + CH_VSPE + PROP_VALUE) = 0;    /* no brand/model/class requirement */
        if (!fast) PatchDescriptor(lo, hi);
    }

    if (cfgOpponents && doAI) {
        int nbAI = (int)U32(chal + CH_NBIA + PROP_VALUE);
        uint32_t carId = 0;
        if (nbAI <= 0) {
            if (lastCls != -2) Log("  no AI opponents in this event");
            lastCls = -2;
            return;
        }
        int cls = PlayerClass(&carId);
        if (cls < 0) {
            if (lastCls != -3) Log("  could not determine the class of the player's car (id %u), opponents untouched", carId);
            lastCls = -3;
            return;
        }
        if (!AICarsInClass(cls)) {
            if (lastCls != -4) Log("  game has no AI-capable vehicles in class %s, opponents untouched", kClassName[cls]);
            lastCls = -4;
            return;
        }
        int bike = cls >= 7;
        uint32_t p = chal + CH_IAS0 + SPEC_CLASS_A;
        U32(chal + CH_IAVT + PROP_VALUE) = bike ? 2 : 1;      /* bikes only / cars only */
        U32(chal + CH_IAS0 + PROP_VALUE) = 3;                 /* spec mode: single class */
        U32(p + PROP_ENUMTBL) = bike ? TBL_BIKE_CLASSES : TBL_CAR_CLASSES;
        U32(p + PROP_MIN) = 0;
        U32(p + PROP_MAX) = bike ? 1 : 6;
        U32(p + PROP_VALUE) = (uint32_t)(bike ? cls - 7 : cls);
        if (cls != lastCls)
            Log("  %d opponent(s) -> random %s of class %s (player vehicle id %u)", nbAI, bike ? "bikes" : "cars",
                kClassName[cls], carId);
        lastCls = cls;
    }
}

/* called from the asm hooks (game thread, all registers preserved by the thunk) */
void __cdecl OnChallengeLoaded(uint32_t chal) { Apply(chal, "spot loaded", 1, 0); }
void __cdecl OnCanEnter(uint32_t chal)        { Apply(chal, "entry check", 1, 0); }
void __cdecl OnCarListCheck(uint32_t chal)    { Apply(chal, "car check",   0, 1); }
void __cdecl OnAISetup(uint32_t chal)         { Apply(chal, "race start",  1, 0); }

/* ------------------------------------------------------------------ */
/* Patching                                                             */
/* ------------------------------------------------------------------ */
static int BytesMatch(uint32_t addr, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) if (U8(addr + i) != b[i]) return 0;
    return 1;
}

static void WriteRel(uint32_t at, uint8_t op, uint32_t target, int len)
{
    DWORD old;
    VirtualProtect((void *)at, len, PAGE_EXECUTE_READWRITE, &old);
    U8(at) = op;
    U32(at + 1) = target - (at + 5);
    for (int i = 5; i < len; i++) U8(at + i) = 0x90;
    VirtualProtect((void *)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)at, len);
}

static const uint8_t sig_CanEnter[]  = {0x8B, 0x4C, 0x24, 0x08, 0x83, 0xEC, 0x1C};              /* 0x73A3C0 */
static const uint8_t sig_CarMatch[]  = {0x83, 0xEC, 0x0C, 0xA1, 0xF0, 0x33, 0x1F, 0x01};        /* 0x75D4E0 */
static const uint8_t sig_AISetup[]   = {0xD9, 0x05, 0xAC, 0x3F, 0xFA, 0x00};                    /* 0x73CD90 */
static const uint8_t sig_Call1[]     = {0xE8, 0x6D, 0xFB, 0xFF, 0xFF};                          /* 0x76062E call 0x7601A0 */
static const uint8_t sig_Call2[]     = {0xE8, 0xE1, 0xF6, 0xFF, 0xFF};                          /* 0x760ABA call 0x7601A0 */
static const uint8_t sig_GetCup[]    = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04};        /* 0x890380 */
static const uint8_t sig_DecCar[]    = {0x83, 0xEC, 0x08, 0x56, 0x8D, 0x44, 0x24, 0x04};        /* 0x402F00 */

static void Install(void)
{
    int ok = BytesMatch(0x73A3C0, sig_CanEnter, sizeof sig_CanEnter) &&
             BytesMatch(0x75D4E0, sig_CarMatch, sizeof sig_CarMatch) &&
             BytesMatch(0x73CD90, sig_AISetup, sizeof sig_AISetup) &&
             BytesMatch(0x76062E, sig_Call1, sizeof sig_Call1) &&
             BytesMatch(0x760ABA, sig_Call2, sizeof sig_Call2) &&
             BytesMatch(0x890380, sig_GetCup, sizeof sig_GetCup) &&
             BytesMatch(0x402F00, sig_DecCar, sizeof sig_DecCar);
    if (!ok) {
        Log("Unsupported TestDriveUnlimited.exe (code signature mismatch) - plugin disabled. Needs the 1.66A exe.");
        return;
    }
    WriteRel(0x76062E, 0xE8, (uint32_t)&Wrap_LoadChallenge, 5);
    WriteRel(0x760ABA, 0xE8, (uint32_t)&Wrap_LoadChallenge, 5);
    WriteRel(0x73A3C0, 0xE9, (uint32_t)&Hook_CanEnter, 7);
    WriteRel(0x75D4E0, 0xE9, (uint32_t)&Hook_CarMatchesCurrent, 8);
    WriteRel(0x73CD90, 0xE9, (uint32_t)&Hook_AISetup, 6);
    Log("Hooks installed. RequireGold=%d UnlockVehicle=%d RandomizeOpponents=%d", cfgRequireGold, cfgUnlock, cfgOpponents);
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return 1;
    DisableThreadLibraryCalls(mod);

    char ini[300];
    DWORD n = GetModuleFileNameA(mod, ini, sizeof(ini) - 8);
    if (n == 0 || n >= sizeof(ini) - 8) return 1;
    int dot = (int)n; while (dot > 0 && ini[dot] != '.' && ini[dot] != '\\') dot--;
    if (ini[dot] != '.') dot = (int)n;
    memcpy(g_logPath, ini, dot); memcpy(g_logPath + dot, ".log", 5);
    memcpy(ini + dot, ".ini", 5);

    cfgEnabled     = (int)GetPrivateProfileIntA("Settings", "Enabled", 1, ini);
    cfgRequireGold = (int)GetPrivateProfileIntA("Settings", "RequireGold", 1, ini);
    cfgUnlock      = (int)GetPrivateProfileIntA("Settings", "UnlockVehicle", 1, ini);
    cfgOpponents   = (int)GetPrivateProfileIntA("Settings", "RandomizeOpponents", 1, ini);
    cfgLog         = (int)GetPrivateProfileIntA("Settings", "Log", 1, ini);

    if (cfgLog) { /* start a fresh log each session */
        HANDLE h = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    Log("TDU_WonRaceFreedom loaded");
    if (cfgEnabled) Install();
    return 1;
}
