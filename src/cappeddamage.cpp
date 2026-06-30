/*
 * CappedDamage - xOBSE plugin for original Oblivion (32-bit)
 *
 * Port of the OBSE64 "capped-player-to-enemy-damage" plugin to the original
 * 32-bit Oblivion engine + xOBSE.
 *
 * Caps how much HEALTH any non-player actor can lose within a rolling time
 * window to a configurable fraction of that actor's MAX health. The window
 * opens on the first health loss and does NOT refresh; once it elapses the next
 * hit opens a fresh window. The cap clamps the damage BEFORE it is applied, so a
 * full-health enemy can never be one-shot (an overkill hit leaves it at
 * (1 - fraction) HP). Window length and the fraction are read from
 * cappeddamage.ini (next to the DLL).
 *
 * Mechanism (differs from the 64-bit build): a vtable-slot hook on the
 * Actor::DamageAV_F float damage function at vtable index 0xA9.
 *   void __thiscall DamageAV_F(Actor* this, UInt32 avCode, float amt, Actor* arg2)
 * In 32-bit Oblivion 1.2.0.416, Character (NPCs) and Creature have DISTINCT
 * implementations of this slot, and PlayerCharacter has its own override, so we
 * hook only the Character and Creature vtable slots:
 *   kVtbl_Character (0x00A6FC9C)[0xA9] -> FUN_005E2BE0
 *   kVtbl_Creature  (0x00A710F4)[0xA9] -> FUN_00625710
 * The player (kVtbl_PlayerCharacter 0x00A73A0C[0xA9] -> FUN_0065E530) is left
 * untouched, so the player is structurally exempt; we also guard at runtime with
 * an actor != *g_thePlayer check. Pre-hit health is read by calling the actor's
 * own GetActorValue (vtable index 0xA1) with kActorVal_Health (8); the max-ever-
 * seen health is cached per actor. See ghidra-mcp-modifications.md / xOBSE memory
 * for the RE details. Addresses verified against Oblivion.exe 1.2.0.416.
 */

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <mutex>

/* ===================== config (cappeddamage.ini) ===================== */
static bool  g_enabled        = true;    // [CappedDamage] Enabled (master switch)
static bool  g_loggingEnabled = false;   // [CappedDamage] EnableLogging
static DWORD g_windowMs       = 3000;     // [CappedDamage] WindowSeconds
static float g_fraction       = 0.3333f;  // [CappedDamage] HealthFraction (0..1)
static char  g_iniPath[MAX_PATH] = {0};

static void log_message(const char *fmt, ...)
{
    if (!g_loggingEnabled) return;
    FILE *f = fopen("obse_cappeddamage.log", "a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args; va_start(args, fmt); vfprintf(f, fmt, args); va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// Resolve "<this-dll-dir>\cappeddamage.ini" from the DLL's own module path.
static void ResolveIniPath()
{
    HMODULE hm = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ResolveIniPath, &hm);
    DWORD n = GetModuleFileNameA(hm, g_iniPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { strcpy(g_iniPath, "cappeddamage.ini"); return; }
    char *dot = strrchr(g_iniPath, '.');           // ...\cappeddamage.dll -> .ini
    if (dot) strcpy(dot, ".ini"); else strcat(g_iniPath, ".ini");
}

// Write a commented default ini if none exists (never clobbers the user's edits).
static void WriteDefaultIniIfMissing()
{
    if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE *f = fopen(g_iniPath, "w");
    if (!f) return;
    fprintf(f,
        "[CappedDamage]\n"
        "; Master on/off switch for this plugin. 1 = enabled, 0 = disabled.\n"
        "; When 0 the damage hook is NOT installed and the game runs exactly as\n"
        "; if this plugin weren't present. Default 1\n"
        "Enabled=1\n\n"
        "; Length of the damage-cap window, in seconds. Default 3.0\n"
        "WindowSeconds=3.0\n\n"
        "; Max fraction of an actor's MAX health it can lose per window (0.0 - 1.0).\n"
        "; 0.333 = one third. Default 0.333\n"
        "HealthFraction=0.333\n\n"
        "; Write a debug log (obse_cappeddamage.log in the game's root folder).\n"
        "; 0 = off, 1 = on. Default 0\n"
        "EnableLogging=0\n");
    fclose(f);
}

static void LoadConfig()
{
    ResolveIniPath();
    WriteDefaultIniIfMissing();

    g_enabled = GetPrivateProfileIntA("CappedDamage", "Enabled", 1, g_iniPath) != 0;

    char buf[64];
    GetPrivateProfileStringA("CappedDamage", "WindowSeconds", "3.0", buf, sizeof(buf), g_iniPath);
    float windowSec = (float)atof(buf);
    if (windowSec <= 0.0f) windowSec = 3.0f;
    g_windowMs = (DWORD)(windowSec * 1000.0f);

    GetPrivateProfileStringA("CappedDamage", "HealthFraction", "0.333", buf, sizeof(buf), g_iniPath);
    g_fraction = (float)atof(buf);
    if (g_fraction <= 0.0f || g_fraction > 1.0f) g_fraction = 0.3333f;

    g_loggingEnabled = GetPrivateProfileIntA("CappedDamage", "EnableLogging", 0, g_iniPath) != 0;
}

/* ===================== OBSE plugin API (minimal, self-contained) =====================
 * Only the pieces xOBSE's PluginManager actually touches. It loads the DLL,
 * GetProcAddress's "OBSEPlugin_Query"/"OBSEPlugin_Load", calls Query to fill
 * PluginInfo (requires infoVersion >= 2 and a non-null name), then calls Load.
 */
struct PluginInfo {
    enum { kInfoVersion = 3 };
    uint32_t     infoVersion;
    const char * name;
    uint32_t     version;
};

// Leading fields of OBSEInterface (stable across OBSE history) - just enough to
// detect the editor and log the runtime version.
struct OBSEInterfaceMin {
    uint32_t obseVersion;
    uint32_t oblivionVersion;
    uint32_t editorVersion;
    uint32_t isEditor;
};

/* ===================== symbols (Oblivion.exe 1.2.0.416, image base 0x400000) ===================== */
static const uintptr_t kVtbl_Character = 0x00A6FC9C;   // NPC vtable
static const uintptr_t kVtbl_Creature  = 0x00A710F4;   // Creature vtable
static const uintptr_t kPlayerPtr      = 0x00B333C4;   // PlayerCharacter** (g_thePlayer)

static const int kVIdx_DamageAV_F    = 0xA9;           // vtable[0xA9] float DamageActorValue
static const int kVIdx_GetActorValue = 0xA1;           // vtable[0xA1] current cumulative AV
static const int kAV_Health          = 8;              // kActorVal_Health

static const uintptr_t kExpect_CharDamage = 0x005E2BE0;   // Character::DamageAV_F
static const uintptr_t kExpect_CreaDamage = 0x00625710;   // Creature::DamageAV_F

typedef void (__thiscall *DamageFn)(void *actor, uint32_t avCode, float amt, void *arg2);
typedef int  (__thiscall *GetAVFn)(void *actor, uint32_t avCode);

static DamageFn g_origCharDamage = nullptr;
static DamageFn g_origCreaDamage = nullptr;

static void *GetPlayer() { return *reinterpret_cast<void **>(kPlayerPtr); }

static int GetCurrentHealth(void *actor)
{
    uintptr_t vtbl = *reinterpret_cast<uintptr_t *>(actor);
    GetAVFn fn = reinterpret_cast<GetAVFn>(*reinterpret_cast<uintptr_t *>(vtbl + kVIdx_GetActorValue * 4));
    return fn(actor, kAV_Health);
}

/* ===================== per-enemy budget: fraction*maxHP per window ===================== */
struct Window { float maxHP; bool active; DWORD start; float budget; };
static std::unordered_map<void *, Window> g_windows;
static std::mutex g_mtx;

// Clamp an incoming health delta; returns the (negative) delta to actually apply.
static float ApplyCap(void *actor, uint32_t avCode, float delta)
{
    if (avCode == kAV_Health && delta < 0.0f && actor != nullptr && actor != GetPlayer()) {
        float curHP = (float)GetCurrentHealth(actor);   // pre-hit health (detour runs before apply)
        if (curHP > 0.0f) {
            float dmg = -delta;
            float maxHP, cap, allowed, budgetAfter;
            DWORD now = GetTickCount();
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                if (g_windows.size() > 512) {
                    for (auto it = g_windows.begin(); it != g_windows.end(); ) {
                        if (now - it->second.start >= g_windowMs) it = g_windows.erase(it);
                        else ++it;
                    }
                }
                Window &w = g_windows[actor];
                if (curHP > w.maxHP) w.maxHP = curHP;       // cache true max (first full-HP sighting / heals)
                maxHP = w.maxHP;
                cap = maxHP * g_fraction;
                if (!w.active || (now - w.start) >= g_windowMs) {
                    w.active = true; w.start = now; w.budget = cap;
                }
                allowed = dmg;
                if (allowed > w.budget) allowed = w.budget;
                if (allowed < 0.0f) allowed = 0.0f;
                w.budget -= allowed;
                budgetAfter = w.budget;
            }
            if (dmg >= 1.0f)
                log_message("HEALTH dmg actor=%p maxHP=%.0f cur=%.0f cap=%.1f incoming=%.1f -> allowed=%.1f (budgetLeft=%.1f)%s",
                            actor, maxHP, curHP, cap, dmg, allowed, budgetAfter, (allowed < dmg) ? "  [CAPPED]" : "");
            return -allowed;
        }
    }
    return delta;
}

/* The vtable slot is invoked __thiscall (this in ECX; avCode/amt/arg2 on stack).
 * We emulate that with __fastcall + a dummy EDX param: ECX=this, EDX=unused, the
 * remaining args land on the stack in the same order, and __fastcall's callee
 * stack cleanup matches __thiscall's. */
static void __fastcall Detour_CharDamage(void *actor, void * /*edx*/, uint32_t avCode, float amt, void *arg2)
{
    amt = ApplyCap(actor, avCode, amt);
    g_origCharDamage(actor, avCode, amt, arg2);
}

static void __fastcall Detour_CreaDamage(void *actor, void * /*edx*/, uint32_t avCode, float amt, void *arg2)
{
    amt = ApplyCap(actor, avCode, amt);
    g_origCreaDamage(actor, avCode, amt, arg2);
}

/* ===================== vtable-slot hook ===================== */
static bool patch_vtable_slot(uintptr_t vtblBase, int index, void *detour,
                              DamageFn *outOrig, uintptr_t expectOrig, const char *label)
{
    uintptr_t slotAddr = vtblBase + (uintptr_t)index * 4;
    DWORD old;
    if (!VirtualProtect((void *)slotAddr, 4, PAGE_READWRITE, &old)) {
        log_message("ERROR: VirtualProtect failed for %s slot @ %p (%lu)", label, (void *)slotAddr, GetLastError());
        return false;
    }
    uintptr_t cur = *reinterpret_cast<uintptr_t *>(slotAddr);
    if (cur != expectOrig) {
        log_message("ERROR: %s DamageAV_F mismatch @ %p (got %p expected %p) - aborting (game updated?)",
                    label, (void *)slotAddr, (void *)cur, (void *)expectOrig);
        VirtualProtect((void *)slotAddr, 4, old, &old);
        return false;
    }
    *outOrig = reinterpret_cast<DamageFn>(cur);
    *reinterpret_cast<uintptr_t *>(slotAddr) = reinterpret_cast<uintptr_t>(detour);
    VirtualProtect((void *)slotAddr, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)slotAddr, 4);
    log_message("SUCCESS: hooked %s DamageAV_F slot @ %p (orig %p)", label, (void *)slotAddr, (void *)cur);
    return true;
}

static bool install_hook()
{
    bool okChar = patch_vtable_slot(kVtbl_Character, kVIdx_DamageAV_F, &Detour_CharDamage,
                                    &g_origCharDamage, kExpect_CharDamage, "Character");
    bool okCrea = patch_vtable_slot(kVtbl_Creature, kVIdx_DamageAV_F, &Detour_CreaDamage,
                                    &g_origCreaDamage, kExpect_CreaDamage, "Creature");
    return okChar && okCrea;
}

/* ===================== OBSE plugin entry points ===================== */
extern "C" {

bool OBSEPlugin_Query(const OBSEInterfaceMin *obse, PluginInfo *info)
{
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name        = "CappedDamage";
    info->version     = 1;

    LoadConfig();   // also creates the default ini next to the DLL on first run
    log_message("==================================================");
    log_message("CappedDamage OBSEPlugin_Query - ini=%s obseVer=%08X oblivionVer=%08X editor=%u",
                g_iniPath, obse ? obse->obseVersion : 0, obse ? obse->oblivionVersion : 0,
                obse ? obse->isEditor : 0);
    return true;
}

bool OBSEPlugin_Load(const OBSEInterfaceMin *obse)
{
    log_message("config: Enabled=%d WindowSeconds=%.2f HealthFraction=%.3f EnableLogging=%d",
                g_enabled ? 1 : 0, g_windowMs / 1000.0f, g_fraction, g_loggingEnabled ? 1 : 0);

    if (obse && obse->isEditor) {
        log_message("CappedDamage: loaded in editor - hook NOT installed (runtime only)");
        return true;
    }

    if (!g_enabled) {
        log_message("CappedDamage: disabled via INI (Enabled=0) - hook NOT installed, damage unchanged");
        return true;
    }

    if (install_hook())
        log_message("CappedDamage: hook installed OK - non-player health loss capped to %.0f%% max per %.1fs",
                    g_fraction * 100.0f, g_windowMs / 1000.0f);
    else
        log_message("CappedDamage: hook NOT installed (damage unchanged)");
    return true;
}

}   // extern "C"
