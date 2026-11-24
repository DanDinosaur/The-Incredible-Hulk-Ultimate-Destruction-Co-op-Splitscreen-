// Split-screen co-op, step 1 (hc01): a second Hulk on controller port 2 (USA/Asia disc only).
//
// The game has one human controller all the way down: CControllerManager holds one CController (it merges the
// four pads), CSimControllerManager holds one CSimController built on it, and a character's CControllerAgent reads
// the sim controller at the character's controller index (+0x20A8), which the game only accepts as 0. These hooks
// add a second CController that listens to port 2 only and a second CSimController on it, answer index 1 with them,
// and spawn P2 from P1's own character template beside P1 with controller index 1. While P2 is in the game, P1's
// controller listens to port 1 only (menus still take any pad).
//
// Respawn rules (Turok / Force Unleashed co-op): a downed player is held, his partner carries on, and he comes back
// beside the partner after a few real seconds; both down is the game's own death (checkpoint restart).
// hc01 only re-spawns P2 when his actor is gone; the held/black-half rules come with split screen (hc02/hc03).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include "app/app.h"
#include "common/cheats.h"
#include "imgui.h"
#include "common/simulation.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

extern std::atomic<int> g_kb_polls, g_kb_ly, g_kb_lx;
// freeze finder: each hook notes its name; a watchdog thread logs the last one (and flushes the log) when the game
// stops calling them for 3 s, and flushes the log every 5 s so it survives the game being killed
std::atomic<const char*> g_crumb{"none"};
std::atomic<int64_t> g_beat_ms{0};
inline int64_t CrumbNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct Crumb {
    const char* prev;
    explicit Crumb(const char* n) : prev(g_crumb.exchange(n)) { g_beat_ms = CrumbNowMs(); }
    ~Crumb() { g_crumb = prev; }
};
// hc30 freeze finder, part 2: the game thread's guest context (set by Tick) so the watchdog can list the return
// addresses on its guest stack (where the game is stuck), and the last frame drawn (a loading screen still draws)
std::atomic<X86Context*> g_main_ctx{nullptr};
std::atomic<int64_t> g_render_ms{0};
// hc30: the level is being ended (0x86980 -> 0x88B80): P2 is let go first and the hooks leave him alone until the
// world runs again
std::atomic<bool> g_unloading{false};
std::atomic<int> g_unload_gen{0};
namespace {
using kernel::R32;
using kernel::W32;

// globals and objects (USA d5e7da38)
constexpr uint32_t kApp = 0x4DD700;       // [kApp]: +0x04 CSimControllerManager, +0x20 CActorManager
constexpr uint32_t kGameCtx = 0x4DD6EC;   // [kGameCtx]: +0x10 CControllerManager
constexpr uint32_t kGuestNew = 0x8D690;   // operator new (cdecl size)

// controllers
constexpr uint32_t kControllerCtor = 0xAE4B0;    // CController(int) thiscall, 0x174 bytes, makes 4 port listeners
constexpr uint32_t kControllerPre = 0xAD960;     // listeners' per-frame update, before the pads are polled
constexpr uint32_t kControllerMerge = 0xAE130;   // merges the listeners into +0x18.. after the poll
constexpr uint32_t kControllerScheme = 0xADC00;  // control scheme (1..3) on every listener
constexpr uint32_t kSimCtor = 0xB3B80;           // CSimController(CController*) thiscall, 0x224 bytes
constexpr uint32_t kSimClear = 0xB3B50;          // (1) thiscall, used while input is off
constexpr uint32_t kCtrlCount = 0x10, kCtrlListeners = 0x14, kListenerScheme = 0x4AC;

// actors
constexpr uint32_t kCreateActorScript = 0x2BF80;  // am_CreateActorAtPosition (mgr; template, x, y, z, facing, team, path or "none", bool, name)
constexpr uint32_t kHandleToActor = 0x59C10;      // thiscall (handle) on [[mgr]+0x44]
constexpr uint32_t kSetPosition = 0x3F400;        // thiscall character (const Vector*)
constexpr uint32_t kSetHealth = 0x3B1C0;          // thiscall character (value, force), ret 8 (USA; EU is 0x3B190)
constexpr uint32_t kMainActor = 0x124, kActorCount = 0xC, kActorHandles = 0x10;
constexpr uint32_t kTemplate = 0x11C, kTemplateName = 0x3B0, kName = 0x20B8, kBrain = 0x1CA0, kTeam = 0x118;
constexpr uint32_t kControllerIndex = 0x20A8, kHealth = 0x1B0C, kMaxHealth = 0x1B10;
constexpr char kP2Name[] = "coop_p2";

// coop.ini beside the game (written with the defaults on first start):
//   p1_pad = 1          the pad that drives P1 while P2 plays (1-4, in the order Windows connected them)
//   p2_pad = 2          the pad that drives P2
//   split = 1           0 keeps one full screen on P1
//   respawn_seconds = 9 how long a downed player waits
//   fall_rescue = 0     1: a player who falls 40 m below his partner is put beside him (the mod menu sets it)
//   keyboard_player = 1 which player the keyboard drives (2 = P2, for testing alone); F8 swaps it in game
struct Settings {
    int keyboard_player = 1;
    int p1_pad = 0, p2_pad = 1;  // 0-based
    bool split = true;
    int respawn_seconds = 9;
    bool fall_rescue = false;  // no automatic teleports unless switched on (the mod menu has the manual ones)
    bool split_vertical = false;  // the halves side by side (P1 left) instead of top and bottom
    bool god[2] = {false, false};  // per player god mode (the mod menu)
    bool menus_4_3 = false;  // (off: the user finds the menus fine) the game's menus and popups in the 4:3 middle of a widescreen picture, not stretched
    bool follow_warps = false;  // P2 joins P1 when the game moves P1 far in one go (cutscenes, mission starts)
    int red_hulk_player = 2;     // which player is drawn as Red Hulk: 1, 2, or 0 for neither (the mod menu sets it)
    bool prop_sort = true;       // hc38: props loaded nearest to EITHER camera first (0 = the game's P1-only order)
    int prop_cap = 100;           // hc41: props loaded at once while P2 plays (game 45/50; hc38's 120 ran a heap out: freeze)
    float prop_radius = 1200.0f;  // hc41: props kept within this many m (loaded within +25%) while P2 plays (hc38: 1600)
    bool friendly_fire = false;
    bool show_smashable = true;
    int skin_p[2] = {-1, -1};
    bool skins_diag = false;
    // hc86: the carrier's arms over his head while carrying (pose override on the shoulders / elbows)
    bool carry_pose = true;
    int carry_pose_test = 0;          // 1: P1's arms always up (for tuning), 2: P2's
    float carry_out = 0.35f, carry_in = 0.35f, carry_height = 3.3f;  // hc92: 3.3 = on the raised hands (wrists 3.2-3.5 m)  // hc89: how far out (sideways) the raised arms lean, per 1 up
    bool skins_preload_all = false;  // hc82: load all 13 skin packages at each level/checkpoint load (instant switching)
    bool skins_hide_swaps = false;   // hc82: the menu skips the stretched skin-swap Abomination / Savage Banner (2, 3)  // hc80: the skin loader's diagnostic logging (heap trees, loader checks, pose dumps)
    bool skins_test = false;  // hc68: the per-player skin loading (crashes the game so far) only with skins_test = 1  // hc50: each player's own skin (-1 the level's, 0-12), switched mid-game
    int skin = -1;  // hc47: the Hulk skin loaded at each level/checkpoint load (-1 the game's choice, 0-12 below)  // hc45: destructible buildings on the radar (smash icons)  // hc43: off = neither player can lock on to / auto-aim at the other
    float zoom_side = 0.7f;      // hc37: P1's D-pad camera distance in side-by-side split (P2 starts with it)
};
const Settings& Config() {
    static const Settings s = [] {
        Settings c;
        const char* path = "coop.ini";
        if (std::FILE* f = std::fopen(path, "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) {
                char key[64];
                float v;
                if (std::sscanf(line, " %63[a-z_0-9] = %f", key, &v) != 2) continue;
                const std::string k = key;
                if (k == "p1_pad") c.p1_pad = std::clamp(int(v), 1, 4) - 1;
                else if (k == "p2_pad") c.p2_pad = std::clamp(int(v), 1, 4) - 1;
                else if (k == "split") c.split = v != 0;
                else if (k == "respawn_seconds") c.respawn_seconds = std::clamp(int(v), 1, 60);
                else if (k == "fall_rescue") c.fall_rescue = v != 0;
                else if (k == "split_vertical") c.split_vertical = v != 0;
                else if (k == "god_p1") c.god[0] = v != 0;
                else if (k == "god_p2") c.god[1] = v != 0;
                else if (k == "menus_4_3") c.menus_4_3 = v != 0;
                else if (k == "follow_warps") c.follow_warps = v != 0;
                else if (k == "red_hulk_player") c.red_hulk_player = std::clamp(int(v), 0, 2);
                else if (k == "prop_sort") c.prop_sort = v != 0;
                else if (k == "friendly_fire") c.friendly_fire = v != 0;
                else if (k == "show_smashable") c.show_smashable = v != 0;
                else if (k == "skin") c.skin = std::clamp(int(v), -1, 12);
                else if (k == "skins_test") c.skins_test = v != 0;
                else if (k == "skins_diag") c.skins_diag = v != 0;
                else if (k == "skins_preload_all") c.skins_preload_all = v != 0;
                else if (k == "skins_hide_swaps") c.skins_hide_swaps = v != 0;
                else if (k == "carry_pose") c.carry_pose = v != 0;
                else if (k == "carry_pose_test") c.carry_pose_test = std::clamp(int(v), 0, 2);
                else if (k == "carry_out") c.carry_out = v;
                else if (k == "carry_in") c.carry_in = v;
                else if (k == "carry_height") c.carry_height = std::clamp(v, 1.0f, 8.0f);
                else if (k == "skin_p1") c.skin_p[0] = std::clamp(int(v), -1, 14);
                else if (k == "skin_p2") c.skin_p[1] = std::clamp(int(v), -1, 14);
                else if (k == "world_cap") c.prop_cap = std::clamp(int(v), 50, 120);
                else if (k == "world_radius") c.prop_radius = std::clamp(v, 150.0f, 1600.0f);
                else if (k == "zoom_side_by_side") c.zoom_side = std::clamp(v, 0.4f, 2.5f);
                else if (k == "keyboard_player") c.keyboard_player = v >= 2 ? 2 : 1;
            }
            std::fclose(f);
        } else if (std::FILE* w = std::fopen(path, "w")) {
            std::fputs("# HULK co-op settings (restart the game after a change)\n"
                       "# pads count 1-4 in the order Windows connected them\n"
                       "p1_pad = 1\np2_pad = 2\n"
                       "# 0 = one full screen on P1\nsplit = 1\n"
                       "respawn_seconds = 9\n"
                       "# 1 = a player who falls far below his partner is put beside him\n"
                       "fall_rescue = 0\n"
                       "# which player the keyboard drives: 1 or 2 (F8 swaps it in game)\n"
                       "keyboard_player = 1\n",
                       w);
            std::fclose(w);
        }
        if (c.p2_pad == c.p1_pad) c.p2_pad = (c.p1_pad + 1) % 4;
        LOGI("coop: settings p1_pad %d p2_pad %d split %d respawn %d s fall rescue %d keyboard P%d", c.p1_pad + 1,
             c.p2_pad + 1, int(c.split), c.respawn_seconds, int(c.fall_rescue), c.keyboard_player);
        return c;
    }();
    return s;
}

// The settings the mod menu changes while playing (coop.ini gives their first values; the menu writes them back)
std::atomic<int> g_respawn_s{0};  // 0 until read
std::atomic<bool> g_fall_rescue{false}, g_split_vertical{false}, g_god[2] = {false, false}, g_menus_4_3{false},
    g_follow_warps{false}, g_friendly_fire{false}, g_show_smashable{true};
std::atomic<int> g_red_hulk{2};
std::atomic<int> g_skin{-2};  // -2 until read from coop.ini
std::atomic<int> g_pskin[2] = {-2, -2};  // hc50: each player's skin, -2 until read
std::atomic<int64_t> g_pskin_changed_ms{0};
std::atomic<uint32_t> g_p_object[2] = {0, 0};  // hc50: each player's Render::Object (seen by the 0x1C5460 hook)
int PlayerSkin(int who) {
    if (g_pskin[who].load() == -2) g_pskin[who] = Config().skin_p[who];
    return g_pskin[who].load();
}
int RespawnSeconds() {
    int v = g_respawn_s.load();
    if (!v) {
        v = Config().respawn_seconds;
        g_fall_rescue = Config().fall_rescue;
        g_split_vertical = Config().split_vertical;
        g_god[0] = Config().god[0];
        g_god[1] = Config().god[1];
        g_menus_4_3 = Config().menus_4_3;
        g_follow_warps = Config().follow_warps;
        g_friendly_fire = Config().friendly_fire;
        g_show_smashable = Config().show_smashable;
        g_red_hulk = Config().red_hulk_player;
        g_respawn_s = v;
    }
    return v;
}
bool SplitVertical() {
    RespawnSeconds();
    return g_split_vertical.load();
}
bool Menus43() {
    RespawnSeconds();
    return g_menus_4_3.load();
}
// hc47 SKINS: the Hulk model the level loads is chosen once per level/checkpoint load by the world stream manager
// (0x206320 -> 0x203A10: "hulk", or an unlocked cheat skin; 0x203890(index) copies the skin name into +0xAA4, and the
// stream then loads that character's files). One model set is loaded for both Hulks, so the skin is for both, and it
// changes at the next checkpoint / level load. Index: the game's own table at 0x203890.
const char* const kSkinNames[15] = {"Grey Hulk",   "Joe Fixit",   "Abomination", "Savage Banner", "Canada Hulk",
                                    "Australia Hulk", "US Hulk",  "UK Hulk",     "France Hulk",   "Germany Hulk",
                                    "Italy Hulk",  "Spain Hulk",  "Japan Hulk",
                                    // hc55: the same two packages on their own skeletons ("real model", see Remap)
                                    "Abomination (real model)", "Savage Banner (real model)"};
// hc55: per-player skins 13 / 14 load package 2 / 3 (and keep that model's own bone lengths)
int SkinFile(int skin) { return skin == 13 ? 2 : skin == 14 ? 3 : skin; }
// the per-player skin list in menu order: Normal, Grey, Joe Fixit, Abomination skin / real, Banner skin / real, countries
constexpr int kSkinOrder[16] = {-1, 0, 1, 2, 13, 3, 14, 4, 5, 6, 7, 8, 9, 10, 11, 12};
int Skin() {
    if (g_skin.load() == -2) g_skin = Config().skin;
    return g_skin.load();
}
bool ShowSmashable() {
    RespawnSeconds();
    return g_show_smashable.load();
}
bool FriendlyFire() {
    RespawnSeconds();
    return g_friendly_fire.load();
}
bool FollowWarps() {
    RespawnSeconds();
    return g_follow_warps.load();
}
std::atomic<bool> g_draw_p2_first{false};  // hc32 test switch (mod menu), not saved
bool DrawP2First() { return g_draw_p2_first.load(); }
int RedHulkPlayer() {  // 1 or 2, 0: neither
    RespawnSeconds();
    return g_red_hulk.load();
}
// hc50: the Red Hulk tint is drawn only while that player has no skin of his own picked (the skin shows instead)
int RedHulkDrawn() {
    const int who = RedHulkPlayer();
    return who && PlayerSkin(who - 1) >= 0 ? 0 : who;
}
bool God(int slot) {
    RespawnSeconds();
    return g_god[slot].load();
}
bool FallRescue() {
    RespawnSeconds();
    return g_fall_rescue.load();
}

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("HULK_COOP");
        return !(v && v[0] == '0');
    }();
    return on;
}

std::atomic<bool> g_menu_toggle{false};  // F1 (HulkCoopPoll) -> the mod menu
std::atomic<uint32_t> g_p2_controller{0}, g_p2_sim{0};
std::atomic<bool> g_p2_present{false};
std::atomic<uint32_t> g_p2_actor{0};
std::atomic<bool> g_camera2_pass{false};
std::atomic<bool> g_p2_marker_pass{false};  // hc30: P2's mission marker check: "the player's" controls are P2's
// downed players (index 0 = P1, 1 = P2), see "Downed and respawn" below
std::atomic<bool> g_downed[2] = {false, false};
// normal play: the chase camera has been the active camera for half a second (not a level load, cutscene or scripted
// shot). The downed and both-down rules only act then: a load resets and deletes actors through the health setter.
std::atomic<bool> g_in_play{false};
std::atomic<bool> g_p2_settled{false};  // P2 has been in the game a second (the extra hooks leave a new P2 alone)
void DownedTick(const hulk::cheats::CheatState& state, X86Context& ctx, uint32_t p1, uint32_t p2);
void OnNewP2(X86Context& ctx, uint32_t p1, uint32_t p2, bool respawn);
void PlaceBeside(X86Context& ctx, int who_slot, uint32_t who, uint32_t partner, const char* why);  // hc97
extern std::atomic<bool> g_p1_dead;  // hc97
void ClearP2Down();  // P2's actor is gone: his downed wait and a pending both-down end with him  // a new P2 actor: his downed state ends
// for the overlay (UI thread): P2's power, max and critical mass threshold; seconds until each downed player is back
std::atomic<float> g_ui_power{0}, g_ui_max{0}, g_ui_threshold{0}, g_ui_back[2] = {0, 0};
std::atomic<bool> g_ui_split{false};  // P2's camera is updating: the sim controller it asks for (index 0) is P2's

float F32(uint32_t va) { return std::bit_cast<float>(R32(va)); }

uint32_t Call(X86Context& ctx, uint32_t ecx, uint32_t fn, std::initializer_list<uint32_t> args) {
    X86Context c = ctx;
    c.ecx = ecx;
    return kernel::CallGuest(c, fn, args);
}
uint32_t GuestNew(X86Context& ctx, uint32_t size) {
    const uint32_t p = kernel::CallGuest(ctx, kGuestNew, {size});
    if (p) std::memset(kernel::HostPtr(p), 0, size);
    return p;
}
// hc30: the game's operator new takes from its "current heap" ([0x527F60], an index into the heap table 0x527C60,
// 16 bytes each; SetCurrentHeap 0x266DA0 (cdecl index) returns the old one). The mod's own objects (P2's controller,
// sim controller, chase camera, strings) were made in whatever heap was current then, often a per-level one: they
// are made in the Persistent Heap (17) now, so a level ending or loading never frees or checks them.
constexpr uint32_t kSetCurrentHeap = 0x266DA0, kHeapTable = 0x527C60, kPersistentHeap = 17;
struct PersistentHeap {
    X86Context& ctx;
    uint32_t was = 0xFFFFFFFF;
    explicit PersistentHeap(X86Context& c) : ctx(c) {
        if (R32(kHeapTable + kPersistentHeap * 16)) was = kernel::CallGuest(ctx, kSetCurrentHeap, {kPersistentHeap});
    }
    ~PersistentHeap() {
        if (was != 0xFFFFFFFF) kernel::CallGuest(ctx, kSetCurrentHeap, {was});
    }
};
// hc34: P2's controller's own allocations (its 4 listeners and their button tables, which the controller code always
// asks of the Controller Heap, 4) are sent to the Persistent Heap while this is above 0, so the Controller Heap keeps
// the room the game sized it for (it ran out when the game reset the pads at a level's end: the quit freeze/crash).
std::atomic<int> g_p2_ctrl_alloc{0};
struct P2ControllerAllocs {
    P2ControllerAllocs() { g_p2_ctrl_alloc++; }
    ~P2ControllerAllocs() { g_p2_ctrl_alloc--; }
};
uint32_t PersistentNew(X86Context& ctx, uint32_t size) {
    PersistentHeap heap(ctx);
    return GuestNew(ctx, size);
}
void AddRef(X86Context& ctx, uint32_t obj) { Call(ctx, obj, R32(R32(obj)), {}); }

// P2's controller: a CController whose only listener is port 2's
std::atomic<uint32_t> g_p2_own_listener0{0};
uint32_t MakeP2Controller(X86Context& ctx, uint32_t p1) {
    PersistentHeap heap(ctx);  // the controller and the listeners its constructor makes
    P2ControllerAllocs own;    // (their Controller Heap requests go to the Persistent Heap too)
    const uint32_t c = GuestNew(ctx, 0x174);
    if (!c) return 0;
    Call(ctx, c, kControllerCtor, {0});
    AddRef(ctx, c);
    const uint32_t p1_first = R32(R32(p1 + kCtrlListeners));
    const uint32_t scheme = p1_first ? R32(p1_first + kListenerScheme) : 1;
    Call(ctx, c, kControllerScheme, {scheme ? scheme : 1});
    const uint32_t list = R32(c + kCtrlListeners);
    if (R32(c + kCtrlCount) < 2 || !list) {
        LOGE("coop: P2 controller has %u listeners", R32(c + kCtrlCount));
        return 0;
    }
    const int port = Config().p2_pad;
    W32(list, R32(list + 4 * port));  // P2's pad's listener first,
    g_p2_own_listener0 = R32(list);   // (hc36: put back in slot 0 whenever P1's listener can't be used)
    W32(c + kCtrlCount, 1);           // and the only one merged
    LOGI("coop: P2 controller %08X (pad %d, control scheme %u)", c, port + 1, scheme);
    return c;
}

uint32_t ActorManager() {
    const uint32_t app = R32(kApp);
    return app ? R32(app + 0x20) : 0;
}

// P2's actor, found by name among the actor manager's actors
uint32_t FindP2(X86Context& ctx, uint32_t mgr) {
    // the handles are stored in the manager itself from +0x10 (0x24970: lea edi, [esi+0x10])
    const uint32_t n = R32(mgr + kActorCount), handles = mgr + kActorHandles, owner = R32(mgr);
    if (!owner || n > 4096) return 0;
    const uint32_t table = R32(owner + 0x44);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = Call(ctx, table, kHandleToActor, {R32(handles + 4 * i)});
        if (a && std::strncmp(static_cast<const char*>(kernel::HostPtr(a + kName)), kP2Name, sizeof(kP2Name)) == 0)
            return a;
    }
    return 0;
}

bool Position(uint32_t actor, float out[3]) {
    const uint32_t body = R32(actor + 0x10);
    const uint32_t xf = body ? R32(body + 0x18) : 0;
    if (!xf) return false;
    for (int i = 0; i < 3; i++) out[i] = F32(xf + 0x40 + 4 * i);
    return true;
}

// guest copies of the strings the spawn needs
uint32_t g_strings = 0;
uint32_t Strings(X86Context& ctx) {
    if (!g_strings) {
        g_strings = PersistentNew(ctx, 64);
        if (g_strings) {  // +0 "", +16 the name, +32 "none" (no AI path)
            std::memcpy(kernel::HostPtr(g_strings + 16), kP2Name, sizeof(kP2Name));
            std::memcpy(kernel::HostPtr(g_strings + 32), "none", 5);
        }
    }
    return g_strings;
}

void Spawn(X86Context& ctx, uint32_t mgr, uint32_t p1) {
    const uint32_t tmpl = R32(p1 + kTemplate);
    float pos[3];
    if (!tmpl || !Position(p1, pos) || !Strings(ctx)) return;
    const char* tname = static_cast<const char*>(kernel::HostPtr(tmpl + kTemplateName));
    pos[0] += 3.0f;  // beside P1 (world units are about metres)
    pos[1] += 2.0f;  // hc97: 2 m up (was 1): 3 m to the side on a hillside can be under the ground; he drops onto it
    const uint32_t ok = Call(ctx, mgr, kCreateActorScript,
                             {tmpl + kTemplateName, std::bit_cast<uint32_t>(pos[0]), std::bit_cast<uint32_t>(pos[1]),
                              std::bit_cast<uint32_t>(pos[2]), 0, g_strings, g_strings + 32, 0, g_strings + 16});
    LOGI("coop: spawn P2 from template '%.32s' at %.0f %.0f %.0f -> %u", tname, pos[0], pos[1], pos[2], ok & 0xFF);
}

void Setup(X86Context& ctx, uint32_t p1, uint32_t p2) {
    W32(p2 + kControllerIndex, 1);
    const uint32_t b1 = R32(p1 + kBrain), b2 = R32(p2 + kBrain);
    if (b1 && b2) kernel::W64(b2 + kTeam, kernel::R64(b1 + kTeam));  // P1's team
    const float max = F32(p1 + kMaxHealth);
    if (max > 0) Call(ctx, p2, kSetHealth, {std::bit_cast<uint32_t>(max), 1});
    LOGI("coop: P2 sound emitter %08X (P1's %08X)", R32(p2 + 0x128), R32(p1 + 0x128));
    LOGI("coop: P2 %08X ready: controller index 1, team copied, health %.0f; pads connected: %d %d %d %d", p2,
         F32(p2 + kHealth), int(input::Poll(0).connected), int(input::Poll(1).connected), int(input::Poll(2).connected),
         int(input::Poll(3).connected));
}

// once per world update, after it (cheats.h appliers run there)
std::atomic<int64_t> g_last_tick_ms{0};  // the world's last update (steady clock): menus and popups stop it
// P2 as the hooks outside the world update may use him: the world has run in the last half second (not a level being
// torn down for the main menu or a load) and the actor still carries P2's name
uint32_t P2Live() {
    const uint32_t p2 = g_p2_actor.load();
    if (!p2 || !g_p2_present.load() || g_unloading.load()) return 0;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - g_last_tick_ms.load() > 500 || !g_p2_settled.load()) return 0;
    if (std::memcmp(kernel::HostPtr(p2 + kName), kP2Name, sizeof(kP2Name)) != 0) return 0;
    return p2;
}
void Tick(const hulk::cheats::CheatState& state, X86Context& ctx, uint8_t*) {
    if (!Enabled()) return;
    Crumb crumb_("Tick");
    static std::atomic<bool> watchdog{false};
    if (!watchdog.exchange(true))
        std::thread([] {
            bool stalled = false;
            int64_t last_flush = CrumbNowMs();
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                const int64_t now = CrumbNowMs(), quiet = now - g_beat_ms.load();
                if (quiet > 3000 && !stalled) {
                    stalled = true;
                    LOGI("coop: watchdog: no co-op hook for %lld ms; last one '%s', world tick %lld ms ago, last frame "
                         "%lld ms ago, unloading %d, P2 %08X",
                         (long long)quiet, g_crumb.load(), (long long)(now - g_last_tick_ms.load()),
                         (long long)(now - g_render_ms.load()), int(g_unloading.load()), g_p2_actor.load());
                    // where the game thread is: the return addresses on its guest stack, three looks 0.7 s apart
                    // (the same each time: it waits or loops there)
                    for (int look = 0; look < 3; look++) {
                        if (X86Context* c = g_main_ctx.load()) {
                            const uint32_t esp = c->esp;
                            char line[1024];
                            int n = std::snprintf(line, sizeof(line), "coop: watchdog stack %d: esp %08X last icall %08X:",
                                                  look, esp, c->last_icall);
                            int found = 0;
                            for (uint32_t a = esp & ~3u; a < esp + 0x3000 && found < 40; a += 4) {
                                if (!mem::IsCommitted(a)) break;
                                const uint32_t v = R32(a);
                                if (v < 0x11000 || v >= 0x3F2BE0 || !mem::IsCommitted(v - 6)) continue;
                                // a return address follows a call: e8 rel32, ff /2 (2, 3 or 6 bytes)
                                const bool call = kernel::G<uint8_t>(v - 5) == 0xE8 || kernel::G<uint8_t>(v - 2) == 0xFF ||
                                                  kernel::G<uint8_t>(v - 3) == 0xFF || kernel::G<uint8_t>(v - 6) == 0xFF;
                                if (!call) continue;
                                found++;
                                if (n < int(sizeof(line)) - 12) n += std::snprintf(line + n, sizeof(line) - n, " %X", v);
                            }
                            LOGI("%s", line);
                        }
                        logx::Flush();
                        std::this_thread::sleep_for(std::chrono::milliseconds(700));
                    }
                    logx::Flush();
                } else if (quiet < 1000 && stalled) {
                    stalled = false;
                    LOGI("coop: watchdog: hooks running again");
                }
                if (now - last_flush > 5000) last_flush = now, logx::Flush();
            }
        }).detach();
    g_main_ctx = &kernel::CurrentContext();
    g_last_tick_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
    static uint32_t last_p1 = 0, p2 = 0, seen_frames = 0, missing_frames = 0;
    static int gen_seen = 0;
    if (gen_seen != g_unload_gen.load()) {  // a level ended since the last tick: start over (P1 may reuse an address)
        gen_seen = g_unload_gen.load();
        last_p1 = 0, p2 = 0, seen_frames = 0, missing_frames = 0;
    }
    if (g_unloading.exchange(false)) LOGI("coop: the world runs again after the level ended");
    const uint32_t mgr = ActorManager();
    const uint32_t p1 = mgr ? R32(mgr + kMainActor) : 0;
    if (!p1 || !g_p2_sim.load()) {
        g_p2_present = false;
        g_p2_actor = 0;
        last_p1 = p1, p2 = 0, seen_frames = 0;
        return;
    }
    if (p1 != last_p1) {  // a new level or a new main character
        last_p1 = p1, p2 = 0, seen_frames = 0, missing_frames = 0;
        g_downed[0] = false, g_downed[1] = false;
        LOGI("coop: P1 %08X ('%.32s')", p1, static_cast<const char*>(kernel::HostPtr(p1 + kName)));
    }
    if (++seen_frames < 180) return;  // let the level settle (3 s)
    if (seen_frames % 30 == 0 || !p2) {
        static bool had_p2 = false;  // a P2 before on this level: a new one is a re-spawn (he fell out of the map)
        if (seen_frames == 180) had_p2 = false;
        const uint32_t found = FindP2(ctx, mgr);
        if (found && found != p2) {
            Setup(ctx, p1, found);
            OnNewP2(ctx, p1, found, had_p2);
            had_p2 = true;
        }
        p2 = found;
    }
    g_p2_present = p2 != 0;
    g_p2_actor = p2;
    static uint32_t settled_p2 = 0;
    static int p2_age = 0;
    if (p2 != settled_p2) settled_p2 = p2, p2_age = 0;
    g_p2_settled = p2 && ++p2_age > 60;
    if (p2) {
        missing_frames = 0;
        if (R32(p2 + kControllerIndex) != 1) W32(p2 + kControllerIndex, 1);
        // the actor manager removes distant actors that are not the main one (0x2A960: farther than about 238 m from
        // P1 or in a part of the city that is not loaded); +0x1CF5 set keeps an actor out of that (hc23 lost P2 so)
        if (!kernel::G<uint8_t>(p2 + 0x1CF5)) kernel::G<uint8_t>(p2 + 0x1CF5) = 1;
        // the actor manager's update (0x25FF0) gives the main actor a full update every frame and any other actor a
        // full one only with +0x1D04 (or two other flags) set, else less often the farther he is from the camera:
        // P2 looked choppy at times. +0x1D04 is read only there and by its getter 0x23B30.
        if (!kernel::G<uint8_t>(p2 + 0x1D04)) kernel::G<uint8_t>(p2 + 0x1D04) = 1;
        // hc31 upgrades: both Hulks are made from the 'hulk' template, and the game's health reset (0x401A0) adds the
        // bought Health / Critical Mass upgrades (template +0x138..+0x14C, unlock flags 0x92940) to whichever Hulk it
        // runs for. Moves are unlocked game-wide. A purchase in the middle of a level only redoes P1, so P2 takes P1's
        // limits: +0x1B10 the top of the bar (health + critical mass), +0x1B14 where critical mass starts.
        for (const uint32_t o : {0x1B10u, 0x1B14u})
            if (R32(p2 + o) != R32(p1 + o) && F32(p1 + o) > 0) {
                static int logged = 0;
                if (logged++ < 10)
                    LOGI("coop: P2 takes P1's upgrades: +%X %.0f -> %.0f", o, F32(p2 + o), F32(p1 + o));
                W32(p2 + o, R32(p1 + o));
            }
        // hc97: a new P2 who ends up more than 8 m below P1 in his first 4 s was spawned under the ground (or P1 was
        // moved right after the spawn): put him beside P1 at once, before he falls out of the map and gets downed
        static uint32_t rescued_p2 = 0;
        if (p2_age <= 240 && rescued_p2 != p2 && !g_downed[1].load() && !g_downed[0].load() && !g_p1_dead.load()) {
            float a1[3], a2[3];
            if (Position(p1, a1) && Position(p2, a2) && a1[1] - a2[1] > 8.0f) {
                rescued_p2 = p2;
                LOGI("coop: new P2 is %.1f m below P1 (spawned under the ground?): placing him beside P1", a1[1] - a2[1]);
                PlaceBeside(ctx, 1, p2, p1, "spawned below P1");
            }
        }
        DownedTick(state, ctx, p1, p2);
        return;
    }
    g_downed[1] = false;
    ClearP2Down();
    // no P2: spawn one once P1 stands steady, and again 4 s after a try that gave none.
    // hc97: hulk_097 spawned P2 at P1's position of that moment (y -14) while P1 was still being moved (1.5 s later he
    // stood at y 11.5 on the hill) -> P2 under the ground, fell, held at 1 hp, re-created. P1 counts as steady after
    // 45 ticks with no jump over 1 m in a tick and under 3 m of height change; after 15 s P2 is spawned anyway.
    static float steady_pos[3], steady_y0 = 0;
    static int steady = 0;
    static uint32_t next_try = 0;
    if (missing_frames == 0) steady = 0, next_try = 0;
    float now1[3];
    if (Position(p1, now1)) {
        const float dx = now1[0] - steady_pos[0], dy = now1[1] - steady_pos[1], dz = now1[2] - steady_pos[2];
        if (steady == 0 || dx * dx + dy * dy + dz * dz > 1.0f || std::fabs(now1[1] - steady_y0) > 3.0f) {
            steady = 1;
            steady_y0 = now1[1];
        } else steady++;
        std::memcpy(steady_pos, now1, sizeof(steady_pos));
    } else steady = 0;
    const uint32_t frame = missing_frames++;
    if (frame >= next_try && (steady >= 45 || frame >= 900)) {
        if (steady < 45) LOGI("coop: P1 not steady after 15 s, spawning P2 anyway");
        Spawn(ctx, mgr, p1);
        next_try = frame + 240;
    }
}
const bool g_registered = Enabled() && hulk::cheats::RegisterApplier("coop", &Tick);
}  // namespace

// CControllerManager::Update (thiscall): the pads are polled here. P2's controller gets the same steps.
X86_EXTERN_FUNC(__imp__sub_000AE840);
X86_FUNC(sub_000AE840) { Crumb crumb_("sub_000AE840");
    const uint32_t mgr = ctx.ecx;
    const uint32_t p1 = R32(mgr + 4);
    uint32_t p2 = g_p2_controller.load();
    if (Enabled() && p1 && !p2) {
        p2 = MakeP2Controller(ctx, p1);
        g_p2_controller = p2;
    }
    // hc36 THE QUIT CRASH: at a level's end SetActiveController (0xAE2F0) rebuilds P1's listeners (0xAE090: the old ones
    // and the list are freed, new ones made). P2's controller still had the old "Port<n>\Slot0" listener in slot 0 and
    // was pre-stepped/merged through it on the next frame: a freed listener (write to 0x4C6, hulk_039). Now P2 uses P1's
    // listener only while he plays and the level is not ending; otherwise his own listener is put back in slot 0 first.
    const bool live = g_p2_present.load() && !g_unloading.load();
    if (p1 && R32(p1 + kCtrlListeners)) {  // while P2 plays, P1 is his pad only; otherwise every pad drives P1
        static bool swapped = false;
        static uint32_t swapped_list = 0, full_count = 0;
        const uint32_t list = R32(p1 + kCtrlListeners);
        const int port = Config().p1_pad;
        if (swapped && list != swapped_list) {  // the game made a new list (in its own order): nothing to swap back
            swapped = false;
            LOGI("coop: P1's listeners were rebuilt by the game (list %08X -> %08X, %u)", swapped_list, list,
                 R32(p1 + kCtrlCount));
        }
        if (R32(p1 + kCtrlCount) != 1 || !full_count) full_count = R32(p1 + kCtrlCount);  // the game's own count (pads it made)
        const bool restrict = live && port > 0 && port < int(full_count);
        if (restrict != swapped) {  // P1's pad's listener in slot 0 while restricted, back after
            const uint32_t a = R32(list), b = R32(list + 4 * port);
            W32(list, b);
            W32(list + 4 * port, a);
            swapped = restrict;
            swapped_list = list;
        }
        W32(p1 + kCtrlCount, live ? 1 : full_count);
        // The game keeps one "active" pad (SetActiveController 0xAE2F0: listener +0xD = 1 for every other pad, and
        // Merge 0xAE130 files those under a bucket nothing reads). At level load it makes the LAST connected pad
        // active, so with two pads P1's pad (and the keyboard on it) went dead. Every pad stays active here: P1's
        // pad drives P1, P2's pad drives P2's controller.
        if (Enabled())
            for (uint32_t i = 0; i < full_count && i < 4; i++)
                if (const uint32_t l = R32(list + 4 * i)) kernel::G<uint8_t>(l + 0xD) = 0;
    }
    const uint32_t p2_list = p2 ? R32(p2 + kCtrlListeners) : 0;
    if (p2_list && !live) {  // no P2 playing, or the level is ending: P2's own listener back in slot 0
        const uint32_t own = g_p2_own_listener0.load();
        if (own && R32(p2_list) != own) {
            W32(p2_list, own);
            LOGI("coop: P2 controller back on its own listener %08X", own);
        }
    }
    // P2's controller reads the pad through P1's controller's listener for P2's port (named "Port<n>\Slot0", bound to
    // the device by the game): a new controller's own listeners are never named, so they read nothing. P1 only merges
    // his own slot while P2 plays, so the shared listener is pre-stepped and merged once, by P2's controller. It is
    // looked up by name every frame, so a list the game rebuilt is followed at once.
    if (p2_list && p1 && Enabled() && live && R32(p1 + kCtrlListeners)) {
        char want[16];
        std::snprintf(want, sizeof(want), "Port%d\\Slot0", Config().p2_pad);
        const uint32_t p1_list = R32(p1 + kCtrlListeners);
        uint32_t found = 0;
        for (uint32_t i = 0; i < 4 && !found; i++) {
            const uint32_t l = R32(p1_list + 4 * i);
            if (!l) continue;
            bool same = true;
            for (int j = 0; j < 16 && same; j++) {
                const char ch = char(kernel::G<uint8_t>(l + 0x14 + j));
                same = ch == want[j];
                if (!ch) break;
            }
            if (same) found = l;
        }
        const uint32_t use = found ? found : g_p2_own_listener0.load();
        if (use && R32(p2_list) != use) {
            W32(p2_list, use);
            LOGI("coop: P2 controller reads listener %08X '%s'", use, found ? want : "(own)");
        }
    }
    if (p2_list && Enabled())  // P2's own listeners: the same "inactive" mark
        for (int i = 0; i < 4; i++)
            if (const uint32_t l = R32(p2_list + 4 * i)) kernel::G<uint8_t>(l + 0xD) = 0;
    const bool step_p2 = p2_list && R32(p2_list) && !g_unloading.load();
    if (step_p2) {
        P2ControllerAllocs own;
        Call(ctx, p2, kControllerPre, {});
    }
    __imp__sub_000AE840(ctx, base);
    if (step_p2 && R32(p2_list)) {
        P2ControllerAllocs own;
        Call(ctx, p2, kControllerMerge, {});
    }
}

// CControllerManager::GetController(index) (thiscall, ret 4): the game only has index 0
X86_EXTERN_FUNC(__imp__sub_000AE640);
X86_FUNC(sub_000AE640) { Crumb crumb_("sub_000AE640");
    if (X86_MEM32(ctx.esp + 4) == 1 && g_p2_controller.load()) {
        ctx.eax = g_p2_controller.load();
        ctx.esp += 8;
        return;
    }
    __imp__sub_000AE640(ctx, base);
}

// hc39 THE CHECKPOINT CRASH (hulk_043, church checkpoint: read of guest 0x4C6 = 0x4B9 + 0xD in 0xAF8E0). The pad
// rumble object (made once at boot by 0xB0500 at flow +0x18; its part 0xAFBF0 at +8: +0xC P1's controller, +0x10 its
// own count of P1's listeners, copied ONCE at boot, +0x14 one 0x30-byte record per listener) walks P1's listener list
// every frame (0xB0080 -> 0xAF8E0 / 0xAFAF0) up to that boot count. When the game rebuilds P1's list (0xAE090: the old
// listeners and list freed, a new list of as many listeners as the input system has pads now, here 3) the rumble
// object still walks 4: list[3] is past the end of the new list, garbage, and the game crashes on whatever is there.
// The rebuild came from the level load: SetActiveController (0xAE2F0) looks for the active pad's listener only among
// the first <count> listeners, and this mod holds P1's count at 1 while P2 plays, so the search missed and the game
// rebuilt the list at every load. Now:
//  1. the real length of every controller's list is remembered when the game builds it (0xAE090),
//  2. SetActiveController searches the whole list (as in the plain game), so a load no longer rebuilds it,
//  3. the rumble object never walks more listeners than the list really has (0xB0080, before the walk).
namespace {
struct ListLen {
    uint32_t ctrl, len;
};
ListLen g_list_len[8];
int g_list_len_n = 0;
uint32_t KnownListLen(uint32_t c) {
    for (int i = 0; i < g_list_len_n; i++)
        if (g_list_len[i].ctrl == c) return g_list_len[i].len;
    return 0;
}
void SetListLen(uint32_t c, uint32_t n) {
    for (int i = 0; i < g_list_len_n; i++)
        if (g_list_len[i].ctrl == c) {
            g_list_len[i].len = n;
            return;
        }
    if (g_list_len_n < 8) g_list_len[g_list_len_n++] = {c, n};
}
}  // namespace

// CController: rebuild the listener list (thiscall, ret 4: the new count)
X86_EXTERN_FUNC(__imp__sub_000AE090);
X86_FUNC(sub_000AE090) { Crumb crumb_("sub_000AE090 listeners");
    const uint32_t c = ctx.ecx;
    const uint32_t was = c ? KnownListLen(c) : 0;
    __imp__sub_000AE090(ctx, base);
    if (!c) return;
    const uint32_t n = R32(c + kCtrlCount);
    SetListLen(c, n);
    LOGI("coop: controller %08X listeners built: %u (list %08X; was %u)%s", c, n, R32(c + kCtrlListeners), was,
         c == g_p2_controller.load() ? " P2's" : "");
}

// CController::SetActiveController(name) (thiscall, ret 4): searched over the whole list
X86_EXTERN_FUNC(__imp__sub_000AE2F0);
X86_FUNC(sub_000AE2F0) { Crumb crumb_("sub_000AE2F0 active pad");
    const uint32_t c = ctx.ecx;
    const uint32_t len = c ? KnownListLen(c) : 0, count = c ? R32(c + kCtrlCount) : 0;
    const uint32_t list = c ? R32(c + kCtrlListeners) : 0;
    const bool widen = list && len > count && c != g_p2_controller.load();
    if (widen) W32(c + kCtrlCount, len);
    __imp__sub_000AE2F0(ctx, base);
    if (widen && R32(c + kCtrlListeners) == list) W32(c + kCtrlCount, count);  // not rebuilt: the mod's count back
    static int logs = 0;
    if (widen && logs < 20) {
        logs++;
        LOGI("coop: active pad search over %u listeners (count was %u)%s", len, count,
             R32(c + kCtrlListeners) == list ? "" : ", the game rebuilt the list anyway");
    }
}

// pad rumble update (thiscall, ret 4): its walk over P1's listeners kept inside the real list
X86_EXTERN_FUNC(__imp__sub_000B0080);
X86_FUNC(sub_000B0080) { Crumb crumb_("sub_000B0080 rumble");
    const uint32_t obj = ctx.ecx;
    if (const uint32_t part = obj ? R32(obj + 8) : 0)
        if (const uint32_t c = R32(part + 0xC)) {
            static uint32_t s_part = 0, s_cap = 0;  // the count the part was made with = its records
            if (part != s_part) {
                s_part = part;
                s_cap = R32(part + 0x10);
            }
            const uint32_t len = KnownListLen(c);
            if (len) {
                const uint32_t want = std::min(s_cap, len);
                if (R32(part + 0x10) != want) {
                    LOGI("coop: rumble walks %u of P1's listeners (was %u; list has %u, made for %u)", want,
                         R32(part + 0x10), len, s_cap);
                    W32(part + 0x10, want);
                }
            }
        }
    __imp__sub_000B0080(ctx, base);
}

// CSimControllerManager::Update(dt) (thiscall, ret 4): P2's sim controller follows P1's (on, or cleared).
// The update proper (0xB3BC0) is only ever reached by this function's tail jump, so it is no recompiled function of
// its own: P2's is run through this same function on a stand-in manager (+4 P2's sim controller, +8 the on flag).
uint32_t g_p2_sim_mgr = 0;
X86_EXTERN_FUNC(__imp__sub_000B3D80);
X86_FUNC(sub_000B3D80) { Crumb crumb_("sub_000B3D80");
    const uint32_t mgr = ctx.ecx, dt = X86_MEM32(ctx.esp + 4);
    if (mgr && mgr == g_p2_sim_mgr) {  // P2's own pass, below
        __imp__sub_000B3D80(ctx, base);
        return;
    }
    __imp__sub_000B3D80(ctx, base);
    if (!Enabled()) return;
    uint32_t sim = g_p2_sim.load();
    const uint32_t c = g_p2_controller.load();
    if (!sim && c) {
        PersistentHeap heap(ctx);
        sim = GuestNew(ctx, 0x224);
        const uint32_t stand_in = sim ? GuestNew(ctx, 16) : 0;
        if (sim && stand_in) {
            Call(ctx, sim, kSimCtor, {c});
            AddRef(ctx, sim);
            W32(stand_in + 4, sim);
            g_p2_sim_mgr = stand_in;
            g_p2_sim = sim;
            LOGI("coop: P2 sim controller %08X", sim);
        } else {
            sim = 0;
        }
    }
    if (g_downed[0].load() && R32(mgr + 4)) Call(ctx, R32(mgr + 4), kSimClear, {1});  // a downed P1 has no control
    if (!sim || !g_p2_sim_mgr) return;
    // on while the game's own input is on and P2 is up; off clears his input
    kernel::G<uint8_t>(g_p2_sim_mgr + 8) = (kernel::G<uint8_t>(mgr + 8) && !g_downed[1].load()) ? 1 : 0;
    Call(ctx, g_p2_sim_mgr, 0xB3D80, {dt});
}

// CSimControllerManager::Get(index) (thiscall, ret 4): a character with controller index 1 reads P2's
X86_EXTERN_FUNC(__imp__sub_000B3D20);
X86_FUNC(sub_000B3D20) { Crumb crumb_("sub_000B3D20");
    const uint32_t index = X86_MEM32(ctx.esp + 4);
    if ((index == 1 || (index == 0 && (g_camera2_pass.load() || g_p2_marker_pass.load()))) && g_p2_sim.load()) {
        ctx.eax = g_p2_sim.load();
        ctx.esp += 8;
        return;
    }
    __imp__sub_000B3D20(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------
// Split screen (hc02): P1 top, P2 bottom. P2 has his own chase camera, updated right after the camera manager's,
// with P2's right stick. The render manager draws the frame twice, once per camera, each in its half of the screen
// (d3d8.cpp SetSplitScreenHalf); the aspect doubles for a half-height view. Cutscenes and script cameras (any camera
// that is not the chase camera) stay full screen on P1.
namespace d3d {
void SetSplitScreenHalf(int half);
void SetSplitScreenVertical(bool vertical);
void SetTint(float tint);
}
namespace coop_render {
void BlackHalf(X86Context& ctx);
}
#define coop_render_BlackHalf coop_render::BlackHalf

namespace {
constexpr uint32_t kCameraNew = 0x263EA0;        // the camera manager's allocator (cdecl size)
constexpr uint32_t kChaseCtor = 0xA2430;         // CChaseCamera() thiscall, 0x4C0 bytes
constexpr uint32_t kChaseSetTarget = 0xA74C0;    // thiscall (app, actor name)
constexpr uint32_t kChaseTarget = 0x350;         // the actor it tracks (+0x345: it is the main actor)
constexpr uint32_t kCamActive = 0x10, kCamChase = 0xB8;  // camera manager: active camera, the chase camera
constexpr uint32_t kVtInit = 0x70, kVtUpdate = 0x8C, kVtMatrix = 0x38, kVtFovAspect = 0x18;
// render manager: camera matrix +0x1C (16 floats), fov +0x5C, aspect +0x60, near +0x64, far +0x68, dirty +0x6C
constexpr uint32_t kRmView = 0x1C, kRmViewBytes = 0x50, kRmFov = 0x5C, kRmAspect = 0x60, kRmClean = 0x6C;

uint32_t g_camera2 = 0, g_camera2_target = 0, g_scratch = 0;
std::atomic<bool> g_split_ready{false}, g_split_broken{false};
float g_view2[16], g_fov2 = 0, g_aspect2 = 0;

// a file named coop_nosplit.txt beside the game keeps one full screen on P1 (for testing P2 alone)
bool SplitAllowed() {
    static const bool allowed = Config().split && !std::filesystem::exists("coop_nosplit.txt");
    return allowed;
}
bool SplitWanted() { return Enabled() && SplitAllowed() && g_p2_present.load() && !g_split_broken.load(); }

// copies the tuning the scripts gave P1's chase camera (distance, heights) to P2's
void CopyTuning(uint32_t from, uint32_t to) {
    for (uint32_t o : {0x3C4u, 0x3C8u, 0x3CCu, 0x440u, 0x444u, 0x44Cu, 0x498u, 0x49Cu}) W32(to + o, R32(from + o));
}
}  // namespace

// CCameraManager::Update(dt, x) (thiscall, ret 8)
X86_EXTERN_FUNC(__imp__sub_0009BB50);
X86_FUNC(sub_0009BB50) { Crumb crumb_("sub_0009BB50");
    const uint32_t mgr = ctx.ecx, dt = X86_MEM32(ctx.esp + 4), arg2 = X86_MEM32(ctx.esp + 8);
    __imp__sub_0009BB50(ctx, base);
    const uint32_t p2 = g_p2_actor.load();
    const uint32_t chase = R32(mgr + kCamChase);
    // cutscenes and scripted shots are P1's full screen; the chase camera has to be the active one for a quarter of a
    // second before split screen comes back (a cutscene hands it back for single frames, which flashed the split)
    static int chase_frames = 0, play_frames = 0;
    const bool chase_active = chase && R32(mgr + kCamActive) == chase;
    play_frames = chase_active ? play_frames + 1 : 0;
    g_in_play = play_frames >= 30;
    if (!SplitWanted() || !p2 || !chase_active) {
        g_split_ready = false;
        chase_frames = 0;
        return;
    }
    const bool settled = ++chase_frames >= 15;
    if (!g_scratch) g_scratch = PersistentNew(ctx, 64);
    if (!g_camera2) {
        PersistentHeap heap(ctx);  // the camera and whatever its constructor and init make
        const uint32_t cam = kernel::CallGuest(ctx, kCameraNew, {0x4C0});
        if (!cam || !g_scratch) return;
        Call(ctx, cam, kChaseCtor, {});
        AddRef(ctx, cam);
        Call(ctx, cam, R32(R32(cam) + kVtInit), {});
        g_camera2 = cam;
        LOGI("coop: P2 chase camera %08X", cam);
    }
    if (g_camera2_target != p2) {
        CopyTuning(chase, g_camera2);
        std::memcpy(kernel::HostPtr(g_scratch + 16), kP2Name, sizeof(kP2Name));
        Call(ctx, g_camera2, kChaseSetTarget, {R32(kApp), g_scratch + 16});
        g_camera2_target = p2;
        LOGI("coop: P2 chase camera follows %08X (name lookup gave %08X)", p2, R32(g_camera2 + kChaseTarget));
    }
    // SetTarget looks the actor up by name and falls back to the main actor (P1) when that fails: the target the
    // camera tracks (+0x350, ref-counted: vtable +0 add, +4 release) is set to P2 directly whenever it is not him
    if (const uint32_t had = R32(g_camera2 + kChaseTarget); had != p2) {
        Call(ctx, p2, R32(R32(p2)), {});
        if (had) Call(ctx, had, R32(R32(had) + 4), {});
        W32(g_camera2 + kChaseTarget, p2);
        kernel::G<uint8_t>(g_camera2 + 0x345) = 0;  // "the target is the main actor"
        Call(ctx, R32(p2 + 0x10), 0x59A50, {g_camera2 + 0x1B8});  // as SetTarget does: the camera's hook on the body
        kernel::G<uint8_t>(g_camera2 + 0x344) = 0;
        LOGI("coop: P2 chase camera target was %08X, now P2 %08X", had, p2);
    }
    // the camera manager clears every camera it owns of "frozen" (+0xF1) at the start of each update (0x98E60);
    // a new camera starts frozen and the chase update (0xA75D0) does nothing while it is
    kernel::G<uint8_t>(g_camera2 + 0xF1) = 0;
    g_camera2_pass = true;
    Call(ctx, g_camera2, R32(R32(g_camera2) + kVtUpdate), {dt, arg2, 1, 1});
    g_camera2_pass = false;
    const uint32_t m = Call(ctx, g_camera2, R32(R32(g_camera2) + kVtMatrix), {});
    Call(ctx, g_camera2, R32(R32(g_camera2) + kVtFovAspect), {g_scratch, g_scratch + 4});
    if (!m) return;
    std::memcpy(g_view2, kernel::HostPtr(m), sizeof(g_view2));
    // hc43 STUCK CAMERA: after climbing a tall building P2's camera sometimes stayed below him looking up at the sky
    // (fixed only by a big jump or R3). If his camera is more than 4 m below him for 2 s, it is set up again (init,
    // then target and tuning again next frame), as when it was made. Logged, so the next log shows the moment.
    {
        static int64_t below_since = 0;
        float pp[3];
        const float cam_y = g_view2[13];
        const bool below = settled && Position(p2, pp) && std::isfinite(cam_y) && cam_y < pp[1] - 4.0f;
        const int64_t now = CrumbNowMs();
        if (!below) below_since = 0;
        else if (!below_since) below_since = now;
        else if (now - below_since > 2000) {
            LOGI("coop: P2's camera stuck %.1f m below P2 for 2 s (P2 at %.0f %.0f %.0f, camera axis %.2f %.2f %.2f): "
                 "camera reset", pp[1] - cam_y, pp[0], pp[1], pp[2], g_view2[8], g_view2[9], g_view2[10]);
            {
                PersistentHeap heap(ctx);
                Call(ctx, g_camera2, R32(R32(g_camera2) + kVtInit), {});
            }
            g_camera2_target = 0;  // SetTarget + P1's tuning again on the next frame
            below_since = 0;
        }
    }
    g_fov2 = F32(g_scratch);
    g_aspect2 = F32(g_scratch + 4);
    g_split_ready = settled && std::isfinite(g_fov2) && g_fov2 > 0;
}

// ---------------------------------------------------------------------------------------------------------------
// hc30: the level ending (quit to the main menu, level change). The game ends a level in 0x86980 (thiscall flow, 1
// arg; +0x50 "a level is loaded"), which unloads it through 0x88B80 (thiscall app world, 1 arg: every manager's
// shutdown, the actor manager's 0x2A890 removing all actors). Before any of that, P2 is let go: the reference P2's
// chase camera holds on him (+0x350, AddRef'd by this mod) is released, so his actor can be deleted with the others,
// and every hook leaves him alone (P2Live, g_unloading) until the world runs again. Each step is logged, and the
// watchdog names it (crumb) if the game stops inside it.
namespace {
void P2Detach(X86Context& ctx, const char* why) {
    const bool was = g_unloading.exchange(true);
    g_unload_gen++;
    if (g_camera2) {
        if (const uint32_t t = R32(g_camera2 + kChaseTarget)) {
            W32(g_camera2 + kChaseTarget, 0);
            Call(ctx, t, R32(R32(t) + 4), {});  // release (vtable +4)
        }
        kernel::G<uint8_t>(g_camera2 + 0x345) = 0;
    }
    g_camera2_target = 0;
    // hc36: P2's controller off P1's listeners before the game frees them (it rebuilds them at the level's end)
    if (const uint32_t c = g_p2_controller.load())
        if (const uint32_t list = R32(c + kCtrlListeners); list && g_p2_own_listener0.load())
            W32(list, g_p2_own_listener0.load());
    g_split_ready = false;
    g_ui_split = false;
    g_in_play = false;
    g_p2_settled = false;
    g_p2_present = false;
    g_p2_actor = 0;
    g_downed[0] = false, g_downed[1] = false;
    ClearP2Down();
    if (!was) LOGI("coop: level ending (%s): P2 let go, camera reference released", why);
    logx::Flush();
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00086980);
void HulkCoopSkinLevelEnd(X86Context& ctx);
X86_FUNC(sub_00086980) { Crumb crumb_("sub_00086980 end level");
    const uint32_t flow = ctx.ecx;
    const bool loaded = flow && kernel::G<uint8_t>(flow + 0x50);
    if (Enabled() && loaded) P2Detach(ctx, "0x86980");
    if (Enabled() && loaded) HulkCoopSkinLevelEnd(ctx);
    const int64_t t0 = CrumbNowMs();
    __imp__sub_00086980(ctx, base);
    if (Enabled() && loaded) LOGI("coop: level ended in %lld ms", (long long)(CrumbNowMs() - t0));
}
X86_EXTERN_FUNC(__imp__sub_00088B80);
X86_FUNC(sub_00088B80) { Crumb crumb_("sub_00088B80 unload world");
    if (Enabled()) P2Detach(ctx, "0x88B80");
    if (Enabled()) HulkCoopSkinLevelEnd(ctx);
    __imp__sub_00088B80(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_0002A890);
X86_FUNC(sub_0002A890) { Crumb crumb_("sub_0002A890 remove all actors");
    if (Enabled()) P2Detach(ctx, "0x2A890");
    __imp__sub_0002A890(ctx, base);
    if (Enabled()) LOGI("coop: all actors removed");
}

// hc31: THE QUIT FREEZE. The hc30 watchdog caught it: at the level's end the controller code (0xAE2F0 making one pad
// the active one -> 0xB2810 -> 0xB1DB0 -> 0xB1020, a button table of N x 32 bytes) asks new 0x8D750 for memory from
// the "Controller Heap" (heap 4, only 0xC800 bytes, sized for the game's own controllers). P2's controller and
// listeners take room there too, so it is full; the heap allocator (0x266E00, cdecl (heap, size)) then calls the
// game's out-of-memory handler ([0x527F64], 0x8D320), which never returns: the game froze. Now a full Controller Heap
// hands the block out of the Persistent Heap (17) instead. That is safe: free (0x267090) finds a block's heap by its
// address.
X86_EXTERN_FUNC(__imp__sub_00266E00);
X86_FUNC(sub_00266E00) {
    constexpr uint32_t kOutOfMemory = 0x527F64, kControllerHeap = 4;
    const uint32_t heap = X86_MEM32(ctx.esp + 4), size = X86_MEM32(ctx.esp + 8);
    if (heap != kControllerHeap || !R32(kHeapTable + kPersistentHeap * 16)) {
        __imp__sub_00266E00(ctx, base);
        return;
    }
    // one try in a heap, without the game's out-of-memory handler (it never returns): the block, or 0
    const uint32_t handler = R32(kOutOfMemory);
    auto attempt = [&](uint32_t h) {
        W32(kOutOfMemory, 0);
        X86_MEM32(ctx.esp + 4) = h;
        ctx.eax = 0;
        __imp__sub_00266E00(ctx, base);
        X86_MEM32(ctx.esp + 4) = heap;
        W32(kOutOfMemory, handler);
        return ctx.eax;
    };
    // P2's controller's own blocks: the Persistent Heap first (hc34), the Controller Heap if that has no room.
    // The game's: the Controller Heap first, the Persistent Heap if it is full. (hc32 also tried the Default Heap, 0:
    // its blocks can't be freed by free 0x267090, which crashed at the next free. Persistent blocks free normally.)
    const bool p2_own = g_p2_ctrl_alloc.load() > 0;
    const uint32_t first = p2_own ? kPersistentHeap : kControllerHeap, second = p2_own ? kControllerHeap : kPersistentHeap;
    if (attempt(first)) return;
    if (const uint32_t got = attempt(second)) {
        static int logged = 0;
        if (logged++ < 20)
            LOGI("coop: %s: heap %u had no room for %u bytes, heap %u gave them", p2_own ? "P2's controller" : "the game",
                 first, size, second);
        ctx.eax = got;
        return;
    }
    LOGE("coop: neither the Controller nor the Persistent Heap has %u bytes; the game's out-of-memory handler runs", size);
    logx::Flush();
    __imp__sub_00266E00(ctx, base);
}

// The controller agent's update (virtual, vtable 0x40D0A8; thiscall agent): it turns the left stick into a world
// direction with the render manager's camera matrix (0x1C3D90 -> +0x1C, fov +0x5C), which during the world update is
// always P1's view. For P2's agent (character controller index 1) the render manager holds P2's camera for the call,
// so his stick is relative to his own half of the screen.
X86_EXTERN_FUNC(__imp__sub_000FD0F0);
X86_FUNC(sub_000FD0F0) { Crumb crumb_("sub_000FD0F0");
    constexpr uint32_t kRenderManager = 0x4E231C;
    const uint32_t agent = ctx.ecx, rm = R32(kRenderManager);
    uint32_t chr = 0;
    if (Enabled() && g_split_ready.load() && rm && g_p2_actor.load()) {
        const uint32_t owner = R32(agent + 0x58);
        if (owner && R32(owner)) chr = Call(ctx, owner, R32(R32(owner) + 0x18), {});
    }
    if (!chr || chr != g_p2_actor.load() || R32(chr + kControllerIndex) != 1) {
        __imp__sub_000FD0F0(ctx, base);
        return;
    }
    uint8_t saved[0x44];
    std::memcpy(saved, kernel::HostPtr(rm + kRmView), sizeof(saved));  // matrix +0x1C..+0x5B, fov +0x5C
    std::memcpy(kernel::HostPtr(rm + kRmView), g_view2, sizeof(g_view2));
    W32(rm + kRmFov, std::bit_cast<uint32_t>(g_fov2));
    // hc43: the render manager's view is rebuilt from the matrix when +0x6C is 0 and then kept (clean = 1). If the stick
    // code rebuilt it here with P2's matrix, P1's next half was drawn from P2's camera once (the flash of P2's view on
    // P1's half): dirty before and after, so the view is always rebuilt from whichever matrix is in place.
    kernel::G<uint8_t>(rm + kRmClean) = 0;
    __imp__sub_000FD0F0(ctx, base);
    std::memcpy(kernel::HostPtr(rm + kRmView), saved, sizeof(saved));
    kernel::G<uint8_t>(rm + kRmClean) = 0;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("coop: P2's stick follows P2's camera (agent %08X)", agent);
    }
}

// The render manager's frame (0x1BC720: gets the manager, tail-jumps to its frame 0x1C3E10), called from
// game/hooks.cpp's HULK_SUB_RENDER_FRAME hook: drawn once per half while split.
X86_EXTERN_FUNC(__imp__sub_001BC720);
// hc32 trees and props vanishing in P2's half. The world draw (0x1C88F0) tests each object of its list against the
// half's view (0x1C2820) and, when it is outside, calls 0x1C4B20 on it (returning to 0x1C8E89): the object's fade
// value +0x80 jumps to its target +0x84, its fade speed +0x88 is cleared and +0xAC counts up. P1's half is drawn
// first, so whatever P1 wasn't looking at had that done to it before P2's half was drawn. (hc31 tried the same with
// the see-through fade 0x1C5130 instead; that was the wrong call: it is only a 50% fade.) While split, these calls are
// held through both halves and done once at the end, only for objects that neither half saw.
std::atomic<int> g_fade_pass{0};
std::vector<uint32_t> g_unseen[2];
X86_EXTERN_FUNC(__imp__sub_001C4B20);
X86_FUNC(sub_001C4B20) {
    const int pass = g_fade_pass.load();
    if (pass && X86_MEM32(ctx.esp) == 0x1C8E89) {
        if (g_unseen[pass - 1].size() < 16384) g_unseen[pass - 1].push_back(ctx.ecx);
        ctx.esp += 4;  // as its "ret"
        return;
    }
    __imp__sub_001C4B20(ctx, base);
}
// hc37 prop sort by the nearer camera: shared state (the hooks are with 0x1E6390 below)
constexpr uint32_t kPropEntries = 0x14608, kPropCount = 0x1E614, kPropMax = 2048;
std::atomic<uint32_t> g_prop_manager{0};  // hc44: the prop/instance manager (for spawning a saved prop)
std::atomic<bool> g_prop_shift{false};
uint32_t g_prop_mgr = 0, g_prop_scratch = 0, g_prop_scratch_i = 0;
float g_prop_c1[2], g_prop_c2[2];
struct PropOrig { bool moved; uint32_t x, z; };
// hc38: keyed by the entry's own id (+0x12, the index of its model record at +0x608): the update SWAPS the 0x14-byte
// entries while it sorts them, so a slot number does not stay with an instance (hc37 put positions back by slot:
// every moved instance got another one's position, buildings and cars piled up near the players).
std::vector<PropOrig> g_prop_orig(kPropMax);
std::vector<uint32_t> g_prop_list;
uint32_t PropEntry(uint32_t i) { return g_prop_mgr + kPropEntries + 0x14 * i; }
int PropId(uint32_t entry) {  // the instance's id, or -1
    if (!g_prop_mgr || entry < g_prop_mgr + kPropEntries) return -1;
    const uint32_t off = entry - g_prop_mgr - kPropEntries;
    if (off % 0x14 || off / 0x14 >= kPropMax) return -1;
    const int id = int(int16_t(kernel::G<uint16_t>(entry + 0x12)));
    return id >= 0 && id < int(kPropMax) ? id : -1;
}
bool PropNearerP2(float x, float z) {
    const float a = (x - g_prop_c1[0]) * (x - g_prop_c1[0]) + (z - g_prop_c1[1]) * (z - g_prop_c1[1]);
    const float b = (x - g_prop_c2[0]) * (x - g_prop_c2[0]) + (z - g_prop_c2[1]) * (z - g_prop_c2[1]);
    return b < a;
}
void PropMark(int i, uint32_t x, uint32_t z) {
    if (!g_prop_orig[i].moved) g_prop_list.push_back(uint32_t(i));
    g_prop_orig[i] = {true, x, z};
}
void PropUnmove(uint32_t entry) {  // the real position back in this entry
    const int id = PropId(entry);
    if (id < 0 || !g_prop_orig[id].moved) return;
    W32(entry, g_prop_orig[id].x);
    W32(entry + 8, g_prop_orig[id].z);
    g_prop_orig[id].moved = false;
}
// hc35 THE VANISHING TREES (and props, people): the world update asks the render manager whether a sphere is in view
// (0x1C4010, thiscall render manager (sphere), al: 1 in view; it rebuilds the view from +0x1C/+0x5C.. when the clean
// flag +0x6C is 0, then tests the sphere 0x1C1310). Its callers switch off what is out of view: the prop/instance
// manager (0x1E6BC4), the actor manager (0x26069, 0x262EC), characters (0x36A69), 0x1FBA37, 0x84CBA and the camera
// (0x99DB2). The update runs once with P1's camera, so what P1 didn't look at was off in P2's half too (hc33's
// draw-order test: still gone with P2's half drawn first). While split, outside the frame drawing, a sphere out of P1's
// view is tested again in P2's view (his camera put in the render manager for the test, then P1's put back).
X86_EXTERN_FUNC(__imp__sub_001C4010);
X86_FUNC(sub_001C4010) {
    const uint32_t rm = ctx.ecx, sphere = X86_MEM32(ctx.esp + 4);
    if (g_prop_shift.load() && X86_MEM32(ctx.esp) == 0x1E6BC9 && sphere) {  // hc37: a moved prop, tested where it is
        const int id = PropId(ctx.edi - 0x10);
        if (id >= 0 && g_prop_orig[id].moved) {
            W32(sphere, g_prop_orig[id].x);
            W32(sphere + 8, g_prop_orig[id].z);
        }
    }
    __imp__sub_001C4010(ctx, base);
    if ((ctx.eax & 0xFF) || g_fade_pass.load() || !rm || !g_split_ready.load() || !SplitWanted() || g_fov2 <= 0.0f)
        return;
    static uint32_t calls = 0, p2_only = 0;
    calls++;
    uint8_t saved[kRmViewBytes];
    std::memcpy(saved, kernel::HostPtr(rm + kRmView), kRmViewBytes);
    const X86Context entry = ctx;
    std::memcpy(kernel::HostPtr(rm + kRmView), g_view2, sizeof(g_view2));
    W32(rm + kRmFov, std::bit_cast<uint32_t>(g_fov2));
    kernel::G<uint8_t>(rm + kRmClean) = 0;
    // the same test again, called straight (not through this hook): sphere and a return slot below the stack in use
    X86Context c = entry;
    c.esp -= 0x40;
    c.esp -= 4;
    X86_MEM32(c.esp) = sphere;
    c.esp -= 4;
    X86_MEM32(c.esp) = 0;
    c.ecx = rm;
    __imp__sub_001C4010(c, base);
    const bool seen2 = c.eax & 0xFF;
    std::memcpy(kernel::HostPtr(rm + kRmView), saved, kRmViewBytes);
    kernel::G<uint8_t>(rm + kRmClean) = 0;
    if (seen2) {
        p2_only++;
        ctx.eax = (ctx.eax & ~0xFFu) | 1;
    }
    if (calls % 20000 == 0) {
        LOGI("coop: in-view tests out of P1's view: %u, %u of them in P2's view (kept on)", calls, p2_only);
        calls = p2_only = 0;
    }
}

// hc36/hc37 THE VANISHING TREES, part 2 (far ones; worst when the players are far apart). The prop/instance manager's
// update (0x1E6390, thiscall, only caller 0x1E6F3F) sorts every instance (0x14-byte entries from +0x14608, count
// +0x1E614: position +0/+4/+8, scaled distance +0xC, flags +0x10, model index +0x12) by its flat x/z distance from the
// render manager's camera position (rm +0x4C = P1's camera), and only the nearest ones get one of the loaded objects
// (at most 128, list +0x1E620, count +0x1E820, cap +0x1EEE4; radii [0x4C6F48] 150 m off, [0x4C6F44] 250 m in).
// hc36 widened the radii, but the pool still filled with what is near P1, so P2's trees went when he was far away.
// hc37: for the update, every instance nearer P2's camera than P1's is moved by (P1's camera - P2's camera), so its
// distance to P1's camera is its distance to P2's: the game sorts by the nearer of the two cameras. Loaded instances
// get their position copied in from their object at the start (0x1D6B40 from 0x1E63D3): that copy is moved the same
// way. The in-view test (0x1C4010 from 0x1E6BC4) and a load (0x1E2940, reads the position) see the real position.
// Every moved position is put back when the update returns.
X86_EXTERN_FUNC(__imp__sub_001E6390);
X86_FUNC(sub_001E6390) { Crumb crumb_("sub_001E6390 props");
    constexpr uint32_t kRenderManager = 0x4E231C, kPropOff = 0x4C6F48, kPropIn = 0x4C6F44, kPropCap = 0x1EEE4;
    // hc41: hc38's 120 / 1600-2000 m froze the game (hulk_045: out of memory in an allocation under the prop update,
    // 0x1E71F8 -> ... -> 0x8D710 -> 0x266E00 OOM handler; the heap-10 guard 0x1D5630 only covers the model load)
    const uint32_t kCoopCap = uint32_t(Config().prop_cap);  // the loaded list has 128 slots; the game keeps 50 (or 45)
    const float kCoopOff = Config().prop_radius, kCoopIn = Config().prop_radius * 1.25f;  // the game: 150 m / 250 m
    const uint32_t mgr = ctx.ecx, rm = R32(kRenderManager);
    if (mgr) g_prop_manager = mgr;
    static float base_off = 0, base_in = 0;
    static uint32_t game_cap = 0;
    if (base_off <= 0) base_off = F32(kPropOff), base_in = F32(kPropIn);
    const bool coop = mgr && g_p2_present.load() && !g_unloading.load() && base_off > 0 && base_in > 0;
    // hc38: while P2 plays, a very wide radius and more loaded instances (props loads stop by themselves when their
    // heap is low: 0x1D5630 needs 0xC800 bytes free in heap 10, so this can't run the game out of memory)
    if (mgr) {
        const uint32_t cap = R32(mgr + kPropCap);
        if (cap != kCoopCap || !game_cap) game_cap = cap;  // the game's own (it sets 45/50, scripts may change it)
        W32(mgr + kPropCap, coop ? std::max(game_cap, kCoopCap) : game_cap);
    }
    W32(kPropOff, std::bit_cast<uint32_t>(coop ? kCoopOff * kCoopOff : base_off));
    W32(kPropIn, std::bit_cast<uint32_t>(coop ? kCoopIn * kCoopIn : base_in));
    static uint32_t calls = 0, moved_total = 0, updates_moved = 0;
    bool on = false;
    if (coop && Config().prop_sort && rm && g_split_ready.load() && SplitWanted()) {
        g_prop_c1[0] = F32(rm + 0x4C), g_prop_c1[1] = F32(rm + 0x54);
        g_prop_c2[0] = g_view2[12], g_prop_c2[1] = g_view2[14];
        const float sx = g_prop_c1[0] - g_prop_c2[0], sz = g_prop_c1[1] - g_prop_c2[1];
        on = std::isfinite(sx) && std::isfinite(sz) && sx * sx + sz * sz > 4.0f;  // more than 2 m apart
        if (on) {
            if (!g_prop_scratch) g_prop_scratch = PersistentNew(ctx, 256 * 12);
            on = g_prop_scratch != 0;
        }
        if (on) {
            g_prop_mgr = mgr;
            g_prop_scratch_i = 0;
            const uint32_t n = std::min(R32(mgr + kPropCount), kPropMax);
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t e = PropEntry(i);
                const int id = PropId(e);
                const float x = F32(e), z = F32(e + 8);
                if (id < 0 || g_prop_orig[id].moved || !PropNearerP2(x, z)) continue;
                PropMark(id, R32(e), R32(e + 8));
                W32(e, std::bit_cast<uint32_t>(x + sx));
                W32(e + 8, std::bit_cast<uint32_t>(z + sz));
            }
            moved_total += uint32_t(g_prop_list.size());
            updates_moved++;
            g_prop_shift = true;
        }
    }
    __imp__sub_001E6390(ctx, base);
    if (on) {
        g_prop_shift = false;
        // every entry back where it really is, found by its id wherever the sort put it
        const uint32_t n = std::min(R32(mgr + kPropCount), kPropMax);
        for (uint32_t i = 0; i < n; i++) PropUnmove(PropEntry(i));
        uint32_t lost = 0;
        for (const uint32_t id : g_prop_list) lost += g_prop_orig[id].moved ? 1 : 0;
        if (lost) {  // hc43: the update can leave an entry past the count; look through the whole table (2048 entries)
            for (uint32_t i = n; i < kPropMax; i++) PropUnmove(PropEntry(i));
            lost = 0;
            for (const uint32_t id : g_prop_list) lost += g_prop_orig[id].moved ? 1 : 0;
        }
        for (const uint32_t id : g_prop_list) g_prop_orig[id].moved = false;
        g_prop_list.clear();
        static int lost_logged = 0;
        if (lost && lost_logged++ < 10) LOGE("coop: %u moved props not found after the update", lost);
    }
    if (mgr && ++calls % 1800 == 0) {
        LOGI("coop: props: radius %.0f/%.0f m, cap %u (game %u), %u loaded; nearer-camera sort in %u of 1800 updates "
             "(%u near P2 on average)",
             std::sqrt(F32(kPropOff)), std::sqrt(F32(kPropIn)), R32(mgr + kPropCap), game_cap, R32(mgr + 0x1E820),
             updates_moved, updates_moved ? moved_total / updates_moved : 0);
        moved_total = updates_moved = 0;
    }
}
// an object's position (thiscall, eax: pointer to x,y,z); from 0x1E63D3 it is copied into the entry at edi
X86_EXTERN_FUNC(__imp__sub_001D6B40);
X86_FUNC(sub_001D6B40) {
    const bool props = g_prop_shift.load() && X86_MEM32(ctx.esp) == 0x1E63D8;
    const uint32_t entry = ctx.edi;
    __imp__sub_001D6B40(ctx, base);
    if (!props || !ctx.eax) return;
    const int id = PropId(entry);
    if (id < 0) return;
    const float x = F32(ctx.eax), z = F32(ctx.eax + 8);
    if (!PropNearerP2(x, z)) {
        g_prop_orig[id].moved = false;  // the game copies its real position in now
        return;
    }
    const uint32_t slot = g_prop_scratch + 12 * (g_prop_scratch_i++ % 256);
    W32(slot, std::bit_cast<uint32_t>(x + g_prop_c1[0] - g_prop_c2[0]));
    W32(slot + 4, R32(ctx.eax + 4));
    W32(slot + 8, std::bit_cast<uint32_t>(z + g_prop_c1[1] - g_prop_c2[1]));
    PropMark(id, R32(ctx.eax), R32(ctx.eax + 8));
    ctx.eax = slot;
}
// load an instance (thiscall entry index, ret 4): it reads the entry's position, so it gets the real one
X86_EXTERN_FUNC(__imp__sub_001E2940);
X86_FUNC(sub_001E2940) { Crumb crumb_("sub_001E2940 prop load");
    if (g_prop_shift.load() && ctx.ecx == g_prop_mgr) {
        const uint32_t i = X86_MEM32(ctx.esp + 4);
        if (i < kPropMax) PropUnmove(PropEntry(i));
    }
    __imp__sub_001E2940(ctx, base);
}

// after both halves: what neither half saw
void FadeAfterPasses(X86Context& ctx) {
    g_fade_pass = 0;
    auto& a = g_unseen[0];
    auto& b = g_unseen[1];
    std::sort(a.begin(), a.end());
    a.erase(std::unique(a.begin(), a.end()), a.end());
    std::sort(b.begin(), b.end());
    static uint32_t frames = 0, hidden = 0, kept = 0;
    for (const uint32_t obj : a)
        if (std::binary_search(b.begin(), b.end(), obj)) {
            Call(ctx, obj, 0x1C4B20, {});
            hidden++;
        } else {
            kept++;
        }
    if (++frames % 1800 == 0) {
        LOGI("coop: objects out of view over %u frames: %u out of both halves, %u only out of P1's (kept for P2)",
             frames, hidden, kept);
        frames = hidden = kept = 0;
    }
    a.clear();
    b.clear();
}

void CoopRenderFrame(X86Context& ctx, uint8_t* base) {
    g_render_ms = CrumbNowMs();  // (no crumb: a loading screen keeps drawing while the game is otherwise stuck)
    constexpr uint32_t kRenderManager = 0x4E231C;
    const uint32_t rm = R32(kRenderManager);
    if (!SplitWanted() || !g_split_ready.load() || !rm) {
        g_ui_split = false;
        __imp__sub_001BC720(ctx, base);
        return;
    }
    uint8_t saved[kRmViewBytes];
    std::memcpy(saved, kernel::HostPtr(rm + kRmView), kRmViewBytes);
    const X86Context entry = ctx;
    const uint32_t swaps = hulk::g_frame_swaps.load();
    const bool vertical = SplitVertical();
    const float shape = vertical ? 0.5f : 2.0f;  // a half's shape against the whole screen's
    d3d::SetSplitScreenVertical(vertical);
    float aspect1;
    std::memcpy(&aspect1, saved + (kRmAspect - kRmView), 4);
    // one half: its view in the render manager, then the game's frame (or the black half of a downed player)
    auto pass = [&](int who) {
        d3d::SetSplitScreenHalf(who);
        if (who == 1) {
            std::memcpy(kernel::HostPtr(rm + kRmView), saved, kRmViewBytes);
        } else {
            std::memcpy(kernel::HostPtr(rm + kRmView), g_view2, sizeof(g_view2));
            W32(rm + kRmFov, std::bit_cast<uint32_t>(g_fov2));
        }
        W32(rm + kRmAspect, std::bit_cast<uint32_t>(aspect1 * shape));
        kernel::G<uint8_t>(rm + kRmClean) = 0;
        g_fade_pass = who;
        if (who == 2 && g_downed[1].load()) {  // as before: P2's black half instead of his frame
            coop_render_BlackHalf(ctx);
            return;
        }
        __imp__sub_001BC720(ctx, base);
        if (who == 1 && g_downed[0].load()) {  // as before: P1's frame, then his half blacked out
            X86Context c = entry;
            coop_render_BlackHalf(c);
        }
    };
    // hc32 test switch (mod menu "Draw P2's half first"): if the game decides what to show from the half drawn first,
    // the vanishing trees move to P1's screen with it on; if they stay on P2's, the world update decides it (P1's view)
    const bool p2_first = DrawP2First();
    const int first = p2_first ? 2 : 1, second = p2_first ? 1 : 2;
    pass(first);
    if (hulk::g_frame_swaps.load() != swaps) {  // the frame was shown inside: two passes would show two frames
        g_split_broken = true;
        LOGE("coop: the render manager's frame swaps the screen; split screen off");
        d3d::SetSplitScreenHalf(0);
        std::memcpy(kernel::HostPtr(rm + kRmView), saved, kRmViewBytes);
        kernel::G<uint8_t>(rm + kRmClean) = 0;
        FadeAfterPasses(ctx);
        return;
    }
    const X86Context after = ctx;
    ctx = entry;
    pass(second);
    ctx = after;
    FadeAfterPasses(ctx);
    d3d::SetSplitScreenHalf(0);
    std::memcpy(kernel::HostPtr(rm + kRmView), saved, kRmViewBytes);
    kernel::G<uint8_t>(rm + kRmClean) = 0;
    g_ui_split = true;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("coop: split screen drawing (P2 fov %.2f aspect %.2f; render manager aspect %.3f, %.3f per half)", g_fov2,
             g_aspect2, aspect1, aspect1 * shape);
    }
}

// hc39 P2'S LOCK-ON BOX (helicopters): the box around a locked-on target ("target_frame" / "locked_border", the target
// icon object at app +0x40, vtable 0x42125C) is not a HUD overlay: the render manager draws it at the end of the world
// frame (0x1C3E10 -> [rm +0x10] vtable +0 = 0x1BDDA0 -> 0x1BCEF0), from the actor manager's main actor's target
// (+0x124 -> +0x328: +0xC type, +8 target). The world frame is drawn once per half, so P2's half showed P1's target's
// box (or none). Targets that also get the HUD's indicator looked fine for P2; helicopters only have this box. Now P2
// is the main actor while the box is drawn in P2's half (as in his HUD pass), put back at once.
X86_EXTERN_FUNC(__imp__sub_001BDDA0);
X86_FUNC(sub_001BDDA0) { Crumb crumb_("sub_001BDDA0 target box");
    const uint32_t p2 = g_fade_pass.load() == 2 ? P2Live() : 0;
    const uint32_t am = p2 ? ActorManager() : 0;
    const uint32_t main_was = am ? R32(am + kMainActor) : 0;
    if (main_was) W32(am + kMainActor, p2);
    __imp__sub_001BDDA0(ctx, base);
    if (main_was && R32(am + kMainActor) == p2) W32(am + kMainActor, main_was);
    if (main_was) {  // P2's target, logged when it changes (type 1 = character, 5 = prop/vehicle)
        static uint32_t last = 0;
        const uint32_t info = R32(p2 + 0x328);
        const uint32_t target = info ? R32(info + 8) : 0;
        if (target != last) {
            last = target;
            LOGI("coop: P2's target box: target %08X type %u", target, info ? R32(info + 0xC) : 0);
        }
    }
}

// The overlays ("Render Overlays" 0xD6C70, thiscall overlay manager: 8 overlays at +0xC, +4 on, vtable +0x10 render)
// are drawn after the world frame, full screen, through a 3D projection whose aspect comes from the display object
// [0x52C2A4] (vtable +0x40). While split, they are drawn once per half: the top half as the game has them (P1's HUD,
// messages), the bottom half only the HUD overlay (vtable 0x40A498) aimed at P2 (its actor at +0x14).
namespace {
constexpr uint32_t kHudVtable = 0x40A498, kHudActor = 0x14, kDisplay = 0x52C2A4;
// The display's aspect getter (0x31B500) answers one of two constants, 16:9 (0x432484) when its widescreen flag
// (+0x437) is set, else 4:3 (0x432480): both are doubled while the overlays draw into a half-height half, or the HUD
// would come out twice as wide as it should.
constexpr uint32_t kAspectWide = 0x432484, kAspect43 = 0x432480;
}  // namespace

// P2's copy of the HUD overlay's power gauge. Its update (vtable +0xC, 0xD0240) runs the gauge part 0xCBC40 (thiscall
// dt, ret 4): it reads the tracked actor's (+0x14) health, keeps its smoothing in +0x14C..+0x1A3 and sets the shared
// gauge drawables. P2 has his own copy of that block: 0xCBC40 runs for him after the game's update, and again with
// dt 0 right before each half is drawn, so each half's gauge shows its own player. (Running the whole update for P2
// crashed in hc20: it reaches parts only the main player has.)
namespace {
constexpr uint32_t kHudGauge = 0xCBC40, kGaugeLo = 0x14C, kGaugeHi = 0x1A4, kGaugeWords = (kGaugeHi - kGaugeLo) / 4;
uint32_t g_gauge_hud = 0, g_gauge2[kGaugeWords], g_gauge1[kGaugeWords], g_gauge1_actor = 0;
bool g_gauge2_in = false;
void GaugeIn(uint32_t hud, uint32_t p2) {
    if (g_gauge_hud != hud) {  // a new HUD (level load): P2's gauge starts as P1's
        g_gauge_hud = hud;
        for (uint32_t i = 0; i < kGaugeWords; i++) g_gauge2[i] = R32(hud + kGaugeLo + 4 * i);
    }
    for (uint32_t i = 0; i < kGaugeWords; i++) {
        g_gauge1[i] = R32(hud + kGaugeLo + 4 * i);
        W32(hud + kGaugeLo + 4 * i, g_gauge2[i]);
    }
    g_gauge1_actor = R32(hud + kHudActor);
    W32(hud + kHudActor, p2);
    g_gauge2_in = true;
}
// hc31/hc32: the gauge's glow animations (+0x168, +0x19C: objects whose list +0x24..+0x28 is played through vtable
// +0x38 / +0x30) are the game's own, one set for both HUDs. The gauge's state machine (+0x1A0, driven by the actor's
// critical mass state +0x1B28) restarts them on each change of state, so P2's runs restarted them for P2's state
// every frame and P1's full bar flashed. hc31 gave P2's runs an empty stand-in, which also left P2's fill at the
// wrong size; hc32 instead runs P2's gauge with P1's state (P1's +0x1A0 copied in, P2's +0x1B28 = P1's for the call),
// so P2's run asks the shared animations for exactly what P1's already did and restarts nothing.
void P2Gauge(X86Context& ctx, uint32_t hud, uint32_t dt) {
    const uint32_t p2 = R32(hud + kHudActor), p1 = g_gauge1_actor;
    if (!p2 || !p1) {
        Call(ctx, hud, kHudGauge, {dt});
        return;
    }
    const uint32_t cm2 = R32(p2 + 0x1B28);
    W32(p2 + 0x1B28, R32(p1 + 0x1B28));
    W32(hud + 0x1A0, g_gauge1[(0x1A0 - kGaugeLo) / 4]);
    Call(ctx, hud, kHudGauge, {dt});
    W32(p2 + 0x1B28, cm2);
}
void GaugeOut(uint32_t hud) {
    if (!g_gauge2_in) return;
    g_gauge2_in = false;
    for (uint32_t i = 0; i < kGaugeWords; i++) {
        g_gauge2[i] = R32(hud + kGaugeLo + 4 * i);
        W32(hud + kGaugeLo + 4 * i, g_gauge1[i]);
    }
    W32(hud + kHudActor, g_gauge1_actor);
}
}  // namespace

// hc30 radar. The HUD's map (CHudMap, vtable 0x40A1F4, HUD +0x88) keeps 0x80 icon entries from +0x258, 0x40 bytes
// each: +0x00 icon type (-1 free; 0 the player's arrow, which alone gets a heading), +0x04 world position, +0x10/+0x14
// map position, +0x18 heading (x, z, 0), +0x28/+0x2C the actor's uid (actor +8/+0xC; found by 0x24280), +0x30 source
// (0 an actor, 2 a fixed place), +0x34 fade, +0x3C "the map centres on this one", +0x3D hidden off the map's edge.
// Its update (0xC9810, run only in the game's HUD update, so for P1) fills the positions and the heading and puts the
// centre (+0x38/+0x3C) on the flagged entry (0xC9720, when +0x20 is 1); its draw (0xC8850) only reads them. So P2's
// radar showed P1's arrow. For each half's draw the entries are set up for that half's player, then put back:
//   P1's half: + an arrow for P2 (blue);   P2's half: the player's arrow on P2 (blue), the centre on him, + P1's arrow.
// Blue: the icon draw of that one entry runs with d3d tint 2 (shaders.cpp: green and blue swapped). The draw loop
// keeps the entry in edi; the per-icon scale call (0x293DA0, returning to 0xC8E79) is where the tint is set.
namespace {
constexpr uint32_t kMapVtable = 0x40A1F4, kHudMapPtr = 0x88, kMapEntries = 0x258, kMapEntrySize = 0x40,
                   kMapEntryCount = 0x80, kMapBytes = kMapEntrySize * kMapEntryCount;
uint8_t g_map_saved[kMapBytes], g_map_centre_saved[8];
uint32_t g_map_saved_at = 0, g_radar_scratch = 0;
std::atomic<uint32_t> g_blue_entry{0};
std::atomic<bool> g_map_drawing{false};

// an entry tracking this actor: uid, place on the map and (for the arrow) heading, as the map's update makes them
void RadarAim(X86Context& ctx, uint32_t map, uint32_t e, uint32_t actor) {
    const uint32_t s = g_radar_scratch;
    float pos[3];
    if (!s || !Position(actor, pos)) return;
    W32(e + 0x28, R32(actor + 8));
    W32(e + 0x2C, R32(actor + 0xC));
    W32(e + 0x30, 0);
    for (int i = 0; i < 3; i++) W32(e + 4 + 4 * i, std::bit_cast<uint32_t>(pos[i]));
    Call(ctx, map, 0xC9460, {s + 0x50, std::bit_cast<uint32_t>(pos[0]), std::bit_cast<uint32_t>(pos[1]),
                             std::bit_cast<uint32_t>(pos[2])});  // world -> map (thiscall &out, x, y, z)
    W32(e + 0x10, R32(s + 0x50));
    W32(e + 0x14, R32(s + 0x54));
    if (R32(e) != 0) return;
    // heading: the actor's facing (0x35550 thiscall &out), x and z, turned by 3/2 pi about the vertical as 0xC9810 does
    if (!(Call(ctx, actor, 0x35550, {s + 0x40}) & 0xFF)) return;
    Call(ctx, s, 0x22B980, {});            // identity
    Call(ctx, s, 0x22BAB0, {0x4096CBE4});  // rotation about Y (4.712 rad)
    W32(e + 0x18, R32(s + 0x40));
    W32(e + 0x1C, R32(s + 0x48));
    W32(e + 0x20, 0);
    Call(ctx, e + 0x18, 0x22D260, {s});  // vector *= matrix
}
// hc45 SMASHABLE BUILDINGS ON THE RADAR. The prop manager's instance table (entries 0x14 bytes at +0x14608, count
// +0x1E614: +0 x, +4 y, +8 z, +0x10 flags (bit 0 loaded; size class (flags >> 10) & 3, 3 = the largest, 180 m view
// distance: buildings), +0x12 record id; record +0x608 + id * 0x28, its +0x18 the loaded object) and a loaded object's
// hit points at +0x108 (spm_GetSmartPropHitPoints, 0x1DC940). Every loaded big instance with hit points within 150 m
// of the half's player gets a radar entry with the game's smash icon (type 2: hulk 0, enemy 1, smash 2 in the icon
// shape table 0xCA61F), a fixed place (+0x30 = 2), made from the player's arrow entry. Nearest 24 at most.
void RadarSmashable(X86Context& ctx, uint32_t map, uint32_t player_entry, uint32_t me) {
    const uint32_t mgr = g_prop_manager.load();
    float mp[3];
    if (!ShowSmashable() || !mgr || !g_radar_scratch || !Position(me, mp)) return;
    struct Hit { float d2, x, y, z; };
    Hit hits[24];
    int n = 0;
    uint32_t big = 0, with_hp = 0;
    const uint32_t count = std::min(R32(mgr + 0x1E614), 2048u);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t e = mgr + 0x14608 + 0x14 * i;
        const uint16_t flags = kernel::G<uint16_t>(e + 0x10);
        if (!(flags & 1) || ((flags >> 10) & 3) != 3) continue;
        big++;
        const int16_t id = kernel::G<int16_t>(e + 0x12);
        if (id < 0 || id >= 0x800) continue;
        const uint32_t obj = R32(mgr + 0x608 + 0x28 * uint32_t(id) + 0x18);
        if (!obj) continue;
        const uint32_t hp = R32(obj + 0x108);
        if (!hp || hp == 0x80000000u || hp >= 0x7F800000u) continue;  // 0, -0, inf/nan or a negative/huge value
        with_hp++;
        const float x = F32(e), y = F32(e + 4), z = F32(e + 8);
        const float dx = x - mp[0], dz = z - mp[2], d2 = dx * dx + dz * dz;
        if (!(d2 < 300.0f * 300.0f)) continue;  // hc47: 300 m (was 150)
        if (n < 24) hits[n++] = {d2, x, y, z};
        else {
            int far_i = 0;
            for (int k = 1; k < 24; k++)
                if (hits[k].d2 > hits[far_i].d2) far_i = k;
            if (d2 < hits[far_i].d2) hits[far_i] = {d2, x, y, z};
        }
    }
    int placed = 0;
    for (int k = 0; k < n; k++) {
        uint32_t slot = 0;
        for (uint32_t i = 0; i < kMapEntryCount && !slot; i++)
            if (R32(map + kMapEntries + i * kMapEntrySize) == 0xFFFFFFFF) slot = map + kMapEntries + i * kMapEntrySize;
        if (!slot) break;
        std::memcpy(kernel::HostPtr(slot), kernel::HostPtr(player_entry), kMapEntrySize);
        W32(slot, 2);          // the smash icon
        W32(slot + 0x30, 2);   // a fixed place
        W32(slot + 0x28, 0), W32(slot + 0x2C, 0);
        kernel::G<uint8_t>(slot + 0x3C) = 0;
        kernel::G<uint8_t>(slot + 0x3D) = 0;
        W32(slot + 4, std::bit_cast<uint32_t>(hits[k].x));
        W32(slot + 8, std::bit_cast<uint32_t>(hits[k].y));
        W32(slot + 12, std::bit_cast<uint32_t>(hits[k].z));
        const uint32_t s = g_radar_scratch;
        Call(ctx, map, 0xC9460, {s + 0x50, std::bit_cast<uint32_t>(hits[k].x), std::bit_cast<uint32_t>(hits[k].y),
                                 std::bit_cast<uint32_t>(hits[k].z)});
        W32(slot + 0x10, R32(s + 0x50));
        W32(slot + 0x14, R32(s + 0x54));
        placed++;
    }
    static int64_t last_log = 0;
    if (CrumbNowMs() - last_log > 15000) {
        last_log = CrumbNowMs();
        LOGI("coop: radar smashables: %u big loaded, %u with hit points, %d within 300 m, %d shown", big, with_hp, n,
             placed);
    }
}
// before a half's HUD draws: me is the half's player, partner the other one. Returns the map when it changed it.
uint32_t RadarIn(X86Context& ctx, uint32_t hud, uint32_t me, uint32_t partner, bool me_blue) {
    g_blue_entry = 0;
    const uint32_t map = hud ? R32(hud + kHudMapPtr) : 0;
    if (!map || R32(map) != kMapVtable || !me || !partner) return 0;
    if (!g_radar_scratch) g_radar_scratch = PersistentNew(ctx, 0x80);
    if (!g_radar_scratch) return 0;
    // the player's arrow: the centred arrow entry (else the arrow on P1)
    uint32_t player = 0, free_entry = 0;
    const uint32_t a1 = R32(me + 8), a2 = R32(me + 0xC);
    (void)a1, (void)a2;
    for (uint32_t i = 0; i < kMapEntryCount; i++) {
        const uint32_t e = map + kMapEntries + i * kMapEntrySize;
        const uint32_t type = R32(e);
        if (type == 0xFFFFFFFF) {
            if (!free_entry) free_entry = e;
            continue;
        }
        if (type == 0 && R32(e + 0x30) == 0 && (!player || kernel::G<uint8_t>(e + 0x3C) == 1)) player = e;
    }
    if (!player || !free_entry) return 0;
    std::memcpy(g_map_saved, kernel::HostPtr(map + kMapEntries), kMapBytes);
    std::memcpy(g_map_centre_saved, kernel::HostPtr(map + 0x38), 8);
    g_map_saved_at = map;
    // the partner's arrow, a copy of the player's (same icon, fade) aimed at the partner, not centred, kept on the edge
    std::memcpy(kernel::HostPtr(free_entry), kernel::HostPtr(player), kMapEntrySize);
    kernel::G<uint8_t>(free_entry + 0x3C) = 0;
    kernel::G<uint8_t>(free_entry + 0x3D) = 0;
    RadarAim(ctx, map, free_entry, partner);
    RadarSmashable(ctx, map, player, me);
    if (me_blue) {  // P2's half: the player's arrow on P2, and the map centred on him
        RadarAim(ctx, map, player, me);
        float pos[3];
        if (kernel::G<uint8_t>(map + 0x20) == 1 && Position(me, pos))
            Call(ctx, map, 0xC9720, {std::bit_cast<uint32_t>(pos[0]), std::bit_cast<uint32_t>(pos[1]),
                                     std::bit_cast<uint32_t>(pos[2])});
        g_blue_entry = player;
    } else {
        g_blue_entry = free_entry;
    }
    static int logged = 0;
    if (logged < 2) {
        logged++;
        LOGI("coop: radar for P%d: arrow entry %u, partner's arrow in entry %u", me_blue ? 2 : 1,
             (player - map - kMapEntries) / kMapEntrySize, (free_entry - map - kMapEntries) / kMapEntrySize);
    }
    return map;
}
void RadarOut(uint32_t map) {
    g_blue_entry = 0;
    if (!map || map != g_map_saved_at) return;
    std::memcpy(kernel::HostPtr(map + kMapEntries), g_map_saved, kMapBytes);
    std::memcpy(kernel::HostPtr(map + 0x38), g_map_centre_saved, 8);
    g_map_saved_at = 0;
}
}  // namespace

// the map's draw (thiscall): the blue tint, if one was set for an icon, ends with it
X86_EXTERN_FUNC(__imp__sub_000C8850);
X86_FUNC(sub_000C8850) { Crumb crumb_("sub_000C8850");
    g_map_drawing = true;
    __imp__sub_000C8850(ctx, base);
    g_map_drawing = false;
    if (g_blue_entry.load()) d3d::SetTint(0.0f);
}
// the 3D state's scale (0x293DA0, thiscall x, y, z): in the map's icon loop it comes right before each icon is drawn
X86_EXTERN_FUNC(__imp__sub_00293DA0);
X86_FUNC(sub_00293DA0) {
    if (g_map_drawing.load() && X86_MEM32(ctx.esp) == 0xC8E79 && g_blue_entry.load())
        d3d::SetTint(ctx.edi == g_blue_entry.load() ? 2.0f : 0.0f);
    __imp__sub_00293DA0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_000D0240);
X86_FUNC(sub_000D0240) { Crumb crumb_("sub_000D0240");
    const uint32_t hud = ctx.ecx, dt = X86_MEM32(ctx.esp + 4), p2 = P2Live();
    __imp__sub_000D0240(ctx, base);
    if (g_gauge2_in || !SplitWanted() || !p2 || !hud || R32(hud) != kHudVtable || !R32(hud + kHudActor)) return;
    GaugeIn(hud, p2);
    P2Gauge(ctx, hud, dt);
    GaugeOut(hud);
}

X86_EXTERN_FUNC(__imp__sub_000D6C70);
X86_FUNC(sub_000D6C70) { Crumb crumb_("sub_000D6C70");
    const uint32_t mgr = ctx.ecx, p2 = P2Live();
    if (!SplitWanted() || !g_ui_split.load() || !p2 || !mgr) {
        __imp__sub_000D6C70(ctx, base);
        return;
    }
    uint32_t hud = 0;
    for (int i = 0; i < 8 && !hud; i++)
        if (const uint32_t o = R32(mgr + 0xC + 4 * i); o && R32(o) == kHudVtable) hud = o;
    const uint32_t wide_was = R32(kAspectWide), std_was = R32(kAspect43);
    const float shape = SplitVertical() ? 0.5f : 2.0f;
    W32(kAspectWide, std::bit_cast<uint32_t>(std::bit_cast<float>(wide_was) * shape));
    W32(kAspect43, std::bit_cast<uint32_t>(std::bit_cast<float>(std_was) * shape));
    const X86Context entry = ctx;
    const bool gauge = hud && R32(hud + kHudActor);
    // P1, top: every overlay (P1's gauge set again: P2's update set it last)
    if (gauge) Call(ctx, hud, kHudGauge, {0});
    d3d::SetSplitScreenHalf(1);
    {
        const uint32_t am1 = ActorManager(), p1 = am1 ? R32(am1 + kMainActor) : 0;
        const uint32_t map = hud ? RadarIn(ctx, hud, p1, p2, false) : 0;  // + P2's arrow (blue) on P1's radar
        __imp__sub_000D6C70(ctx, base);
        RadarOut(map);
    }
    const X86Context after = ctx;
    // P2, bottom: the HUD only, on P2
    if (hud) {
        uint8_t on[8] = {};
        for (int i = 0; i < 8; i++)
            if (const uint32_t o = R32(mgr + 0xC + 4 * i); o && o != hud) {
                on[i] = kernel::G<uint8_t>(o + 4);
                kernel::G<uint8_t>(o + 4) = 0;
            }
        ctx = entry;
        if (gauge) {
            GaugeIn(hud, p2);
            P2Gauge(ctx, hud, 0);
        } else {
            W32(hud + kHudActor, p2);
        }
        d3d::SetSplitScreenHalf(2);
        // the radar (and anything else the HUD reads from "the player" and the view): P2 is the main actor and P2's
        // camera is the render manager's while his HUD draws, both put back at once
        const uint32_t am = ActorManager(), rm = R32(0x4E231C);
        const uint32_t main_was = am ? R32(am + kMainActor) : 0;
        uint8_t view_was[0x44];
        if (rm) {
            std::memcpy(view_was, kernel::HostPtr(rm + kRmView), sizeof(view_was));
            std::memcpy(kernel::HostPtr(rm + kRmView), g_view2, sizeof(g_view2));
            W32(rm + kRmFov, std::bit_cast<uint32_t>(g_fov2));
        }
        // P2's radar: his own arrow (blue) and centre, + P1's arrow
        const uint32_t map = RadarIn(ctx, hud, p2, main_was, true);
        if (am && main_was) W32(am + kMainActor, p2);
        __imp__sub_000D6C70(ctx, base);
        if (am && main_was) W32(am + kMainActor, main_was);
        RadarOut(map);
        if (rm) std::memcpy(kernel::HostPtr(rm + kRmView), view_was, sizeof(view_was));
        if (gauge) GaugeOut(hud);
        else W32(hud + kHudActor, 0);
        for (int i = 0; i < 8; i++)
            if (const uint32_t o = R32(mgr + 0xC + 4 * i); o && o != hud) kernel::G<uint8_t>(o + 4) = on[i];
    }
    ctx = after;
    d3d::SetSplitScreenHalf(0);
    W32(kAspectWide, wide_was);
    W32(kAspect43, std_was);
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("coop: overlays drawn per half (HUD overlay %08X, display aspect %.3f, doubled while drawn)", hud,
             std::bit_cast<float>(wide_was));
    }
}

// Red Hulk: P2 is drawn recoloured (green skin red, blue trousers near black; gpu shaders.cpp RedHulk). The scene's
// objects (Render::Object, vtable 0x4218C8) are drawn by 0x1C5460 (thiscall): its drawable at +0x10, its world matrix
// at +0x30 with the position at +0x60. The object standing where P2 stands (nearer him than P1) is drawn with
// d3d::SetTint(1).
namespace d3d {
void SetTint(float tint);
}
std::atomic<uint32_t> g_p2_drawable{0};
std::atomic<bool> g_tint_all{false};
// the parts of P2's (and P1's) CompositeDrawable: +0x44 -> a list of 12-byte entries (+0x3C first, +0x40 end), the part's
// drawable at +8 of each. The game's draw queue displays the parts (a PolySkin's Display, 0x2B78C0) without going through
// the composite, so P2's parts are recoloured there.
constexpr int kMaxParts = 48;
std::atomic<uint32_t> g_p2_parts[kMaxParts], g_p1_parts[kMaxParts], g_p1_drawable{0};
std::atomic<int> g_p2_part_count{0}, g_p1_part_count{0};
int Parts(uint32_t comp, uint32_t* out) {
    const uint32_t list = comp ? R32(comp + 0x44) : 0;
    if (!list) return 0;
    const uint32_t first = R32(list + 0x3C), end = R32(list + 0x40);
    if (!first || end < first || (end - first) / 0xC > uint32_t(kMaxParts)) return 0;
    int n = 0;
    for (uint32_t e = first; e < end; e += 0xC)
        if (const uint32_t d = R32(e + 8)) out[n++] = d;
    return n;
}
void NoteP2Drawable(uint32_t comp) {
    uint32_t parts[kMaxParts];
    const int n = Parts(comp, parts);
    for (int i = 0; i < n; i++) g_p2_parts[i] = parts[i];
    g_p2_part_count = n;
    uint32_t p1parts[kMaxParts];
    const int m = Parts(g_p1_drawable.load(), p1parts);
    int shared = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < m; j++) shared += parts[i] == p1parts[j];
    LOGI("coop: Red Hulk: P2's model has %d parts (first %08X, vtable %08X), P1's %d, %d shared", n, n ? parts[0] : 0,
         n ? R32(parts[0]) : 0, m, shared);
    logx::Flush();
}
void NoteP1Drawable(uint32_t comp) {
    uint32_t parts[kMaxParts];
    const int n = Parts(comp, parts);
    for (int i = 0; i < n; i++) g_p1_parts[i] = parts[i];
    g_p1_part_count = n;
}
// one of P2's parts that P1's model does not share (a shared one would turn P1 red too)
bool IsP2Part(uint32_t d) {
    bool mine = false;
    for (int i = 0, n = g_p2_part_count.load(); i < n && !mine; i++) mine = g_p2_parts[i].load() == d;
    if (!mine) return false;
    for (int i = 0, n = g_p1_part_count.load(); i < n; i++)
        if (g_p1_parts[i].load() == d) return false;
    return true;
}
// one of P1's parts that P2's model does not share (hc30: Red Hulk may be P1)
bool IsP1Part(uint32_t d) {
    bool mine = false;
    for (int i = 0, n = g_p1_part_count.load(); i < n && !mine; i++) mine = g_p1_parts[i].load() == d;
    if (!mine) return false;
    for (int i = 0, n = g_p2_part_count.load(); i < n; i++)
        if (g_p2_parts[i].load() == d) return false;
    return true;
}  // test (F12 with coop_debug.txt): every draw recoloured, to check the renderer side  // P2's drawable (the object's +0x10), found by 0x1C5460 below
X86_EXTERN_FUNC(__imp__sub_001C5460);
X86_FUNC(sub_001C5460) { Crumb crumb_("sub_001C5460");
    const uint32_t obj = ctx.ecx, p2 = P2Live();
    float a[3], b[3];
    bool red = false;
    // P1's model is looked for even with no P2 (Red Hulk may be P1); P2's while he is in the game
    const bool have2 = p2 && Position(p2, a);
    if (Enabled() && obj && !g_unloading.load() && (have2 || RedHulkPlayer() == 1)) {
        float o[3];
        for (int i = 0; i < 3; i++) o[i] = F32(obj + 0x60 + 4 * i);
        float d2 = have2 ? (o[0] - a[0]) * (o[0] - a[0]) + (o[1] - a[1]) * (o[1] - a[1]) +
                                     (o[2] - a[2]) * (o[2] - a[2])
                               : 1e9f;
        float d1 = 1e9f;
        const uint32_t mgr = ActorManager();
        if (const uint32_t p1 = mgr ? R32(mgr + kMainActor) : 0; p1 && Position(p1, b))
            d1 = (o[0] - b[0]) * (o[0] - b[0]) + (o[1] - b[1]) * (o[1] - b[1]) + (o[2] - b[2]) * (o[2] - b[2]);
        // hc95: only a skinned Hulk model counts (first part a PolySkin, vtable 0x42E694): after a "new P2"
        // respawn other models at his spot (parts 0x42E764 / 0x42EC7C) matched too, the pick flipped between them
        // every frame and P2 was not drawn
        bool hulk_model = false;
        if ((d2 < 0.75f * 0.75f || d1 < 0.75f * 0.75f) && R32(obj + 0x10) && R32(R32(obj + 0x10)) == 0x42D484) {
            uint32_t pp[kMaxParts];
            hulk_model = Parts(R32(obj + 0x10), pp) > 0 && R32(pp[0]) == 0x42E694;
        }
        if (!hulk_model) d1 = d2 = 1e9f;
        red = d2 < 0.75f * 0.75f && d2 < d1;
        // hc50: each player's own Render::Object (a character model: CompositeDrawable), for the per-player skins
        if (R32(R32(obj + 0x10)) == 0x42D484) {
            if (red) g_p_object[1] = obj;
            else if (d1 < 0.75f * 0.75f && d1 < d2) g_p_object[0] = obj;
        }
        if (d1 < 0.75f * 0.75f && d1 < d2 && R32(obj + 0x10) != g_p1_drawable.load()) {
            g_p1_drawable = R32(obj + 0x10);
            NoteP1Drawable(R32(obj + 0x10));
        }
        static int calls = 0, reported = 0;  // no match yet: the nearest object every 4000 draws, a few times
        static float best = 1e9f;
        static uint32_t best_obj = 0;
        if (d2 < best) best = d2, best_obj = obj;
        if (have2 && ++calls % 4000 == 0 && reported < 5 && !red && !g_p2_drawable.load()) {
            reported++;
            LOGI("coop: Red Hulk search: nearest object to P2 %08X at %.2f m (vtable %08X)", best_obj, std::sqrt(best),
                 R32(best_obj));
            best = 1e9f;
        }
        if (red && R32(obj + 0x10) != g_p2_drawable.load()) {
            g_p2_drawable = R32(obj + 0x10);
            NoteP2Drawable(R32(obj + 0x10));
            LOGI("coop: Red Hulk: P2's object %08X, drawable %08X (vtable %08X, display +0x18 %08X)", obj, R32(obj + 0x10),
                 R32(R32(obj + 0x10)), R32(R32(R32(obj + 0x10)) + 0x18));
            logx::Flush();
        }
    }
    (void)red;
    __imp__sub_001C5460(ctx, base);
}

// The queued drawables are drawn later through their Display, vtable +0x18 (a CompositeDrawable, vtable 0x42D484, draws
// its parts the same way): 0x298B30, and 0x298CB0 for the posed path. P2's is drawn recoloured. (+0x38, hooked in hc24,
// only releases a reference.)
namespace {
template <class F>
void RedHulkDisplay(X86Context& ctx, uint8_t* base, F original, const char* which) {
    const bool red = Enabled() && RedHulkDrawn() == 2 && ctx.ecx && ctx.ecx == g_p2_drawable.load();
    if (red) d3d::SetTint(1.0f);
    original(ctx, base);
    if (red) d3d::SetTint(g_tint_all.load() ? 1.0f : 0.0f);
    static int logged = 0;
    if (red && logged < 2) {
        logged++;
        LOGI("coop: Red Hulk: P2's drawable displayed through %s", which);
        logx::Flush();
    }
}
}  // namespace
X86_EXTERN_FUNC(__imp__sub_002B78C0);
X86_FUNC(sub_002B78C0) { Crumb crumb_("sub_002B78C0");  // PolySkin Display (thiscall, 2 args)
    const uint32_t part = ctx.ecx, a1 = X86_MEM32(ctx.esp + 4), a2 = X86_MEM32(ctx.esp + 8);
    // P2's own part, or a shared part drawn with P2's model's pose (the composite's +0x10) as an argument
    const uint32_t comp = g_p2_drawable.load(), pose = comp ? R32(comp + 0x10) : 0;
    const bool by_pose = pose && (a1 == pose || a2 == pose);
    // a shared part with no pose match: an argument that is a 4x4 transform whose position (+0x30) stands where P2
    // stands, nearer him than P1, is P2's draw
    bool by_place = false;
    const uint32_t p2 = g_p2_actor.load();
    float at2[3], at1[3];
    if (Enabled() && !by_pose && p2 && Position(p2, at2)) {
        const uint32_t am = ActorManager(), p1 = am ? R32(am + kMainActor) : 0;
        const bool have1 = p1 && Position(p1, at1);
        for (const uint32_t m : {a1, a2}) {
            if (m < 0x10000 || m >= 0x10000000) continue;  // not a pointer
            float t[3];
            for (int i = 0; i < 3; i++) t[i] = F32(m + 0x30 + 4 * i);
            if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) continue;
            const auto d2 = [&](const float* q) {
                return (t[0] - q[0]) * (t[0] - q[0]) + (t[1] - q[1]) * (t[1] - q[1]) + (t[2] - q[2]) * (t[2] - q[2]);
            };
            if (d2(at2) < 1.5f * 1.5f && (!have1 || d2(at2) < d2(at1))) by_place = true;
        }
    }
    const bool red = Enabled() && RedHulkDrawn() == 2 && part && (IsP2Part(part) || by_pose || by_place);
    static int seen = 0;  // what a part's draw is given, for the next step if neither test matches
    if (Enabled() && comp && seen < 6 && seen++ % 2 == 0) {
        LOGI("coop: Red Hulk: PolySkin %08X args %08X %08X; P2 model %08X pose %08X -> %s", part, a1, a2, comp, pose,
             red ? "red" : "not P2's");
        logx::Flush();
    }
    if (red) d3d::SetTint(1.0f);
    __imp__sub_002B78C0(ctx, base);
    if (red) d3d::SetTint(g_tint_all.load() ? 1.0f : 0.0f);
    static int logged = 0;
    if (red && logged < 2) {
        logged++;
        LOGI("coop: Red Hulk: P2's part %08X displayed through PolySkin 0x2B78C0", part);
        logx::Flush();
    }
}
// The parts are really drawn by 0x2A41E0 (thiscall part (queue, arg), ret 8; reached from the part's +0x20, 0x2B7900).
// Given a render queue it only queues the part (0x29B750) and the queue draws it later, sorted by material, where
// it can no longer be told apart; given none it draws at once. P2's parts are drawn at once, recoloured.
X86_EXTERN_FUNC(__imp__sub_002A41E0);
X86_FUNC(sub_002A41E0) { Crumb crumb_("sub_002A41E0");
    const uint32_t part = ctx.ecx, queue = X86_MEM32(ctx.esp + 4);
    const int who = Enabled() && part ? RedHulkDrawn() : 0;  // hc30: the mod menu picks P1, P2 or neither
    const bool red = (who == 2 && IsP2Part(part)) || (who == 1 && IsP1Part(part));
    if (!red) {
        __imp__sub_002A41E0(ctx, base);
        return;
    }
    if (queue) W32(ctx.esp + 4, 0);  // draw now instead of queueing
    // hc31 lights (the black Red Hulk): queueing picks the lights near the part (0x29B750: the queue's light record
    // +0x18 gets the part's place, the world matrix's translation from the device's 0x28FAA0, and 0x29FD20 records the
    // lights for the part's light key +0x44), and the opaque pass sets them right before it draws the part (0x2A2AF0
    // -> 0x2A06D0(key)). Drawn at once, neither happened: the part was lit by whatever lights the last model left,
    // sometimes none. Both steps are done here.
    if (queue) {
        const uint32_t rec = R32(queue + 0x18), dev = R32(R32(0x52C2B0));
        if (rec && dev && R32(part + 0x44) == part) {
            const uint32_t m = Call(ctx, dev, 0x28FAA0, {});
            if (m)
                for (int i = 0; i < 3; i++) W32(rec + 0x4C + 4 * i, R32(m + 0x30 + 4 * i));
            Call(ctx, rec, 0x29FD20, {part});
        }
    }
    if (const uint32_t key = R32(part + 0x44)) Call(ctx, 0, 0x2A06D0, {key});
    d3d::SetTint(1.0f);
    __imp__sub_002A41E0(ctx, base);
    d3d::SetTint(g_tint_all.load() ? 1.0f : 0.0f);
    static int logged = 0;
    if (logged < 2) {
        logged++;
        LOGI("coop: Red Hulk: P%d's part %08X drawn at once (queue %08X) with the red tint", who, part, queue);
        logx::Flush();
    }
}
X86_EXTERN_FUNC(__imp__sub_00298B30);
X86_FUNC(sub_00298B30) { Crumb crumb_("sub_00298B30"); RedHulkDisplay(ctx, base, __imp__sub_00298B30, "0x298B30"); }
X86_EXTERN_FUNC(__imp__sub_00298CB0);
X86_FUNC(sub_00298CB0) { Crumb crumb_("sub_00298CB0"); RedHulkDisplay(ctx, base, __imp__sub_00298CB0, "0x298CB0"); }

// P2's sounds: the sound manager plays a character's event (0x1EC980, thiscall manager (character, event name), ret 8)
// through the character's sound emitter (+0x128), made when the character is set up. A P2 without one is silent:
// his events are played through P1's emitter (the two stay close).
X86_EXTERN_FUNC(__imp__sub_001EC980);
X86_FUNC(sub_001EC980) { Crumb crumb_("sub_001EC980");
    const uint32_t chr = X86_MEM32(ctx.esp + 4), p2 = P2Live();
    if (Enabled() && p2 && chr == p2) {
        const uint32_t mgr = ActorManager();
        const uint32_t p1 = mgr ? R32(mgr + kMainActor) : 0;
        static int logged = 0;
        if (logged < 4) {
            logged++;
            const uint32_t name = X86_MEM32(ctx.esp + 8);
            LOGI("coop: P2 sound '%.40s': P2 emitter %08X, P1 emitter %08X", name ? static_cast<const char*>(kernel::HostPtr(name)) : "",
                 R32(p2 + 0x128), p1 ? R32(p1 + 0x128) : 0);
        }
        // neither Hulk has an emitter (both play by name, 0x1EFE20), and P2's are not heard: played as P1's
        if (p1 && !R32(p2 + 0x128)) X86_MEM32(ctx.esp + 4) = p1;
    }
    __imp__sub_001EC980(ctx, base);
}

// Mission markers (the stars on the map, the jump-out circle): the trigger volume manager's update (0x1FC6A0, thiscall
// dt, ret 4) runs its 8 marker sets (+0x234, 0xC70 bytes each) through 0x1FA7D0 (thiscall set (actor, dt), ret 8) for
// the main actor only. A set keeps whether its actor is inside one of its markers (count +0x410, list +0x10; 0x1F9040
// thiscall set (actor, marker), ret 8, is the plain distance test). hc27 ran every set a second time for P2, so each
// frame P1's run said "left" and P2's "entered": a star could never start. hc30: each set has one owner. It changes to
// P2 when P2 stands in one of its markers and P1 does not, and back when P2 leaves; the set's one run is then made for
// the owner (P2 the main actor and his pad "the player's" during it).
namespace {
struct MarkerOwner {
    uint32_t set = 0;
    bool p2 = false;
};
MarkerOwner g_marker_owner[16];
bool InMarker(X86Context& ctx, uint32_t set, uint32_t actor) {
    const uint32_t n = R32(set + 0x410);
    if (n > 256) return false;
    for (uint32_t k = 0; k < n; k++)
        if (const uint32_t m = R32(set + 0x10 + 4 * k); m && (Call(ctx, set, 0x1F9040, {actor, m}) & 0xFF)) return true;
    return false;
}
}  // namespace
X86_EXTERN_FUNC(__imp__sub_001FA7D0);
X86_FUNC(sub_001FA7D0) { Crumb crumb_("sub_001FA7D0");
    const uint32_t set = ctx.ecx, actor = X86_MEM32(ctx.esp + 4), p2 = P2Live();
    const uint32_t am = ActorManager(), p1 = am ? R32(am + kMainActor) : 0;
    if (!Enabled() || !set || !p2 || !p1 || actor != p1 || g_downed[1].load() || R32(set + 0x458) != 2) {
        __imp__sub_001FA7D0(ctx, base);
        return;
    }
    MarkerOwner* o = nullptr;
    for (auto& m : g_marker_owner)
        if (m.set == set) o = &m;
    for (auto& m : g_marker_owner)
        if (!o && !m.set) o = &m;
    if (!o) o = &g_marker_owner[0];
    if (o->set != set) o->set = set, o->p2 = false;
    const bool p2_in = InMarker(ctx, set, p2);
    const bool own2 = o->p2 ? p2_in : (p2_in && !InMarker(ctx, set, p1));
    if (own2 != o->p2) {
        o->p2 = own2;
        static int logged = 0;
        if (logged++ < 60) LOGI("coop: marker set %08X now %s", set, own2 ? "P2's (he stands in it)" : "P1's");
    }
    if (!own2) {
        __imp__sub_001FA7D0(ctx, base);
        return;
    }
    X86_MEM32(ctx.esp + 4) = p2;
    W32(am + kMainActor, p2);
    g_p2_marker_pass = true;  // a star or the circle may wait for a button: "the player's" controls are P2's
    __imp__sub_001FA7D0(ctx, base);
    g_p2_marker_pass = false;
    if (R32(am + kMainActor) == p2) W32(am + kMainActor, p1);
}

// The game's menus, popups and pause screen ("Render Frontend" 0xBDD60, thiscall) are laid out for 4:3 and come out
// stretched on a widescreen picture: they are drawn in its 4:3 middle instead (d3d8.cpp ApplySplit half 3).
X86_EXTERN_FUNC(__imp__sub_000BDD60);
X86_FUNC(sub_000BDD60) { Crumb crumb_("sub_000BDD60");
    const bool narrow = Enabled() && Menus43() && app::Widescreen();
    if (narrow) d3d::SetSplitScreenHalf(3);
    __imp__sub_000BDD60(ctx, base);
    if (narrow) d3d::SetSplitScreenHalf(0);
}

// The ground probe under a character (0x38660, thiscall): it reuses its last answer while the character has moved less
// than a tolerance, 0.01 m for the main actor but 0.4 m for any other, so P2's height stepped every 40 cm and his
// running looked choppy. While P2's probe runs he is the main actor.
X86_EXTERN_FUNC(__imp__sub_00038660);
X86_FUNC(sub_00038660) { Crumb crumb_("sub_00038660");
    const uint32_t chr = ctx.ecx, p2 = P2Live(), am = ActorManager();
    if (!Enabled() || !p2 || chr != p2 || !am || !R32(am + kMainActor)) {
        __imp__sub_00038660(ctx, base);
        return;
    }
    const uint32_t main_was = R32(am + kMainActor);
    W32(am + kMainActor, p2);
    __imp__sub_00038660(ctx, base);
    W32(am + kMainActor, main_was);
}

// ---------------------------------------------------------------------------------------------------------------
// Downed and respawn (the Turok / Force Unleashed co-op rules)
// - A player whose health would reach 0 while his partner is up is held at 1 hp instead: no control, invulnerable,
//   and his half of the screen goes black.
// - After 4 real seconds he comes back beside his partner, facing him, with full health.
// - If the partner is already down, the hit is the game's own: death and the checkpoint restart.
// - A player who falls far below his partner (out of the map) is put back beside him.
// Every damage path ends in the character's health setter, so that is where the hold happens.
namespace {
constexpr uint32_t kSetInvulnA = 0x361C0, kSetInvulnB = 0x36140;  // thiscall (bool): the invulnerability counters
constexpr uint32_t kSetFacing = 0x354E0;                            // thiscall (radians)
constexpr uint32_t kRageThreshold = 0x1B14;
// a downed player's wait, in seconds of running world (a pause or popup does not count)
std::atomic<float> g_down_left[2] = {0, 0};
std::chrono::steady_clock::time_point g_last_down_tick;
// P1 died (both down): the game's death and checkpoint restart run untouched until P1 is back at full health
std::atomic<bool> g_p1_dead{false};
int g_alive_ticks = 0;
std::atomic<uint32_t> g_player[2] = {0, 0};  // P1, P2 actors for the health hook
std::atomic<uint32_t> g_pending_down{0};     // the health hook marked this actor down; the tick finishes it
std::atomic<bool> g_both_down{false};        // P2 fell while P1 was down: P1's hold ends in the game's own death
}  // namespace
// test aids (coop_debug.txt beside the game): F9 knocks the keyboard's player down (0 hp through the health setter),
// F10 drops him 60 m below his partner (the fell rule), F11 knocks both down. Set by the input thread, done in the tick.
std::atomic<int> g_req_down{-1}, g_req_fall{-1};
std::atomic<int> g_req_teleport{-1};  // the mod menu: 1 P2 to P1, 0 P1 to P2
std::atomic<bool> g_req_unlock{false};
std::atomic<int> g_req_survey{-1};  // hc47: the mod menu: log the world pieces near this player
std::atomic<int64_t> g_world_note_until{0};  // hc46: "P2 brought back" on screen until then (CrumbNowMs)  // hc44: the mod menu: unlock every move/upgrade
std::atomic<int> g_req_prop{-1};      // hc44: the mod menu: player * 2 + (0 save the held prop, 1 spawn the saved one)
std::atomic<bool> g_press_b[2] = {false, false};  // hc49: a player pressed B / A in the game (not in the mod menu)
std::atomic<bool> g_press_a[2] = {false, false};
std::atomic<uint32_t> g_stream{0};       // hc49: the world stream manager (seen by the 0x203A10 hook)
// hc49 FRIENDLY FIRE OFF, THROWN PROPS: what each player has held lately (his attached prop slots, the prop's target
// info [obj +0xA0] +0x10 as 0x1D72E0 reads it), so a hit by a prop the partner threw can be refused
struct HeldProp { std::atomic<uint32_t> obj{0}, info{0}; std::atomic<int64_t> ms{0}; };
HeldProp g_held[2][8];

// hc44 SAVED PROP: "Save held object" keeps the type of the prop a player holds (his attached props, 8 slots of 0x50
// bytes at +0x3A8, the ones 0x39440 checks; a prop instance has its type at +0xEC, the type's id at +8, 8 bytes) in
// coop_saved_prop.txt. "Spawn saved object" makes a new one of that type about 30 ft (9 m) in front of the player,
// through the prop manager's spawn (0x1E43B0, thiscall (type id*, instance name id*, matrix*, 1), ret 0x10: what
// spm_SpawnSmartProp ends in); the player picks it up himself.
uint32_t g_saved_prop[2] = {0, 0};
bool g_saved_prop_read = false;
uint32_t g_prop_scratch_spawn = 0;
void ReadSavedProp() {
    if (g_saved_prop_read) return;
    g_saved_prop_read = true;
    if (std::FILE* f = std::fopen("coop_saved_prop.txt", "r")) {
        unsigned a = 0, b = 0;
        if (std::fscanf(f, "%x %x", &a, &b) == 2) g_saved_prop[0] = a, g_saved_prop[1] = b;
        std::fclose(f);
    }
}
// hc44 UNLOCK ALL UPGRADES: the unlockables manager ([app +0x54] +0x3C, 0x2FE entries of 4 bytes; 0x86340, 0x92940)
// holds every move/upgrade; a CUnlockableMove (vtable 0x4044B4) is bought when its byte +0x20 is 1 (what 0x92870 sets
// when one is bought). Moves are game-wide, so both Hulks get them, and the game's own save keeps them. The health /
// critical mass upgrades are added to the bars at the next health reset (respawn, checkpoint, level load).
void UnlockAll() {
    const uint32_t app = R32(kApp), flow = app ? R32(app + 0x54) : 0, mgr = flow ? R32(flow + 0x3C) : 0;
    if (!mgr) {
        LOGI("coop: menu - unlock all: no unlockables manager");
        return;
    }
    uint32_t moves = 0, newly = 0;
    for (uint32_t i = 0; i < 0x2FE; i++) {
        const uint32_t u = R32(mgr + 4 * i);
        if (!u || R32(u) != 0x4044B4) continue;
        moves++;
        if (!kernel::G<uint8_t>(u + 0x20)) kernel::G<uint8_t>(u + 0x20) = 1, newly++;
    }
    LOGI("coop: menu - unlock all: %u moves/upgrades, %u newly unlocked", moves, newly);
}
void PropRequest(X86Context& ctx, int who, bool spawn, uint32_t me) {
    if (!me) return;
    if (!spawn) {
        for (uint32_t i = 0; i < 8; i++) {
            const uint32_t obj = R32(me + 0x3A8 + 0x50 * i);
            const uint32_t type = obj ? R32(obj + 0xEC) : 0;
            if (!type || !(R32(type + 8) | R32(type + 12))) continue;
            g_saved_prop[0] = R32(type + 8), g_saved_prop[1] = R32(type + 12);
            g_saved_prop_read = true;
            if (std::FILE* f = std::fopen("coop_saved_prop.txt", "w")) {
                std::fprintf(f, "%08X %08X\n", g_saved_prop[0], g_saved_prop[1]);
                std::fclose(f);
            }
            LOGI("coop: menu - P%d saved the held prop (slot %u, object %08X, type %08X, id %08X %08X)", who + 1, i, obj,
                 type, g_saved_prop[0], g_saved_prop[1]);
            return;
        }
        LOGI("coop: menu - P%d holds no prop to save (attached actor %08X, slots %08X %08X %08X)", who + 1,
             R32(me + 0x338), R32(me + 0x3A8), R32(me + 0x3F8), R32(me + 0x448));
        return;
    }
    ReadSavedProp();
    const uint32_t mgr = g_prop_manager.load();
    float pos[3];
    const uint32_t body = R32(me + 0x10), xf = body ? R32(body + 0x18) : 0;
    if (!(g_saved_prop[0] | g_saved_prop[1]) || !mgr || !xf || !Position(me, pos)) {
        LOGI("coop: menu - P%d: no saved prop to spawn (or no prop manager %08X)", who + 1, mgr);
        return;
    }
    if (!g_prop_scratch_spawn) g_prop_scratch_spawn = PersistentNew(ctx, 0x60);
    if (!g_prop_scratch_spawn) return;
    // hc54: the way he faces is -Z of the body matrix (hc17, PlaceBeside: SetFacing's +Z row points away from where
    // the character looks); hc44 used +Z, which would have put the object behind him
    float fx = -F32(xf + 0x20), fz = -F32(xf + 0x28);
    const float len = std::sqrt(fx * fx + fz * fz);
    if (!(len > 0.01f)) fx = 0, fz = 1;
    else fx /= len, fz /= len;
    const float at[3] = {pos[0] + 9.0f * fx, pos[1] + 1.5f, pos[2] + 9.0f * fz};
    static uint32_t count = 0;
    const uint32_t g = g_prop_scratch_spawn;
    W32(g, g_saved_prop[0]), W32(g + 4, g_saved_prop[1]);
    W32(g + 8, 0x48554C4Bu ^ ++count), W32(g + 12, 0x434F4F50u);  // a new instance name id each time
    for (uint32_t k = 0; k < 16; k++) W32(g + 0x10 + 4 * k, std::bit_cast<uint32_t>(k % 5 == 0 ? 1.0f : 0.0f));
    for (int k = 0; k < 3; k++) W32(g + 0x10 + 0x30 + 4 * k, std::bit_cast<uint32_t>(at[k]));
    PersistentHeap heap(ctx);
    Call(ctx, mgr, 0x1E43B0, {g, g + 8, g + 0x10, 1});
    LOGI("coop: menu - P%d spawned saved prop %08X %08X at %.0f %.0f %.0f", who + 1, g_saved_prop[0], g_saved_prop[1],
         at[0], at[1], at[2]);
}
std::atomic<bool> g_req_both{false};
namespace {

int PlayerSlot(uint32_t actor) {
    if (!actor) return -1;
    if (actor == g_player[0].load()) return 0;
    if (actor == g_player[1].load()) return 1;
    return -1;
}

void SetInvulnerable(X86Context& ctx, uint32_t actor, bool on) {
    Call(ctx, actor, kSetInvulnA, {on ? 1u : 0u});
    Call(ctx, actor, kSetInvulnB, {on ? 1u : 0u});
}

// Where each player has just been: points at least 1.5 m apart, oldest first. Ground a player has just walked over
// is a safe place to put his partner (not inside a wall, not over a drop).
struct Trail {
    float p[24][3];
    int count = 0;
    void Add(const float v[3]) {
        if (count) {
            const float* l = p[count - 1];
            const float dx = v[0] - l[0], dy = v[1] - l[1], dz = v[2] - l[2];
            if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) return;
        }
        if (count == 24) {
            std::memmove(p[0], p[1], sizeof(p[0]) * 23);
            count = 23;
        }
        std::memcpy(p[count++], v, sizeof(p[0]));
    }
};
Trail g_trail[2];

// beside the partner, facing him: the newest point of his trail 3 to 10 m back, else just above him
void PlaceBeside(X86Context& ctx, int who_slot, uint32_t who, uint32_t partner, const char* why) {
    float p[3], me[3];
    if (!Position(partner, p) || !Strings(ctx)) return;
    const bool had_me = Position(who, me);
    // no trail point far enough back (the partner stood still): 3 m from him, on my side of him (else east)
    float dx0 = had_me ? me[0] - p[0] : 0.0f, dz0 = had_me ? me[2] - p[2] : 0.0f;
    const float d0 = std::sqrt(dx0 * dx0 + dz0 * dz0);
    if (d0 < 0.5f) dx0 = 1.0f, dz0 = 0.0f;
    else dx0 /= d0, dz0 /= d0;
    float v[3] = {p[0] + 3.0f * dx0, p[1] + 0.5f, p[2] + 3.0f * dz0};
    const Trail& t = g_trail[who_slot ^ 1];
    for (int i = t.count - 1; i >= 0; i--) {
        // 3 to 10 m away on the ground (not a point he passed in the air, above where he stands now)
        const float dx = t.p[i][0] - p[0], dy = t.p[i][1] - p[1], dz = t.p[i][2] - p[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 >= 3.0f * 3.0f && d2 <= 10.0f * 10.0f && std::fabs(dy) <= 2.5f) {
            // 2 m to the side of the trail: not straight between the partner and his camera
            const float h = std::sqrt(d2);
            v[0] = t.p[i][0] + 2.0f * dz / h, v[1] = t.p[i][1] + 0.3f, v[2] = t.p[i][2] - 2.0f * dx / h;
            break;
        }
    }
    std::memcpy(kernel::HostPtr(g_strings + 48), v, sizeof(v));
    Call(ctx, who, kSetPosition, {g_strings + 48});
    // SetFacing builds a Y rotation whose +Z row is (sin, 0, cos); a character looks down -Z, so the angle is that
    // of the direction from the partner to the new spot (seen in hc17: atan2(partner - spot) faced him away)
    const float yaw = std::atan2(v[0] - p[0], v[2] - p[2]);
    Call(ctx, who, kSetFacing, {std::bit_cast<uint32_t>(yaw)});
    g_trail[who_slot].count = 0;  // his own trail starts again from here
    LOGI("[RESPAWN] %s %08X beside %08X at %.1f %.1f %.1f (was %.1f %.1f %.1f), facing %.2f rad", why, who, partner,
         v[0], v[1], v[2], had_me ? me[0] : 0.0f, had_me ? me[1] : 0.0f, had_me ? me[2] : 0.0f, yaw);
    logx::Flush();
}

// hc49: each tick, the props in each player's hands (kept 10 s after they leave them: the flight of a throw)
void NoteHeldProps(const uint32_t players[2]) {
    const int64_t now = CrumbNowMs();
    for (int i = 0; i < 2; i++) {
        const uint32_t me = players[i];
        if (!me) continue;
        for (uint32_t k = 0; k < 8; k++) {
            const uint32_t obj = R32(me + 0x3A8 + 0x50 * k);
            if (!obj) continue;
            const uint32_t t = R32(obj + 0xA0), info = t ? R32(t + 0x10) : 0;
            int slot = -1, oldest = 0;
            for (int j = 0; j < 8; j++) {
                if (g_held[i][j].obj.load() == obj) slot = j;
                if (g_held[i][j].ms.load() < g_held[i][oldest].ms.load()) oldest = j;
            }
            if (slot < 0) {
                slot = oldest;
                g_held[i][slot].obj = obj;
            }
            g_held[i][slot].info = info;
            g_held[i][slot].ms = now;
        }
    }
}
// the attacker's target info is a prop that this player held in the last 10 s
bool HeldLately(int who, uint32_t info) {
    const int64_t now = CrumbNowMs();
    const uint32_t owner = R32(info + 8);
    for (int j = 0; j < 8; j++) {
        const HeldProp& h = g_held[who][j];
        if (!h.obj.load() || now - h.ms.load() > 10000) continue;
        if (info == h.info.load() || owner == h.obj.load()) return true;
    }
    return false;
}

// hc49 FRIENDLY FIRE ON: PICK THE PARTNER UP AND THROW HIM (user, 20 Nov). The Hulk's own grab only takes props and
// smaller characters, so this is the mod's own: with friendly fire on, B next to the partner (within 3 m, nothing in
// your hands, nothing grabbed by the game after it) lifts him over your head; he is carried there (his A jumps off);
// B again throws him about 16 m the way you were going (else the way you face).
struct Carry {
    int who = -1;          // the carrier (0 P1, 1 P2), -1 none
    int phase = 0;         // 1 checking the game grabbed nothing, 2 carrying, 3 in the air
    int ticks = 0;
    int gen = -1;
    float start[3] = {}, dir[2] = {0, 1}, last[3] = {}, t = 0;
    float hist[10][3] = {};
    int hist_n = 0;
} g_carry;
std::atomic<float> g_hands_h[2] = {0.0f, 0.0f};  // hc94: the carrier's raised wrists above his root (m)
bool HoldsSomething(uint32_t me) {
    if (R32(me + 0x338) || R32(me + 0x334)) return true;
    for (uint32_t k = 0; k < 8; k++)
        if (R32(me + 0x3A8 + 0x50 * k)) return true;
    return false;
}
void CarryEnd(const char* why) {
    if (g_carry.who >= 0) LOGI("coop: carry - P%d %s", (g_carry.who ^ 1) + 1, why);
    g_carry = Carry{};
}
void CarryPut(X86Context& ctx, uint32_t who, const float v[3]) {
    if (!Strings(ctx)) return;
    std::memcpy(kernel::HostPtr(g_strings + 48), v, 12);
    Call(ctx, who, kSetPosition, {g_strings + 48});
}
void CarryTick(X86Context& ctx, const uint32_t players[2]) {
    bool b[2], a[2];
    for (int i = 0; i < 2; i++) b[i] = g_press_b[i].exchange(false), a[i] = g_press_a[i].exchange(false);
    const bool ok = FriendlyFire() && g_p2_present.load() && players[0] && players[1] && !g_downed[0].load() &&
                    !g_downed[1].load() && !g_p1_dead.load() && !g_unloading.load();
    if (!ok) {
        if (g_carry.who >= 0) CarryEnd("let go (friendly fire off, a player down or the level ending)");
        return;
    }
    if (g_carry.who >= 0 && g_carry.gen != g_unload_gen.load()) CarryEnd("let go (new level)");
    float pos[2][3];
    if (!Position(players[0], pos[0]) || !Position(players[1], pos[1])) return;
    if (g_carry.who < 0) {
        for (int i = 0; i < 2; i++) {
            if (!b[i]) continue;
            const float dx = pos[i][0] - pos[i ^ 1][0], dy = pos[i][1] - pos[i ^ 1][1], dz = pos[i][2] - pos[i ^ 1][2];
            // hc84: 3.5 m (was 3.0: the menu's teleport puts a player exactly 3.0 m away, which missed)
            const bool close_by = dx * dx + dz * dz <= 3.5f * 3.5f && std::fabs(dy) <= 2.5f;
            if (!close_by || HoldsSomething(players[i])) {
                static int told = 0;  // why a B press next to the partner lifted nothing (the first few)
                if (told < 8 && dx * dx + dz * dz < 6.0f * 6.0f) {
                    told++;
                    LOGI("coop: carry - P%d's B: no lift (%.1f m apart, height %.1f, hands +0x334 %08X +0x338 %08X slot0 %08X)",
                         i + 1, std::sqrt(dx * dx + dz * dz), dy, R32(players[i] + 0x334), R32(players[i] + 0x338),
                         R32(players[i] + 0x3A8));
                }
                continue;
            }
            g_carry = Carry{};
            g_carry.who = i, g_carry.phase = 1, g_carry.gen = g_unload_gen.load();
            break;
        }
        return;
    }
    const int c = g_carry.who, o = c ^ 1;
    const uint32_t me = players[c], him = players[o];
    g_carry.ticks++;
    // where the carrier is going: his position 10 ticks ago
    std::memcpy(g_carry.hist[g_carry.hist_n % 10], pos[c], 12);
    g_carry.hist_n++;
    if (g_carry.phase == 1) {  // a few ticks: if the game grabbed something with that B, no carry
        if (HoldsSomething(me)) {
            LOGI("coop: carry - hands +0x334 %08X +0x338 %08X slot0 %08X", R32(me + 0x334), R32(me + 0x338), R32(me + 0x3A8));
            return CarryEnd("not lifted: the game grabbed something else");
        }
        if (g_carry.ticks < 6) return;
        const float dx = pos[c][0] - pos[o][0], dz = pos[c][2] - pos[o][2];
        if (dx * dx + dz * dz > 4.0f * 4.0f) return CarryEnd("not lifted: moved apart");
        g_carry.phase = 2, g_carry.ticks = 0;
        LOGI("coop: carry - P%d lifts P%d", c + 1, o + 1);
    }
    if (g_carry.phase == 2) {
        if (a[o] && g_carry.ticks > 10) {  // the carried player jumps off
            CarryEnd("jumped off");
            PlaceBeside(ctx, o, him, me, "carry: jumped off");
            return;
        }
        // hc94: on his raised hands: the measured hand height (Abomination's are higher), else carry_height
        const float hh = g_hands_h[c].load();
        const float v[3] = {pos[c][0], pos[c][1] + (hh > 1.0f && hh < 8.0f ? hh + 0.1f : Config().carry_height), pos[c][2]};
        // the game moved him away (a cutscene, a warp, the respawn): let go
        if (g_carry.ticks > 2) {
            const float dx = pos[o][0] - g_carry.last[0], dy = pos[o][1] - g_carry.last[1], dz = pos[o][2] - g_carry.last[2];
            if (dx * dx + dy * dy + dz * dz > 12.0f * 12.0f) return CarryEnd("let go (moved by the game)");
        }
        if (b[c] && g_carry.ticks > 10) {  // throw: the way the carrier was going, else the way he faces
            const float* old = g_carry.hist[(g_carry.hist_n) % 10];
            float fx = pos[c][0] - old[0], fz = pos[c][2] - old[2];
            float len = std::sqrt(fx * fx + fz * fz);
            if (len < 0.4f) {
                const uint32_t body = R32(me + 0x10), xf = body ? R32(body + 0x18) : 0;
                fx = xf ? -F32(xf + 0x20) : 0.0f, fz = xf ? -F32(xf + 0x28) : 1.0f;  // a character looks down -Z
                len = std::sqrt(fx * fx + fz * fz);
                if (!(len > 0.01f)) fx = 0, fz = 1, len = 1;
            }
            g_carry.dir[0] = fx / len, g_carry.dir[1] = fz / len;
            std::memcpy(g_carry.start, v, 12);
            g_carry.phase = 3, g_carry.t = 0, g_carry.ticks = 0;
            LOGI("coop: carry - P%d throws P%d (direction %.2f %.2f)", c + 1, o + 1, g_carry.dir[0], g_carry.dir[1]);
            return;
        }
        CarryPut(ctx, him, v);
        std::memcpy(g_carry.last, v, 12);
        return;
    }
    if (g_carry.phase == 3) {  // the flight: 16 m in 0.9 s, 3 m up at the top, then the game's own fall
        static int64_t last_ms = 0;
        const int64_t now = CrumbNowMs();
        const float dt = g_carry.ticks == 1 ? 1.0f / 30 : std::clamp(float(now - last_ms) / 1000.0f, 0.0f, 0.1f);
        last_ms = now;
        g_carry.t += dt;
        const float t = std::min(g_carry.t, 0.9f);
        const float h = 17.8f * t, up = 7.7f * t - 8.5f * t * t;
        const float v[3] = {g_carry.start[0] + g_carry.dir[0] * h, g_carry.start[1] + up, g_carry.start[2] + g_carry.dir[1] * h};
        CarryPut(ctx, him, v);
        if (g_carry.t >= 0.9f) CarryEnd("landed after the throw");
    }
}

// hc50 PER-PLAYER SKINS, switched mid-game (user, 20 Nov; plan in HULK_SKINS_HANDOFF.md). How the game makes a
// Hulk's model: the character asks the render library ([0x4E231C] +0xC, 0x1C5900) for a template by the model's
// 64-bit name key (map +0x128: last index +0x10, entries +0x14; a template is {key 8 bytes, +8 model}); a missing one
// is made from the first model of that name in the world stream's inventory (0x1C4190: [stream +0x44] Find), and each
// actor gets its own clone (CompositeDrawable vtable 0x42D484, Clone vtable +0x10) in a Render::Object (vtable
// 0x4218C8: +0x10 model, +0x18 pose; SetPose 0x1C5010 hands the pose to the model through its vtable +0x28).
// Here: a skin's package art/global_<skin>.p3d is loaded into its own inventory section (as the level load does,
// 0x206320: section 0x266480(0x800), 0x203E90(path, 0, 1, 0, section, 0, 0x11)), the Hulk model is found in that
// section by the key of one of the library's character templates (section vtable +0xC Find(type filter 0x3F6330,
// key)), cloned, and given to the player's Render::Object with his own pose. "Normal" puts the game's model back (kept).
const char* const kSkinFiles[13] = {"grey_hulk",   "joefixit",    "abomination_ashulk", "banner_savage", "hulk_canada",
                                    "hulk_australia", "hulk_us",  "hulk_uk",   "hulk_france", "hulk_german",
                                    "hulk_italy",  "hulk_spain",  "hulk_japan"};
// hc53 joint maps (from the disc, art.rcf global_*.p3d "hulk_skeleton"): kJointMap[to][from][j] = the joint of
// skeleton "from" that joint j of skeleton "to" follows (a joint "from" lacks follows its nearest parent it has).
// Skeletons: 0 Hulk / Grey / Joe Fixit / country Hulks (38), 1 Abomination (38, other order), 2 Savage Banner (37).
constexpr int kJointCount[3] = {38, 38, 37};
constexpr uint8_t kJointMap[3][3][40] = {
    {
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37},
        {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 36, 1},
        // to 0: "Shoulder_Con_L" is not in skeleton 2, follows "Clavicle_L"
        // to 0: "Shoulder_Con_R" is not in skeleton 2, follows "Clavicle_R"
        {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 19, 26, 27, 28, 29, 30, 31, 32, 26, 35, 36, 34, 1},
    },
    {
        {0, 37, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 36, 35},
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37},
        // to 1: "Shoulder_Con_L" is not in skeleton 2, follows "Clavicle_L"
        // to 1: "Shoulder_Con_R" is not in skeleton 2, follows "Clavicle_R"
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 19, 26, 27, 28, 29, 30, 31, 32, 26, 35, 34, 36},
    },
    {
        // to 2: "Wrist_Grapple" is not in skeleton 0, follows "Wrist_R"
        {0, 37, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 27, 28, 29, 30, 31, 32, 30, 36, 34, 35},
        // to 2: "Wrist_Grapple" is not in skeleton 1, follows "Wrist_R"
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31, 32, 33, 31, 36, 35, 37},
        {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36},
    },
};
// hc53: a skin on another skeleton (Abomination, Savage Banner) keeps its clone's own pose (Clone makes one,
// 0x2842F0); just before its mesh is skinned (0x2B7900: part +0x60 pose -> 0x284820 update -> 0x2DFC90 skin
// matrices = skeleton +0x20 inverse binds x pose +0x1C world matrices, 0x40 bytes each) the player's animated pose is
// brought up to date and its world matrices are copied over joint by joint by name (kJointMap). Pose: +8 skeleton
// (+0x28 parent indices), +0xC joints, +0x10 local quats, +0x14 local matrices, +0x1C world matrices, flags +0x20
// (locals from quats) +0x22 +0x23 (worlds dirty).
struct Remap {
    std::atomic<uint32_t> clone{0}, pose{0}, obj{0};
    std::atomic<int> to{0}, from{0};
    std::atomic<bool> real{false};  // hc55 "real model": rotations only, its own bone lengths (rest below)
    std::vector<uint8_t> rest;       // its pose's local matrices as cloned (the skeleton's rest pose)
};
// hc55: the joints whose position (not only rotation) comes from the animation: the roots and the pelvis carry the
// body's movement, jumps and crouches (0 Motion_Root, Balance_Root, Character_Root, Pelvis; 1/2: also Extra_Attributes)
constexpr bool RootJoint(int fam, int j) { return fam == 0 ? j <= 3 : j <= 4; }
Remap g_remap[2];
struct SkinPkg {
    int skin = -1;
    uint32_t sec = 0, model = 0;
    int state = 0;  // 1 loading, 2 ready, 3 failed
    int64_t started = 0, unused_since = 0;  // (unused_since: when the model was found)
};
SkinPkg g_pkgs[13];  // hc82: one slot per package (was 3) so skins_preload_all can keep them all
bool SkinPkgLoaded(int file) {  // hc83: the package is in memory (it switches in a moment, no checkpoint needed)
    for (const SkinPkg& p : g_pkgs)
        if (p.state == 1 || p.state == 2)
            if (p.skin == file) return true;
    return false;
}
struct PlayerModel {
    uint32_t obj = 0, original = 0, clone = 0;
    int applied = -1, gen = -1;
    int64_t seen_ms = 0;
};
PlayerModel g_pm[2];
uint32_t g_skin_scratch = 0;  // +0 the type filter object, +0x10 the path
std::vector<std::pair<uint32_t, int64_t>> g_release_later;  // models to release a few ticks later (not mid-draw)
uint32_t g_stream_seen = 0;
void Release(X86Context& ctx, uint32_t obj) { Call(ctx, obj, R32(R32(obj) + 4), {}); }
// hc57: hc56 still froze (hulk_054: the main loop in the heap allocator 0x26CD90 / 0x26C040, which returns 0 when a
// heap is full, and the game then waits): the Persistent Heap (17) only has room for the one skin package the level
// load keeps there (it frees the old one before loading the next). The extra packages go into the level heap (11, the
// one the location .p3d files are loaded into: "Stream Heap", 0x38E000 bytes, ~1.1 MB free in the church; Persistent
// 0x32F000 had 3 KB free) and are released when the level ends (SkinLevelEnd); the per-player
// clones into the render heap (15, where 0x1C39E0 makes the game's own). Heap table 0x527C60 (16 bytes each, +0 the
// heap; heap +0x494 bytes in use, +0x498 the most ever, +0x490 blocks).
constexpr uint32_t kLevelHeap = 11, kRenderHeap = 15;
struct HeapScope {
    X86Context& ctx;
    uint32_t was = 0xFFFFFFFF;
    HeapScope(X86Context& c, uint32_t index) : ctx(c) {
        if (R32(kHeapTable + index * 16)) was = kernel::CallGuest(ctx, kSetCurrentHeap, {index});
    }
    ~HeapScope() {
        if (was != 0xFFFFFFFF) kernel::CallGuest(ctx, kSetCurrentHeap, {was});
    }
};
// hc59 (hulk_057): heap 11 (Stream) is another kind of allocator: an ordinary allocation in it crashed (operator new
// 0x8D690 -> 0x266E00: [heap] vtable +0xC (size); a heap that is full returns 0 and the game's out-of-memory hook
// [0x527F64] then waits forever, which was the hc55/hc56 freeze in the Persistent Heap). Sizes from the game's heap
// list 0x400520 (12 bytes each: name, size): 17 Persistent 0x32F000 (3 KB free in the church), 11 Stream 0x38E000,
// 15 Rendering 0x64000, 6 Sound 0x12F000, 3 Script 0x96000, 10 Prop 0xD4800. The packages go into a heap of the same
// kind as the Persistent one (same vtable) with the most room left, never the Prop Heap (its pool froze the game).
uint32_t HeapSize(uint32_t i) { return i < 19 ? R32(0x400520 + 12 * i + 4) : 0; }
// hc70: the heap table entries are also a tree (16 bytes: heap, first child, next sibling, parent; root 0x527F40 =
// entry 45; free 0x267090 walks it with 0x266F50 to find a block's heap). hc69 (hulk_066) crashed in that walk on a
// node pointer of 1 during our load. Every entry is checked: its pointers must be 0 or inside the table.
bool HeapNodeOk(uint32_t v) { return !v || (v >= kHeapTable && v <= 0x527F40 && (v - kHeapTable) % 16 == 0); }
void LogHeapTree(const char* when) {
    if (!Config().skins_diag) return;  // hc80
    char line[1400];
    size_t used = 0;
    int bad = 0;
    for (uint32_t i = 0; i <= 45 && used + 80 < sizeof(line); i++) {
        const uint32_t e = kHeapTable + 16 * i, h = R32(e), c = R32(e + 4), n = R32(e + 8), par = R32(e + 12);
        if (!h && !c && !n && !par) continue;
        const bool ok = HeapNodeOk(c) && HeapNodeOk(n) && HeapNodeOk(par);
        bad += !ok;
        used += std::snprintf(line + used, sizeof(line) - used, " %u:%X/c%X/n%X/p%X%s", i, h, c ? (c - kHeapTable) / 16 : 0,
                              n ? (n - kHeapTable) / 16 : 0, par ? (par - kHeapTable) / 16 : 0, ok ? "" : "!BAD");
    }
    LOGI("coop: skins - heap tree %s (%d bad; index:heap/child/next/parent):%s", when, bad, line);
    logx::Flush();
}
void LogHeaps(const char* when) {
    if (!Config().skins_diag) return;  // hc80
    char line[900];
    size_t used = 0;
    for (uint32_t i = 0; i < 24 && used + 64 < sizeof(line); i++) {
        const uint32_t h = R32(kHeapTable + i * 16);
        if (!h) continue;
        used += std::snprintf(line + used, sizeof(line) - used, " %u:%uK/%uK/%uK vt%X", i, R32(h + 0x494) >> 10,
                              R32(h + 0x498) >> 10, HeapSize(i) >> 10, R32(h));
    }
    LOGI("coop: skins - heaps %s (index: in use / most / size, vtable):%s", when, line);
}
// hc71: the heap for a skin package. hc70 (hulk_067): the table is a tree; heap 11 (Stream) has child heaps 19, 20 and
// heap 7 (Room) 21-40 carved out of their memory, so a parent's "free" room is its children's: loading into 11
// corrupted them. A heap object holds its range (+0xC start, +0x10 end) and size (+0x14; room = +0x14 - +0x494). The
// package goes into a leaf heap of the Persistent Heap's kind with >= 300 KB room whose range holds no other heap.
// hc72: every heap but the Persistent Heap is reset or rebuilt while a level loads (hc57-71 put the packages in
// Stream, Sound, Effect... and the level load later reused that memory: garbage objects, a crash in a dynamic_cast
// during the next allocation). The game itself loads its one skin into the Persistent Heap (0x206320: heap 0x11),
// which has ~3K spare. So at boot (0x8D3A0 builds heap i from the static list 0x400520 {name, size, extra}) the
// Persistent Heap is made bigger by room for the players' packages (~200K files each); the arena comes from the
// system allocator (guest user memory, not the 64MB physical pool), so this costs nothing else.
constexpr uint32_t kSkinHeapExtra = 0x200000;
X86_EXTERN_FUNC(__imp__sub_0008D3A0);
X86_FUNC(sub_0008D3A0) {
    const uint32_t index = X86_MEM32(ctx.esp + 4), entry = 0x400520 + 12 * kPersistentHeap;
    if (index == kPersistentHeap && Enabled() && Config().skins_test && R32(entry + 4) == 0x32F000) {
        W32(entry + 4, 0x32F000 + kSkinHeapExtra);
        LOGI("coop: skins - Persistent Heap made %uK bigger for the players' skins", kSkinHeapExtra / 1024);
    }
    __imp__sub_0008D3A0(ctx, base);
}
uint32_t SkinHeap() {
    if (const uint32_t ph = R32(kHeapTable + kPersistentHeap * 16); ph && R32(0x400520 + 12 * kPersistentHeap + 4) > 0x32F000) {
        const uint32_t size = R32(ph + 0x14), in_use = R32(ph + 0x494);
        LOGI("coop: skins - Persistent Heap: size %uK in use %uK", size / 1024, in_use / 1024);
        return size > in_use + 300 * 1024 ? kPersistentHeap : 0xFFFFFFFF;
    }
    return 0xFFFFFFFF;
}
uint32_t SkinHeapOld() {
    const uint32_t ph = R32(kHeapTable + kPersistentHeap * 16), pvt = ph ? R32(ph) : 0;
    uint32_t best = 0xFFFFFFFF, best_free = 300 * 1024;
    for (uint32_t i = 0; i < 45; i++) {
        const uint32_t e = kHeapTable + 16 * i, h = R32(e);
        if (!h || h == 0x527F50 || !pvt || R32(h) != pvt || i == 10 || i == kPersistentHeap || R32(e + 4)) continue;
        const uint32_t lo = R32(h + 0xC), hi = R32(h + 0x10), size = R32(h + 0x14), in_use = R32(h + 0x494);
        if (hi <= lo || size <= in_use) continue;
        bool overlaps = false;
        for (uint32_t j = 0; j < 45 && !overlaps; j++) {
            const uint32_t g = R32(kHeapTable + 16 * j);
            if (j == i || !g || g == 0x527F50 || R32(g) != pvt) continue;
            const uint32_t glo = R32(g + 0xC);
            overlaps = glo >= lo && glo < hi;
        }
        LOGI("coop: skins - heap %u: %08X-%08X size %uK in use %uK%s", i, lo, hi, size >> 10, in_use >> 10,
             overlaps ? " (holds another heap)" : "");
        if (!overlaps && size - in_use > best_free) best = i, best_free = size - in_use;
    }
    return best;
}
// hc66: the game's loader cannot be run in play (hc55-hc65: from the tick, the frame start, in heaps 17, 11 and 6 it
// froze or crashed every time). The packages are loaded during the level/checkpoint load instead (SkinPreload, after
// the game's own skin, the same call in the same place), for the skins the players have picked then. A skin picked
// later comes with the next checkpoint; one already loaded switches at once.
std::atomic<int> g_skin_load_slot{-1};   // (unused since hc66)
std::atomic<bool> g_skin_loading{false};  // a load is running (frames run inside it: the skin code waits)
std::atomic<uint32_t> g_level_epoch{0};  // hc81: counts level / checkpoint loads
bool StartPackage(X86Context& ctx, SkinPkg& p, int skin) {
    (void)ctx, (void)p;
    // hc81: once per skin until a level load (two players wanting two unloaded skins logged every frame in hc80)
    static uint32_t told = 0;
    static uint32_t told_epoch = 0;
    if (told_epoch != g_level_epoch.load()) told_epoch = g_level_epoch.load(), told = 0;
    if (skin >= 0 && skin < 32 && !(told & (1u << skin))) {
        told |= 1u << skin;
        LOGI("coop: skins - %s is not loaded: it comes with the next checkpoint / level load", kSkinFiles[skin]);
    }
    return false;
}
// During the level load (0x206320, after the game's own skin package): the packages of the skins the players have
// picked, into the level's Stream Heap (11, the one the location files are loaded into right then), with the call
// the game uses (0x203E90(path, 0, 1, 0, section, 0, heap): it waits for the file). Released at the level's end.
// hc76: how SkinPreload issues its load: set by the 0x203E90 hook, which re-runs the game's own load call in place
// (same guest stack, no CallGuest) - every load made through CallGuest crashed the loader fiber (hc57-75)
uint32_t (*g_issue_load)(X86Context& ctx, uint32_t path, uint32_t sec, uint32_t heap) = nullptr;
void SkinPreload(X86Context& ctx, uint32_t stream) {
    static bool inside = false;
    if (!stream || inside) return;
    struct In { In() { inside = true; } ~In() { inside = false; } } in_;
    g_stream = stream;
    // hc82: the players' skins first, then (skins_preload_all) every other package
    int files[2 + 13], nfiles = 0;
    for (int who = 0; who < 2; who++)
        if (PlayerSkin(who) >= 0) files[nfiles++] = SkinFile(PlayerSkin(who));
    if (Config().skins_preload_all)
        for (int f = 0; f < 13; f++) files[nfiles++] = f;
    for (int fi = 0; fi < nfiles; fi++) {
        const int file = files[fi];
        bool have = false;
        for (SkinPkg& p : g_pkgs) have |= p.state && p.skin == file;
        if (have) continue;
        SkinPkg* slot = nullptr;
        for (SkinPkg& p : g_pkgs)
            if (!p.state) { slot = &p; break; }
        if (!slot) break;
        if (!g_skin_scratch) {
            PersistentHeap heap(ctx);
            g_skin_scratch = GuestNew(ctx, 0x60);
        }
        if (!g_skin_scratch) return;
        char path[64];
        std::snprintf(path, sizeof(path), "art/global_%s.p3d", kSkinFiles[file]);
        const int64_t t0 = CrumbNowMs();
        uint32_t sec = 0, req = 0;
        LogHeapTree("before our section");
        const uint32_t hi = SkinHeap();
        if (hi == 0xFFFFFFFF) {
            LOGI("coop: skins - no heap with room for %s", path);
            logx::Flush();
            return;
        }
        {
            HeapScope heap(ctx, hi);
            sec = GuestNew(ctx, 0x14);
            if (!sec) return;
            Call(ctx, sec, 0x266480, {0x800});
            AddRef(ctx, sec);
            std::memcpy(kernel::HostPtr(g_skin_scratch + 0x10), path, std::strlen(path) + 1);
            LOGI("coop: skins - level load: loading %s into heap %u, section %08X", path, hi, sec);
            logx::Flush();
            LogHeapTree("section made, before the load");
            if (Config().skins_diag) {  // hc80: diagnostic only
                char w[400] = {};
                int k = 0;
                for (uint32_t i = 0; i < 16; i++) k += std::snprintf(w + k, sizeof(w) - k, " %08X", R32(0x527C28 + 4 * i));
                const uint32_t lr = R32(0x527C2C);
                LOGI("coop: skins - esp %08X fs %08X [fs] %08X [fs+4] %08X [fs+8] %08X; task 527C28:%s; loader esp? %08X",
                     ctx.esp, ctx.fs_base, R32(ctx.fs_base), R32(ctx.fs_base + 4), R32(ctx.fs_base + 8), w,
                     lr ? R32(lr) : 0);
                logx::Flush();
            }
            g_skin_loading = true;
            req = g_issue_load ? g_issue_load(ctx, g_skin_scratch + 0x10, sec, hi)
                               : Call(ctx, stream, 0x203E90, {g_skin_scratch + 0x10, 0, 1, 0, sec, 0, hi});
            g_skin_loading = false;
            if (req) Call(ctx, req, R32(R32(req) + 4), {});
        }
        *slot = SkinPkg{};
        slot->skin = file, slot->sec = sec, slot->state = 1, slot->started = CrumbNowMs();
        LOGI("coop: skins - %s loaded in %lld ms", path, static_cast<long long>(CrumbNowMs() - t0));
        LogHeaps("after");
        logx::Flush();
    }
}
// the Hulk model in a loaded package: the first character model of the library's templates that this section has
uint32_t FindModel(X86Context& ctx, uint32_t sec) {
    const uint32_t rm = R32(0x4E231C), lib = rm ? R32(rm + 0xC) : 0, map = lib ? lib + 0x128 : 0;
    if (!map || !g_skin_scratch) return 0;
    const uint32_t n = std::min(R32(map + 0x10) + 1, 1000u), arr = R32(map + 0x14);
    W32(g_skin_scratch, 0x3F6330);  // the CompositeDrawable type filter (vtable [0] 0x46280: a dynamic cast)
    for (uint32_t i = 0; i < n && arr; i++) {
        const uint32_t t = R32(arr + 4 * i), proto = t ? R32(t + 8) : 0;
        if (!proto || R32(proto) != 0x42D484) continue;
        const uint32_t m = Call(ctx, sec, R32(R32(sec) + 0xC), {g_skin_scratch, R32(t), R32(t + 4)});
        if (m && R32(m) == 0x42D484) {
            uint32_t parts[kMaxParts];
            LOGI("coop: skins - model %08X (%d parts) found under key %08X %08X (the level's model %08X, %d parts)", m,
                 Parts(m, parts), R32(t), R32(t + 4), proto, Parts(proto, parts));
            return m;
        }
    }
    return 0;
}
void Install(X86Context& ctx, PlayerModel& pm, uint32_t model, bool player_pose = true) {
    AddRef(ctx, model);
    const uint32_t old = R32(pm.obj + 0x10);
    W32(pm.obj + 0x10, model);
    if (const uint32_t pose = R32(pm.obj + 0x18); pose && player_pose) Call(ctx, model, R32(R32(model) + 0x28), {pose});
    if (old && old != pm.original) g_release_later.push_back({old, CrumbNowMs() + 300});
    else if (old == pm.original) Release(ctx, old);  // the object's own reference (ours keeps it)
}
void SkinTick(X86Context& ctx, const uint32_t players[2]) {
    static bool inside = false;  // hc56: never inside itself (a game call below could run the main loop)
    if (inside || g_skin_loading.load()) return;
    struct Inside { Inside() { inside = true; } ~Inside() { inside = false; } } inside_;
    const int64_t now = CrumbNowMs();
    for (size_t i = 0; i < g_release_later.size();) {
        if (now < g_release_later[i].second) { i++; continue; }
        Release(ctx, g_release_later[i].first);
        g_release_later.erase(g_release_later.begin() + i);
    }
    // a level ended: every object and model of the old one is gone (the packages stay, Persistent Heap)
    static int gen = -1;
    if (gen != g_unload_gen.load()) {
        gen = g_unload_gen.load();
        for (PlayerModel& pm : g_pm) pm = PlayerModel{};
        g_remap[0].clone = g_remap[1].clone = 0;
        g_p_object[0] = g_p_object[1] = 0;
        g_release_later.clear();
    }
    const bool settled = now - g_pskin_changed_ms.load() > 700;  // scrolling through the list loads nothing
    for (int who = 0; who < 2; who++) {
        PlayerModel& pm = g_pm[who];
        const uint32_t obj = g_p_object[who].load();
        const int want = PlayerSkin(who);
        {  // why nothing happens (every 5 s while a skin is wanted and not on)
            static int64_t told[2] = {0, 0};
            if (want != pm.applied && now - told[who] > 5000) {
                told[who] = now;
                LOGI("coop: skins - P%d wants %d (on: %d): actor %08X object %08X (vtable %08X, model vtable %08X), "
                     "kept object %08X, settled %d", who + 1, want, pm.applied, players[who], obj, obj ? R32(obj) : 0,
                     obj ? R32(R32(obj + 0x10)) : 0, pm.obj, int(settled));
            }
        }
        if (!players[who] || !obj || R32(obj) != 0x4218C8 || R32(R32(obj + 0x10)) != 0x42D484) continue;
        // another object at his place (a soldier he holds, one he stands on) while his own one still exists: not his
        if (pm.obj && obj != pm.obj && R32(pm.obj) == 0x4218C8 &&
            (R32(pm.obj + 0x10) == pm.original || (pm.clone && R32(pm.obj + 0x10) == pm.clone)))
            continue;
        if (obj != pm.obj) {  // a new model object for this player (level load, checkpoint): the game's model is his own
            if (pm.obj && pm.original) LOGI("coop: skins - P%d has a new model object %08X (was %08X)", who + 1, obj, pm.obj);
            pm = PlayerModel{};
            g_remap[who].clone = 0;
            pm.obj = obj, pm.original = R32(obj + 0x10), pm.seen_ms = now;
            AddRef(ctx, pm.original);  // kept for "Normal"
        }
        if (now - pm.seen_ms < 1000 || !settled || want == pm.applied) continue;
        if (want < 0) {
            g_remap[who].clone = 0;
            Install(ctx, pm, pm.original);
            pm.clone = 0, pm.applied = -1;
            LOGI("coop: skins - P%d back to the level's model", who + 1);
            continue;
        }
        SkinPkg* pkg = nullptr;
        for (SkinPkg& p : g_pkgs)
            if (p.state && p.skin == SkinFile(want)) pkg = &p;
        if (!pkg) {  // a free slot: never loaded, or a package nobody uses (released first)
            for (SkinPkg& p : g_pkgs) {
                const bool used = (p.skin == SkinFile(PlayerSkin(0)) || p.skin == SkinFile(PlayerSkin(1)) ||
                                   p.skin == SkinFile(g_pm[0].applied) || p.skin == SkinFile(g_pm[1].applied));
                if (!p.state || (p.state != 1 && !used)) { pkg = &p; break; }
            }
            if (!pkg) continue;
            if (pkg->state && pkg->sec) {
                LOGI("coop: skins - releasing the %s package", kSkinFiles[pkg->skin]);
                Release(ctx, pkg->sec);
            }
            if (!StartPackage(ctx, *pkg, SkinFile(want))) { *pkg = SkinPkg{}; continue; }
        }
        if (pkg->state == 1) {
            static int64_t next_try = 0;
            if (!pkg->sec || now < next_try) continue;  // not loaded yet (the next frame's start loads it)
            next_try = now + 500;
            if (pkg->model) {  // found last time: one more second for the rest of the package
                if (now - pkg->unused_since > 1000) pkg->state = 2;
            } else if (const uint32_t m = FindModel(ctx, pkg->sec)) pkg->model = m, pkg->unused_since = now;
            else if (now - pkg->started > 30000) {
                pkg->state = 3;
                LOGI("coop: skins - %s: no Hulk model found in 30 s (out of memory flag %u)", kSkinFiles[pkg->skin],
                     R32(0x527F64));
            }
            continue;
        }
        if (pkg->state == 3) {
            if (pm.applied != want) LOGI("coop: skins - P%d: %s could not be used", who + 1, kSkinFiles[SkinFile(want)]);
            pm.applied = want;  // not again until he picks another
            continue;
        }
        uint32_t clone = 0;
        {
            HeapScope heap(ctx, kRenderHeap);
            clone = Call(ctx, pkg->model, R32(R32(pkg->model) + 0x10), {});
        }
        if (!clone || R32(clone) != 0x42D484) {
            LOGI("coop: skins - P%d: cloning %08X failed", who + 1, pkg->model);
            pm.applied = want;
            continue;
        }
        // the player's pose drives the new model: it has to be for a skeleton with the same joints in the same order
        // (pose +8 skeleton, +0xC joint count; the clone brings its package's own pose at +0x40). From the disc
        // (art.rcf, 20 Nov): every package's model is "hulk" on "hulk_skeleton"; 38 joints in one order for the Hulk,
        // Grey Hulk, Joe Fixit and the nine country Hulks; Abomination has the same 38 in another order, Savage Banner
        // 37 in another. The players' pose is for the level's own skin.
        const auto family = [](int skin) { return SkinFile(skin) == 2 ? 1 : SkinFile(skin) == 3 ? 2 : 0; };
        int level_family = 0;  // the skin the level loaded: the stream manager's +0xAC4 (0x206320)
        char loaded[33] = {};
        if (const uint32_t st = g_stream.load()) {
            std::memcpy(loaded, kernel::HostPtr(st + 0xAC4), 32);
            if (!std::strcmp(loaded, "abomination_ashulk")) level_family = 1;
            else if (!std::strcmp(loaded, "banner_savage")) level_family = 2;
        }
        const uint32_t mine = R32(obj + 0x18), theirs = R32(clone + 0x40);
        const int to = family(want), from = level_family;
        const bool remap = to != from;
        if (!mine || !theirs || R32(theirs + 0xC) != uint32_t(kJointCount[to]) || R32(mine + 0xC) != uint32_t(kJointCount[from])) {
            LOGI("coop: skins - P%d: %s's skeleton (%u joints) is not the level Hulk's (%u joints, %s): not used",
                 who + 1, kSkinNames[want], theirs ? R32(theirs + 0xC) : 0, mine ? R32(mine + 0xC) : 0, loaded);
            g_release_later.push_back({clone, now});
            AddRef(ctx, clone);  // (the release above is the clone's only one)
            pm.applied = want;
            continue;
        }
        LOGI("coop: skins - P%d: pose %08X skeleton %08X %u joints; %s's own pose %08X skeleton %08X %u joints", who + 1, mine,
             mine ? R32(mine + 8) : 0, mine ? R32(mine + 0xC) : 0, kSkinNames[want], theirs, theirs ? R32(theirs + 8) : 0,
             theirs ? R32(theirs + 0xC) : 0);
        g_remap[who].clone = 0;
        if (remap) {  // its own pose, fed from the player's every frame (the 0x2B7900 hook)
            AddRef(ctx, theirs);  // kept while the clone is his (never released: a few KB)
            g_remap[who].pose = theirs, g_remap[who].obj = obj, g_remap[who].to = to, g_remap[who].from = from;
            g_remap[who].real = want >= 13;
            g_remap[who].rest.resize(0x40 * size_t(kJointCount[to]));
            if (const uint32_t locals = R32(theirs + 0x14))
                std::memcpy(g_remap[who].rest.data(), kernel::HostPtr(locals), g_remap[who].rest.size());
            g_remap[who].clone = clone;
            LOGI("coop: skins - P%d: %s's joints follow his animation by name (skeleton %d from %d, %s)", who + 1,
                 kSkinNames[want], to, from, want >= 13 ? "own bone lengths" : "the Hulk's shape");
        }
        Install(ctx, pm, clone, !remap);
        pm.clone = clone, pm.applied = want;
        LOGI("coop: skins - P%d is now %s (model %08X, object %08X, pose %08X)", who + 1, kSkinNames[want], clone, obj,
             R32(obj + 0x18));
        logx::Flush();
    }
}

void DownedTick(const hulk::cheats::CheatState& state, X86Context& ctx, uint32_t p1, uint32_t p2) {
    const uint32_t players[2] = {p1, p2};
    g_player[0] = p1;
    g_player[1] = p2;
    NoteHeldProps(players);
    SkinTick(ctx, players);
    CarryTick(ctx, players);
    if (const int who = g_req_down.exchange(-1); who >= 0 && !g_downed[who].load()) {
        LOGI("coop: test - P%d knocked down", who + 1);
        Call(ctx, players[who], kSetHealth, {0, 1});
    }
    if (g_req_both.exchange(false)) {
        LOGI("coop: test - both knocked down");
        if (!g_downed[1].load()) Call(ctx, p2, kSetHealth, {0, 1});
        if (!g_downed[0].load()) Call(ctx, p1, kSetHealth, {0, 1});
    }
    if (const int who = g_req_fall.exchange(-1); who >= 0 && Strings(ctx)) {
        float v[3];
        if (Position(players[who ^ 1], v)) {
            v[1] -= 60.0f;
            std::memcpy(kernel::HostPtr(g_strings + 48), v, sizeof(v));
            Call(ctx, players[who], kSetPosition, {g_strings + 48});
            LOGI("coop: test - P%d dropped to %.1f %.1f %.1f", who + 1, v[0], v[1], v[2]);
        }
    }
    if (const int who = g_req_teleport.exchange(-1); who >= 0) {  // the mod menu: who goes to his partner
        if (g_downed[0].load() || g_downed[1].load()) LOGI("coop: menu teleport skipped, a player is down");
        else PlaceBeside(ctx, who, players[who], players[who ^ 1], who ? "menu: P2 to P1" : "menu: P1 to P2");
    }
    if (g_req_unlock.exchange(false)) UnlockAll();
    // hc47 SURVEY: every loaded prop-table entry within 250 m of the player, with what might tell a destructible
    // skyscraper apart (size class, flags, hit points, object +0x150 category, its type id), nearest first
    if (const int who = g_req_survey.exchange(-1); who >= 0) {
        const uint32_t mgr = g_prop_manager.load(), me = players[who];
        float mp[3];
        if (mgr && me && Position(me, mp)) {
            struct Row { float d; uint32_t i; };
            std::vector<Row> rows;
            const uint32_t count = std::min(R32(mgr + 0x1E614), 2048u);
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t e = mgr + 0x14608 + 0x14 * i;
                const float dx = F32(e) - mp[0], dz = F32(e + 8) - mp[2], d = std::sqrt(dx * dx + dz * dz);
                if (d < 250.0f) rows.push_back({d, i});
            }
            std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
            LOGI("coop: survey by P%d at %.0f %.0f %.0f: %zu pieces within 250 m", who + 1, mp[0], mp[1], mp[2],
                 rows.size());
            int shown = 0;
            for (const Row& w : rows) {
                if (shown++ >= 60) break;
                const uint32_t e = mgr + 0x14608 + 0x14 * w.i;
                const uint16_t flags = kernel::G<uint16_t>(e + 0x10);
                const int16_t id = kernel::G<int16_t>(e + 0x12);
                const uint32_t rec = (id >= 0 && id < 0x800) ? mgr + 0x608 + 0x28 * uint32_t(id) : 0;
                const uint32_t obj = rec ? R32(rec + 0x18) : 0, type = obj ? R32(obj + 0xEC) : 0;
                LOGI("coop:   %5.0f m  y %5.0f  flags %04X class %u  rec %08X %08X  obj %08X hp %08X cat %u  type "
                     "%08X %08X",
                     w.d, F32(e + 4), flags, (flags >> 10) & 3, rec ? R32(rec) : 0, rec ? R32(rec + 4) : 0, obj,
                     obj ? R32(obj + 0x108) : 0, obj ? R32(obj + 0x150) : 0, type ? R32(type + 8) : 0,
                     type ? R32(type + 12) : 0);
            }
            logx::Flush();
        }
    }
    // hc46 WORLD LIMIT (user: "automatically teleport him close to me when I get to the 100"): the world pieces the
    // game can keep loaded (cap 100, world_cap) fill up when the players are far apart, and then the ground under P2
    // goes. When the pool is full and P2 is more than 250 m from P1 for 2 s, P2 is put beside P1 (on-screen note).
    {
        static int64_t full_since = 0;
        const uint32_t mgr = g_prop_manager.load();
        float a[3], b[3];
        bool over = false;
        if (mgr && !g_downed[0].load() && !g_downed[1].load() && Position(p1, a) && Position(p2, b)) {
            const uint32_t loaded = R32(mgr + 0x1E820), cap = R32(mgr + 0x1EEE4);
            const float dx = a[0] - b[0], dz = a[2] - b[2];
            over = cap && loaded + 1 >= cap && dx * dx + dz * dz > 250.0f * 250.0f;
            const int64_t now = CrumbNowMs();
            if (!over) full_since = 0;
            else if (!full_since) full_since = now;
            else if (now - full_since > 2000) {
                LOGI("coop: world limit: %u of %u pieces loaded, P2 %.0f m from P1: P2 brought to P1", loaded, cap,
                     std::sqrt(dx * dx + dz * dz));
                PlaceBeside(ctx, 1, p2, p1, "world limit");
                g_world_note_until = now + 2500;
                full_since = 0;
            }
        }
    }
    if (const int req = g_req_prop.exchange(-1); req >= 0) PropRequest(ctx, req >> 1, req & 1, players[req >> 1]);
    if (const uint32_t a = g_pending_down.exchange(0)) {
        const int slot = PlayerSlot(a);
        if (slot >= 0) {
            SetInvulnerable(ctx, a, true);
            LOGI("[DOWN] P%d %08X held at 1 hp, partner %s", slot + 1, a, g_downed[slot ^ 1].load() ? "down" : "up");
        }
    }
    if (g_both_down.exchange(false) && g_downed[0].load()) {
        LOGI("[DOWN] both players down: P1's death, the game's checkpoint restart");
        g_downed[0] = false;
        g_downed[1] = false;
        g_p1_dead = true;
        SetInvulnerable(ctx, p1, false);
        SetInvulnerable(ctx, p2, false);
        Call(ctx, p1, kSetHealth, {0, 1});
        logx::Flush();
        return;
    }
    if (g_p1_dead.load()) {  // the death runs; the rules come back once P1 has had his health for a second
        g_alive_ticks = F32(p1 + kHealth) >= 1.0f ? g_alive_ticks + 1 : 0;
        if (g_alive_ticks < 60) return;
        g_p1_dead = false;
        g_alive_ticks = 0;
        g_trail[0].count = g_trail[1].count = 0;
        const float max = F32(p2 + kMaxHealth);
        if (max > 0) Call(ctx, p2, kSetHealth, {std::bit_cast<uint32_t>(max), 1});
        LOGI("coop: P1 is back after his death, co-op rules on");
        logx::Flush();
    }
    // hc44: friendly fire off: a player whose target (+0x328 target info: +0xC type 1 = character, +8 actor) is the
    // partner, however it was set, has it dropped (released and cleared, as 0x3941D does)
    if (!FriendlyFire()) {
        for (int i = 0; i < 2; i++) {
            const uint32_t me = players[i], partner = players[i ^ 1];
            const uint32_t info = me ? R32(me + 0x328) : 0;
            if (info && partner && R32(info + 0xC) == 1 && R32(info + 8) == partner) {
                Call(ctx, info, R32(R32(info) + 4), {});
                W32(me + 0x328, 0);
                static uint32_t n = 0;
                if (++n % 100 == 1) LOGI("coop: friendly fire off: P%d's target on the partner dropped (%u times)", i + 1, n);
            }
        }
    }
    // the game moved P1 far in one tick (a cutscene, a mission start, a script teleport): with follow_warps on, P2
    // joins him (off by default: no automatic teleports unless the player asks for them in the mod menu)
    {
        static float last1[3];
        static uint32_t last_p1 = 0;
        static int last_gen = -1;
        if (last_gen != g_unload_gen.load()) {  // hc40: a new level: no "warp" from where P1 was in the last one
            last_gen = g_unload_gen.load();
            last_p1 = 0;
        }
        float now1[3];
        if (Position(p1, now1)) {
            if (last_p1 == p1) {
                const float dx = now1[0] - last1[0], dy = now1[1] - last1[1], dz = now1[2] - last1[2];
                if (dx * dx + dy * dy + dz * dz > 30.0f * 30.0f) {
                    LOGI("coop: P1 warped %.0f m", std::sqrt(dx * dx + dy * dy + dz * dz));
                    g_trail[0].count = 0;
                    // hc42: a game teleport of P1 across the map (the trip to the city: 1678 m in one tick, hulk_046)
                    // always takes P2 along: no jump moves 300 m in 1/30 s, and P2 left there stands where the world
                    // isn't loaded (no ground, buildings he falls through). Shorter warps only with follow_warps.
                    const bool long_warp = dx * dx + dy * dy + dz * dz > 300.0f * 300.0f;
                    if ((long_warp || FollowWarps()) && !g_downed[1].load()) PlaceBeside(ctx, 1, p2, p1, "P1 warped");
                }
            }
            std::memcpy(last1, now1, sizeof(last1));
            last_p1 = p1;
        }
    }
    for (int i = 0; i < 2; i++) {
        float pos[3];
        if (!g_downed[i].load() && Position(players[i], pos)) g_trail[i].Add(pos);
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - g_last_down_tick).count(), 0.0f, 0.1f);
    g_last_down_tick = now;
    for (int i = 0; i < 2; i++) {
        const uint32_t me = players[i], partner = players[i ^ 1];
        if (g_downed[i].load()) {
            if (!g_downed[i ^ 1].load()) g_down_left[i] = std::max(0.0f, g_down_left[i].load() - dt);
            if (g_down_left[i].load() > 0 || g_downed[i ^ 1].load()) continue;
            g_downed[i] = false;  // before the setter: the hold lets the full health through
            PlaceBeside(ctx, i, me, partner, i ? "P2" : "P1");
            const float max = F32(me + kMaxHealth);
            Call(ctx, me, kSetHealth, {std::bit_cast<uint32_t>(max > 0 ? max : 100.0f), 1});
            SetInvulnerable(ctx, me, false);
            LOGI("[RESPAWN] P%d health %.0f", i + 1, F32(me + kHealth));
            continue;
        }
        // fell out of the map (only with fall_rescue on; there is no automatic tether)
        static unsigned checks = 0;
        if (FallRescue() && ++checks % 30 == 0 && !g_downed[i ^ 1].load()) {
            float a[3], b[3];
            if (Position(me, a) && Position(partner, b) && b[1] - a[1] > 40.0f) PlaceBeside(ctx, i, me, partner, "fell");
        }
    }
    for (int i = 0; i < 2; i++)
        g_ui_back[i] = g_downed[i].load() ? std::max(0.01f, g_down_left[i].load()) : 0.0f;
    g_ui_power = F32(p2 + kHealth);
    g_ui_max = F32(p2 + kMaxHealth);
    g_ui_threshold = F32(p2 + kRageThreshold);
    // P2 gets the power cheats P1 has (cheat_stats.cpp applies them to the main character only)
    if (!g_downed[1].load()) {
        const float health = F32(p2 + kHealth), max = F32(p2 + kMaxHealth), threshold = F32(p2 + kRageThreshold);
        float target = 0;
        if (state.On(hulk::cheats::Cheat::InfiniteRage)) target = max;
        else if (state.On(hulk::cheats::Cheat::InfiniteHealth)) target = std::min(threshold, max);
        if (health > 0 && target > 0 && health < target) Call(ctx, p2, kSetHealth, {std::bit_cast<uint32_t>(target), 1});
    }
}
}  // namespace
// hc57: the level ends (0x86980, before the game unloads it): the extra skin packages in the level heap are released
// while it still exists; the players' objects (holding the clones) go with the level as usual.
void HulkCoopSkinLevelEnd(X86Context& ctx) {
    g_skin_load_slot = -1;
    for (SkinPkg& p : g_pkgs) {
        if (p.state && p.sec) {
            LOGI("coop: skins - level end: releasing the %s package", kSkinFiles[p.skin]);
            Release(ctx, p.sec);
        }
        p = SkinPkg{};
    }
    g_remap[0].clone = g_remap[1].clone = 0;
}

namespace {
void ClearP2Down() {
    g_both_down = false;
    g_down_left[1] = 0;
}
void OnNewP2(X86Context& ctx, uint32_t p1, uint32_t p2, bool respawn) {
    g_downed[1] = false;
    g_down_left[1] = 0;
    g_trail[1].count = 0;
    g_player[1] = p2;
    if (respawn && !g_p1_dead.load() && !g_downed[0].load()) PlaceBeside(ctx, 1, p2, p1, "new P2");
}
}  // namespace

// a character's health setter (thiscall value, force; ret 8): a co-op player's hit that would down him holds him at 1
X86_EXTERN_FUNC(__imp__sub_0003B1C0);
X86_FUNC(sub_0003B1C0) { Crumb crumb_("sub_0003B1C0");
    const uint32_t actor = ctx.ecx;
    const int slot = g_p2_present.load() && !g_p1_dead.load() && g_in_play.load() ? PlayerSlot(actor) : -1;
    if (slot >= 0) {
        const float value = std::bit_cast<float>(uint32_t(X86_MEM32(ctx.esp + 4)));
        const float now = F32(actor + kHealth);
        const bool god = hulk::cheats::Level(hulk::cheats::Cheat::Invulnerability) != 0;
        if (God(slot) && value < now && !g_downed[slot].load()) {  // the mod menu's god mode for this player
            X86_MEM32(ctx.esp + 4) = std::bit_cast<uint32_t>(now);
        } else if (slot == 1 && god && value < now) {  // the invulnerability cheat covers P2 too
            X86_MEM32(ctx.esp + 4) = std::bit_cast<uint32_t>(now);
        } else if (g_downed[slot].load()) {
            if (value < 1.0f) X86_MEM32(ctx.esp + 4) = std::bit_cast<uint32_t>(1.0f);
        } else if (value < 1.0f && g_downed[slot ^ 1].load()) {
            if (slot == 0) {  // P1's hit with P2 down: the game's own death
                g_p1_dead = true;
                g_downed[1] = false;
            } else {  // P2 down too: hold him for the frame, P1's death follows (the main character's)
                X86_MEM32(ctx.esp + 4) = std::bit_cast<uint32_t>(1.0f);
                g_downed[1] = true;
                g_down_left[1] = float(RespawnSeconds());
                g_both_down = true;
            }
        } else if (value < 1.0f) {
            X86_MEM32(ctx.esp + 4) = std::bit_cast<uint32_t>(1.0f);
            g_downed[slot] = true;
            g_down_left[slot] = float(RespawnSeconds());
            g_pending_down = actor;
        }
    }
    __imp__sub_0003B1C0(ctx, base);
}

namespace coop_render {
// D3DDevice_Clear (stdcall count, rects, flags, color, z, stencil): the downed player's half is black
constexpr uint32_t kD3DClear = 0x386D70;
void BlackHalf(X86Context& ctx) {
    kernel::CallGuest(ctx, kD3DClear, {0, 0, 0xF3, 0xFF000000u, std::bit_cast<uint32_t>(1.0f), 0});
}
}  // namespace coop_render

// The keyboard's player (input thread, from GameInfo::pad): the port whose pad the keyboard joins. P2's pad port
// while P2 plays and keyboard_player is 2 (F8 swaps it), otherwise port 0 like the plain game.
std::atomic<int> g_keyboard_player{0};  // 0 until read from the settings
std::atomic<int> g_kb_polls{0}, g_kb_ly{0}, g_kb_lx{0};
void HulkCoopNoteKeyboard(int port, int lx, int ly) {  // diagnosis: the keyboard reached P2's pad port
    g_kb_polls++;
    g_kb_lx = lx;
    g_kb_ly = ly;
    (void)port;
}
int HulkCoopKeyboardPort() {
    if (!Enabled()) return 0;
    int who = g_keyboard_player.load();
    if (!who) {
        who = Config().keyboard_player;
        g_keyboard_player.store(who);
    }
    return who == 2 && g_p2_present.load() ? Config().p2_pad : 0;
}

// Polled with port 0's pad. F8 (game window in front) swaps the keyboard between P1 and P2.
// coop_quit.txt beside the game closes it cleanly, so a rebuild needs no Alt+F4.
void HulkCoopPoll(void* hwnd) {
    static bool f8_was = false;
    const bool front = hwnd && GetForegroundWindow() == HWND(hwnd);
    const SHORT f8_state = GetAsyncKeyState(VK_F8);  // low bit: pressed since the last call (short taps)
    const bool f8 = front && (f8_state & 0x8001);
    if (f8 && !f8_was && Enabled()) {
        HulkCoopKeyboardPort();  // reads the setting the first time
        const int who = g_keyboard_player.load() == 2 ? 1 : 2;
        g_keyboard_player.store(who);
        LOGI("coop: keyboard now drives P%d", who);
    }
    f8_was = f8;
    static bool f1_was = false;  // F1 opens and closes the co-op mod menu
    const bool f1 = front && (GetAsyncKeyState(VK_F1) & 0x8001);
    if (f1 && !f1_was && Enabled()) g_menu_toggle = true;
    f1_was = f1;
    static const bool debug_keys = std::filesystem::exists("coop_debug.txt");
    if (debug_keys && front && Enabled() && g_p2_present.load()) {
        const int me = g_keyboard_player.load() == 2 ? 1 : 0;
        static bool was[3] = {};
        static bool f12_was = false;
        const bool f12 = (GetAsyncKeyState(0x7B) & 0x8001) != 0;  // VK_F12
        if (f12 && !f12_was) {
            g_tint_all = !g_tint_all.load();
            d3d::SetTint(g_tint_all.load() ? 1.0f : 0.0f);
            LOGI("coop: test - everything recoloured %s", g_tint_all.load() ? "on" : "off");
        }
        f12_was = f12;
        const int keys[3] = {VK_F9, VK_F10, VK_F11};
        for (int i = 0; i < 3; i++) {
            const bool down = (GetAsyncKeyState(keys[i]) & 0x8001) != 0;
            if (down && !was[i]) {
                if (i == 0) g_req_down = me;
                else if (i == 1) g_req_fall = me;
                else g_req_both = true;
            }
            was[i] = down;
        }
    }
    static int polls = 0;
    if (++polls % 30 == 0 && hwnd && GetFileAttributesA("coop_quit.txt") != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA("coop_quit.txt");
        LOGI("coop: coop_quit.txt found, closing the game");
        logx::Flush();
        PostMessageW(HWND(hwnd), WM_CLOSE, 0, 0);
    }
}

// The overlay, drawn over the finished frame each present (app GameInfo::ui, UiLock held): P2's power bar in his half
// (the game's own HUD shows P1's), the line between the halves, and "back in N" over a downed player's black half.
namespace {
void DrawMenu();
}
void HulkCoopUi(double) {
    if (Enabled()) DrawMenu();
    if (!Enabled() || !g_p2_present.load()) return;
    ImGuiIO& io = ImGui::GetIO();
    const float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float vw = std::min(W, H * aspect), vh = vw / aspect;
    const float x0 = (W - vw) / 2, y0 = (H - vh) / 2;  // the game's picture in the window
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const bool split = g_ui_split.load(), vert = split && SplitVertical();
    // each player's area: [left, top, width, height]
    float area[2][4];
    for (int i = 0; i < 2; i++) {
        area[i][0] = x0, area[i][1] = y0, area[i][2] = vw, area[i][3] = vh;
        if (split && vert) area[i][2] = vw / 2, area[i][0] = x0 + (i ? vw / 2 : 0);
        else if (split) area[i][3] = vh / 2, area[i][1] = y0 + (i ? vh / 2 : 0);
    }
    const float half_top = area[1][1], half_h = area[1][3], half_left = area[1][0], half_w = area[1][2];
    if (split && vert) d->AddLine({x0 + vw / 2, y0}, {x0 + vw / 2, y0 + vh}, IM_COL32(0, 0, 0, 255), std::max(2.0f, vw / 400));
    else if (split) d->AddLine({x0, y0 + vh / 2}, {x0 + vw, y0 + vh / 2}, IM_COL32(0, 0, 0, 255), std::max(2.0f, vh / 240));
    // P2's power: green, orange in critical mass, a mark at the critical mass threshold
    const float max = g_ui_max.load(), power = g_ui_power.load(), threshold = g_ui_threshold.load();
    // the bar only while the world runs (not over the pause menu, popups or loading) and, when split screen is on,
    // only while it is drawn (not over cutscenes or scripted camera shots, which are P1's full screen)
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool world_running = now_ms - g_last_tick_ms.load() < 250;
    const bool show_bar = world_running && (split || !SplitAllowed());
    if (max > 0 && show_bar) {
        const float bw = half_w * (vert ? 0.5f : 0.28f), bh = std::max(8.0f, std::min(half_h, vh / 2) * 0.03f);
        const ImVec2 a{half_left + half_w * 0.04f, half_top + half_h * (split ? (vert ? 0.04f : 0.06f) : 0.90f)};
        const float f = std::clamp(power / max, 0.0f, 1.0f);
        const bool critical = threshold > 0 && power > threshold;
        d->AddRectFilled({a.x - 2, a.y - 2}, {a.x + bw + 2, a.y + bh + 2}, IM_COL32(0, 0, 0, 200));
        d->AddRectFilled(a, {a.x + bw * f, a.y + bh}, critical ? IM_COL32(255, 150, 20, 255) : IM_COL32(60, 200, 60, 255));
        if (threshold > 0 && threshold < max) {
            const float t = a.x + bw * threshold / max;
            d->AddLine({t, a.y - 2}, {t, a.y + bh + 2}, IM_COL32(255, 255, 255, 200), 2);
        }
        d->AddText(ImGui::GetFont(), bh * 1.6f, {a.x, a.y - bh * 1.9f}, IM_COL32(255, 255, 255, 255), "P2");
    }
    // a downed player's countdown, centred in his half (both halves when split, the whole picture otherwise)
    for (int i = 0; i < 2; i++) {
        const float back = g_ui_back[i].load();
        if (back <= 0 || !g_downed[i].load() || !world_running) continue;
        const float left = area[i][0], top = area[i][1], w = area[i][2], h = area[i][3];
        char text[48];
        std::snprintf(text, sizeof(text), "P%d DOWN - back in %d", i + 1, int(std::ceil(back)));
        const float size = std::min(h * 0.08f, w * 0.06f);
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
        d->AddText(ImGui::GetFont(), size, {left + (w - ts.x) / 2, top + (h - ts.y) / 2}, IM_COL32(255, 60, 60, 255), text);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Co-op mod menu: RB + d-pad up on either controller, or F1, opens and closes it. D-pad or left stick
// up/down picks, left/right changes a value, A does it, B closes. While it is open the game gets no input from any
// pad (or the keyboard), and the buttons that closed it are held back until they are released.
namespace {
std::atomic<float> g_zoom[2] = {1.0f, 1.0f};  // each player's camera distance (D-pad up/down in side-by-side split)
// hc37: both start at P1's saved distance (coop.ini zoom_side_by_side); P2's follows P1's until P2 uses his D-pad
std::atomic<bool> g_zoom_read{false}, g_zoom_p2_own{false}, g_zoom_dirty{false};
void ZoomInit() {
    if (g_zoom_read.exchange(true)) return;
    g_zoom[0] = Config().zoom_side;
    g_zoom[1] = Config().zoom_side;
}
enum MenuItem {
    kTeleportP2, kTeleportP1, kSplitDirection, kGodP1, kGodP2, kFriendlyFire, kSkinP1, kSkinP2, kShowSmashable, kSurvey, kSaveProp, kSpawnProp, kUnlockAll, kRedHulk, kDrawOrder, kRespawnWait, kFallRescue, kFollowWarps, kMenus43,
    kKeyboardPlayer, kClose, kMenuItems
};
std::atomic<bool> g_menu_open[2] = {false, false};  // each player's own menu (P1 index 0, P2 index 1)
std::atomic<int> g_menu_sel[2] = {0, 0};
std::atomic<int64_t> g_menu_note_until{0};
std::atomic<int> g_menu_note_id{-1};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void SaveIni() {
    const Settings& c = Config();
    if (std::FILE* w = std::fopen("coop.ini", "w")) {
        std::fprintf(w,
                     "# HULK co-op settings (the in-game mod menu, RB + D-pad Up or F1, changes everything below split)\n"
                     "# pads count 1-4 in the order Windows connected them\n"
                     "p1_pad = %d\np2_pad = %d\n"
                     "# 0 = one full screen on P1\nsplit = %d\n"
                     "respawn_seconds = %d\n"
                     "# 1 = a player who falls far below his partner is put beside him\n"
                     "fall_rescue = %d\n"
                     "# which player the keyboard drives: 1 or 2 (F8 swaps it in game)\n"
                     "keyboard_player = %d\n"
                     "# 1 = the halves side by side (P1 left), 0 = top and bottom\n"
                     "split_vertical = %d\n"
                     "# god mode for each player\n"
                     "god_p1 = %d\ngod_p2 = %d\n"
                     "# 1 = the game's menus and popups in the 4:3 middle of a widescreen picture (not stretched)\n"
                     "menus_4_3 = %d\n"
                     "# 1 = P2 joins P1 when the game moves P1 far in one go (cutscenes, mission starts)\n"
                     "follow_warps = %d\n"
                     "# 1 = the players can lock on to and auto-aim throws at each other (0 = only at enemies)\n"
                     "friendly_fire = %d\n"
                     "# 1 = buildings that can be destroyed are shown on the radar (smash icons)\n"
                     "show_smashable = %d\n"
                     "# Hulk skin for both players, loaded at the next checkpoint / level load: -1 the game's choice,\n"
                     "# 0 Grey Hulk, 1 Joe Fixit, 2 Abomination, 3 Savage Banner, 4-12 the country Hulks\n"
                     "skin = %d\n"
                     "# each player's own skin, switched in the game at once (mod menu): -1 = the level's, 0-12 as above,\n"
                     "# 13 Abomination / 14 Savage Banner with their own bodies (real model)\n"
                     "skin_p1 = %d\nskin_p2 = %d\n"
                     "# which player is Red Hulk: 1, 2, or 0 for neither\n"
                     "red_hulk_player = %d\n"
                     "# P1's camera distance in side-by-side split (D-pad up/down; P2 starts with it). 1 = the game's\n"
                     "zoom_side_by_side = %.3f\n"
                     "# 1 = trees and props near either player are kept first (0 = the game's order, nearest P1)\n"
                     "prop_sort = %d\n"
                     "# while P2 plays: world pieces (buildings, city ground, trees) loaded at once (50-120) and their radius\n"
                     "# in m (150-1600). Higher = the players can be further apart, but more memory (120 / 1600 froze)\n"
                     "world_cap = %d\nworld_radius = %.0f\n"
                     "# 1 = per-player skins on (the P1 / P2 skin menu options)\n"
                     "skins_test = %d\n"
                     "# 1 = extra skin-loading log lines (for Claude)\n"
                     "skins_diag = %d\n"
                     "# 1 = all 13 skin packages loaded at each level / checkpoint load: every skin switches at once\n"
                     "skins_preload_all = %d\n"
                     "# 1 = the menu leaves out the stretched skin-swap Abomination / Savage Banner (the real models stay)\n"
                     "skins_hide_swaps = %d\n",
                     c.p1_pad + 1, c.p2_pad + 1, int(c.split), RespawnSeconds(), int(FallRescue()),
                     g_keyboard_player.load() == 2 ? 2 : 1, int(SplitVertical()), int(God(0)), int(God(1)),
                     int(Menus43()), int(FollowWarps()), int(FriendlyFire()), int(ShowSmashable()), Skin(), PlayerSkin(0), PlayerSkin(1), RedHulkPlayer(), (ZoomInit(), double(g_zoom[0].load())), int(Config().prop_sort),
                     Config().prop_cap, double(Config().prop_radius), int(Config().skins_test), int(Config().skins_diag),
                     int(Config().skins_preload_all), int(Config().skins_hide_swaps));
        std::fclose(w);
    }
}

void MenuDo(int who, int item, int dir) {  // who: the player whose menu it is; dir 0: A, -1/+1: left/right
    switch (item) {
    case kSurvey:
        if (dir) return;
        g_req_survey = who;
        g_menu_note_id = item;
        g_menu_note_until = NowMs() + 1500;
        return;
    case kUnlockAll:
        if (dir) return;
        g_req_unlock = true;
        g_menu_note_id = item;
        g_menu_note_until = NowMs() + 1500;
        return;
    case kSaveProp:
    case kSpawnProp:
        if (dir) return;
        g_req_prop = who * 2 + (item == kSpawnProp ? 1 : 0);
        g_menu_note_id = item;
        g_menu_note_until = NowMs() + 1500;
        if (item == kSpawnProp) g_menu_open[who] = false;  // back to the game to pick it up
        return;
    case kTeleportP2:
    case kTeleportP1:
        if (dir) return;
        if (!g_p2_present.load()) return;
        g_req_teleport = item == kTeleportP2 ? 1 : 0;
        g_menu_note_id = item;
        g_menu_note_until = NowMs() + 1500;
        g_menu_open[who] = false;  // back to the game to see it
        LOGI("coop: menu - %s", item == kTeleportP2 ? "teleport P2 to P1" : "teleport P1 to P2");
        return;
    case kRespawnWait: {
        const int v = std::clamp(RespawnSeconds() + (dir ? dir : 1), 1, 60);
        g_respawn_s = v;
        SaveIni();
        return;
    }
    case kFallRescue:
        FallRescue();
        g_fall_rescue = !g_fall_rescue.load();
        SaveIni();
        return;
    case kSplitDirection:
        SplitVertical();
        g_split_vertical = !g_split_vertical.load();
        SaveIni();
        LOGI("coop: menu - split %s", g_split_vertical.load() ? "side by side" : "top and bottom");
        return;
    case kGodP1:
    case kGodP2: {
        const int who = item == kGodP2 ? 1 : 0;
        God(who);
        g_god[who] = !g_god[who].load();
        SaveIni();
        LOGI("coop: menu - P%d god mode %s", who + 1, g_god[who].load() ? "on" : "off");
        return;
    }
    case kRedHulk: {  // P2 -> P1 -> off -> P2 (right / A), the other way with left
        const int order[3] = {2, 1, 0};
        int at = 0;
        for (int i = 0; i < 3; i++)
            if (order[i] == RedHulkPlayer()) at = i;
        at = (at + (dir < 0 ? 2 : 1)) % 3;
        g_red_hulk = order[at];
        SaveIni();
        LOGI("coop: menu - Red Hulk %s", g_red_hulk.load() ? (g_red_hulk.load() == 1 ? "P1" : "P2") : "off");
        return;
    }
    case kDrawOrder:
        g_draw_p2_first = !g_draw_p2_first.load();
        LOGI("coop: menu - test: P2's half drawn %s", g_draw_p2_first.load() ? "first" : "second (normal)");
        return;
    case kMenus43:
        Menus43();
        g_menus_4_3 = !g_menus_4_3.load();
        SaveIni();
        return;
    case kSkinP1:
    case kSkinP2: {  // hc50: Normal (the level's) -> Grey Hulk -> ... -> Japan Hulk -> Normal; applied at once in the game
        const int p = item == kSkinP2 ? 1 : 0;
        int at = 0;
        for (int i = 0; i < 16; i++)
            if (kSkinOrder[i] == PlayerSkin(p)) at = i;
        int v = kSkinOrder[(at + (dir < 0 ? 15 : 1)) % 16];  // A or right: next
        // hc82: skins_hide_swaps skips the stretched skin-swap versions (2, 3); the real models stay
        for (int guard = 0; Config().skins_hide_swaps && (v == 2 || v == 3) && guard < 16; guard++) {
            for (int i = 0; i < 16; i++)
                if (kSkinOrder[i] == v) at = i;
            v = kSkinOrder[(at + (dir < 0 ? 15 : 1)) % 16];
        }
        g_pskin[p] = v;
        g_pskin_changed_ms = CrumbNowMs();
        SaveIni();
        LOGI("coop: menu - P%d skin %d (%s)", p + 1, v, v < 0 ? "the level's" : kSkinNames[v]);
        return;
    }
    case kShowSmashable:
        ShowSmashable();
        g_show_smashable = !g_show_smashable.load();
        SaveIni();
        return;
    case kFriendlyFire:
        FriendlyFire();
        g_friendly_fire = !g_friendly_fire.load();
        SaveIni();
        return;
    case kFollowWarps:
        FollowWarps();
        g_follow_warps = !g_follow_warps.load();
        SaveIni();
        return;
    case kKeyboardPlayer:
        HulkCoopKeyboardPort();  // reads the setting the first time
        g_keyboard_player = g_keyboard_player.load() == 2 ? 1 : 2;
        SaveIni();
        return;
    case kClose:
        if (!dir) g_menu_open[who] = false;
        return;
    }
}
}  // namespace

// From GameInfo::pad (input thread), after the keyboard has joined the pad: each player's own menu. RB + d-pad up on
// his pad (F1 on the keyboard, for the player it drives) opens and closes it; only he works it, it is drawn in his half,
// and only his input is held back from the game while it is open.
void HulkCoopMenuPad(int port, input::Pad& pad) {
    if (!Enabled() || port < 0 || port > 3) return;
    static uint16_t prev[4] = {};
    static bool hold_back[4] = {};
    uint16_t b = pad.buttons;
    if (pad.analog[0] > 30) b |= 0x1000;  // A
    if (pad.analog[1] > 30) b |= 0x2000;  // B
    if (pad.ly > 16000) b |= 0x4000;      // stick up / down / left / right, as d-pad presses for the menu
    if (pad.ly < -16000) b |= 0x8000;
    const bool stick_left = pad.lx < -16000, stick_right = pad.lx > 16000;
    static bool sl_prev[4] = {}, sr_prev[4] = {};
    const uint16_t pressed = b & ~prev[port];
    const bool left = (pressed & 0x4) || (stick_left && !sl_prev[port]);
    const bool right = (pressed & 0x8) || (stick_right && !sr_prev[port]);
    prev[port] = b;
    sl_prev[port] = stick_left, sr_prev[port] = stick_right;
    const bool rb = pad.analog[4] > 30;  // RB (the black button's slot)
    const bool combo = rb && (pressed & 0x1);
    // whose pad this is: P2's port, else P1's (his pad's port, or port 0 where the keyboard joins while it drives P1)
    const int kb_port = HulkCoopKeyboardPort();
    const int who = port == Config().p2_pad ? 1 : (port == Config().p1_pad || port == 0) ? 0 : -1;
    // popups, tutorial boxes and the pause menu read P1's controls only: while the world is stopped (one of those is
    // up) P2's A and B also count as P1's, so either player can close them. P2's pad is read on its own port; the
    // next poll of P1's port carries his A/B along.
    static std::atomic<uint8_t> p2_ab{0};
    const int64_t now_ms = NowMs();
    const bool world_stopped = now_ms - g_last_tick_ms.load() > 250;
    if (who == 1) p2_ab = uint8_t((pad.analog[0] > 30 ? 1 : 0) | (pad.analog[1] > 30 ? 2 : 0));
    if (who == 0 && port == Config().p1_pad && world_stopped && g_p2_present.load() && !g_menu_open[0].load() &&
        !g_menu_open[1].load()) {
        const uint8_t ab = p2_ab.load();
        if (ab & 1) pad.analog[0] = 0xFF;
        if (ab & 2) pad.analog[1] = 0xFF;
    }
    if (port == kb_port && g_menu_toggle.exchange(false)) {  // F1: the keyboard's player's menu
        const int kb_who = g_keyboard_player.load() == 2 && g_p2_present.load() ? 1 : 0;
        g_menu_open[kb_who] = !g_menu_open[kb_who].load();
        g_menu_sel[kb_who] = 0;
    }
    if (who < 0) return;
    // side-by-side split: d-pad up/down zoom this player's camera in and out (held), and do not reach the game
    if (!g_menu_open[who].load() && !rb && g_ui_split.load() && SplitVertical() && g_p2_present.load() && (b & 0x3)) {
        ZoomInit();
        float z = g_zoom[who].load();
        z *= (b & 0x1) ? 0.985f : 1.015f;  // up closer, down farther (about 1.5% a poll)
        z = std::clamp(z, 0.4f, 2.5f);
        g_zoom[who] = z;
        if (who == 1) g_zoom_p2_own = true;
        else {
            if (!g_zoom_p2_own.load()) g_zoom[1] = z;  // P2 keeps P1's distance until he picks his own
            g_zoom_dirty = true;
        }
        pad.buttons &= uint16_t(~0x3u);
    } else if (who == 0 && !(b & 0x3) && g_zoom_dirty.exchange(false)) {  // P1 let go of the D-pad: remember it
        SaveIni();
        LOGI("coop: P1's side-by-side camera distance %.2f saved (P2 %.2f%s)", g_zoom[0].load(), g_zoom[1].load(),
             g_zoom_p2_own.load() ? ", his own" : ", follows P1");
    }
    if (who == 1 && !g_p2_present.load()) return;  // no P2 in the game: his pad has no menu
    std::atomic<bool>& open = g_menu_open[who];
    if (!open.load() && !hold_back[port] && !world_stopped) {  // hc49: the carry (friendly fire on)
        if (pressed & 0x2000) g_press_b[who] = true;
        if (pressed & 0x1000) g_press_a[who] = true;
    }
    if (!open.load()) {
        if (combo) {
            open = true;
            g_menu_sel[who] = 0;
            hold_back[port] = true;
        }
    } else if (combo || (pressed & 0x2000)) {
        open = false;
        hold_back[port] = true;
    } else {
        int sel = g_menu_sel[who].load();
        if (pressed & (0x1 | 0x4000)) sel = (sel + kMenuItems - 1) % kMenuItems;
        if (pressed & (0x2 | 0x8000)) sel = (sel + 1) % kMenuItems;
        g_menu_sel[who] = sel;
        if (left) MenuDo(who, sel, -1);
        if (right) MenuDo(who, sel, 1);
        if (pressed & 0x1000) MenuDo(who, sel, 0);
        hold_back[port] = true;
    }
    if (open.load()) hold_back[port] = true;
    if (hold_back[port]) {
        if (!open.load() && !b && !stick_left && !stick_right) hold_back[port] = false;
        const bool connected = pad.connected;
        pad = {};
        pad.connected = connected;
    }
}

namespace {
// a player's area of the window: the game's picture, his half of it while split
void PlayerArea(int who, float out[4]) {
    ImGuiIO& io = ImGui::GetIO();
    const float aspect = app::Widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float vw = std::min(W, H * aspect), vh = vw / aspect;
    out[0] = (W - vw) / 2, out[1] = (H - vh) / 2, out[2] = vw, out[3] = vh;
    if (!g_ui_split.load()) return;
    if (SplitVertical()) out[2] = vw / 2, out[0] += who ? vw / 2 : 0;
    else out[3] = vh / 2, out[1] += who ? vh / 2 : 0;
}

void DrawOneMenu(int who) {
    float area[4];
    PlayerArea(who, area);
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const float W = area[2], H = area[3];
    const float size = std::min(H * 0.045f, W * 0.024f) * (g_ui_split.load() ? 1.25f : 1.0f), line = size * 1.45f;
    char rows[kMenuItems][64];
    std::snprintf(rows[kTeleportP2], 64, "Teleport P2 to P1");
    std::snprintf(rows[kTeleportP1], 64, "Teleport P1 to P2");
    std::snprintf(rows[kRespawnWait], 64, "Respawn wait:  < %d s >", RespawnSeconds());
    std::snprintf(rows[kFallRescue], 64, "Fall rescue:  < %s >", FallRescue() ? "On" : "Off");
    std::snprintf(rows[kSplitDirection], 64, "Split screen:  < %s >", SplitVertical() ? "Side by side" : "Top / bottom");
    std::snprintf(rows[kGodP1], 64, "P1 god mode:  < %s >", God(0) ? "On" : "Off");
    std::snprintf(rows[kGodP2], 64, "P2 god mode:  < %s >", God(1) ? "On" : "Off");
    std::snprintf(rows[kDrawOrder], 64, "Test - draw P2's half first:  < %s >", DrawP2First() ? "On" : "Off");
    std::snprintf(rows[kFriendlyFire], 64, "Friendly fire:  < %s >", FriendlyFire() ? "On" : "Off");
    for (int p = 0; p < 2; p++)  // hc66: a skin that is not on yet comes with the next checkpoint (or is loading)
        std::snprintf(rows[p ? kSkinP2 : kSkinP1], 64, "P%d skin:  < %s >%s", p + 1,
                      PlayerSkin(p) < 0 ? "Normal" : kSkinNames[PlayerSkin(p)],
                      PlayerSkin(p) < 0 || g_pm[p].applied == PlayerSkin(p) ? ""
                      : !Config().skins_test ? " (not working yet)"
                      : SkinPkgLoaded(SkinFile(PlayerSkin(p))) ? " (loading)" : " (next checkpoint)");  // hc83
    std::snprintf(rows[kShowSmashable], 64, "Smashable buildings on radar:  < %s >", ShowSmashable() ? "On" : "Off");
    std::snprintf(rows[kSurvey], 64, "Log buildings near me (for Claude)");
    std::snprintf(rows[kSaveProp], 64, "Save held object");
    std::snprintf(rows[kUnlockAll], 64, "Unlock all upgrades (both Hulks)");
    std::snprintf(rows[kSpawnProp], 64, "Spawn saved object (30 ft ahead)");
    std::snprintf(rows[kRedHulk], 64, "Red Hulk:  < %s >%s", RedHulkPlayer() == 1 ? "P1" : RedHulkPlayer() == 2 ? "P2" : "Off",
                  RedHulkPlayer() && !RedHulkDrawn() ? " (his skin shows)" : "");
    std::snprintf(rows[kFollowWarps], 64, "P2 joins P1 after cutscenes:  < %s >", FollowWarps() ? "On" : "Off");
    std::snprintf(rows[kMenus43], 64, "Menus not stretched (4:3):  < %s >", Menus43() ? "On" : "Off");
    std::snprintf(rows[kKeyboardPlayer], 64, "Keyboard drives:  < P%d >", g_keyboard_player.load() == 2 ? 2 : 1);
    std::snprintf(rows[kClose], 64, "Close");
    char title[40];
    std::snprintf(title, sizeof(title), "P%d  CO-OP MOD MENU", who + 1);
    const char* help = "Up/Down select  Left/Right change  A ok  B close";
    float bw = size * 2;
    for (int i = 0; i < kMenuItems; i++) bw = std::max(bw, ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, rows[i]).x + size * 2);
    bw = std::max(bw, ImGui::GetFont()->CalcTextSizeA(size * 0.6f, FLT_MAX, 0, help).x + size * 2);
    const float bh = line * (float(kMenuItems) + 2.6f);
    const ImVec2 a{area[0] + (W - bw) / 2, area[1] + std::max(0.0f, (H - bh) / 2)}, z{a.x + bw, a.y + bh};
    d->AddRectFilled(a, z, IM_COL32(8, 20, 8, 230), size * 0.3f);
    d->AddRect(a, z, who ? IM_COL32(230, 90, 90, 255) : IM_COL32(90, 230, 90, 255), size * 0.3f, 0, 2.0f);
    d->AddText(ImGui::GetFont(), size, {a.x + size, a.y + size * 0.4f},
               who ? IM_COL32(255, 150, 140, 255) : IM_COL32(140, 255, 140, 255), title);
    const int sel = g_menu_sel[who].load();
    for (int i = 0; i < kMenuItems; i++) {
        const float y = a.y + line * (i + 1.3f);
        const bool dim = (i == kTeleportP2 || i == kTeleportP1) && !g_p2_present.load();
        if (i == sel) d->AddRectFilled({a.x + size * 0.5f, y - size * 0.15f}, {z.x - size * 0.5f, y + size * 1.15f},
                                       IM_COL32(40, 140, 40, 255), size * 0.2f);
        d->AddText(ImGui::GetFont(), size, {a.x + size, y}, dim ? IM_COL32(120, 120, 120, 255) : IM_COL32(255, 255, 255, 255),
                   rows[i]);
    }
    d->AddText(ImGui::GetFont(), size * 0.6f, {a.x + size, z.y - line * 0.95f}, IM_COL32(180, 220, 180, 255), help);
}

void DrawMenu() {
    if (CrumbNowMs() < g_world_note_until.load()) {
        ImGuiIO& io = ImGui::GetIO();
        const char* t = "Too far apart - P2 brought back to P1";
        const float size = io.DisplaySize.y * 0.035f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, t);
        ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size,
                                                {(io.DisplaySize.x - ts.x) / 2, io.DisplaySize.y * 0.40f},
                                                IM_COL32(255, 220, 120, 255), t);
    }
    const int note = g_menu_note_id.load();
    if (note >= 0 && NowMs() < g_menu_note_until.load()) {
        ImGuiIO& io = ImGui::GetIO();
        const char* t = note == kTeleportP2 ? "P2 teleported to P1"
                        : note == kTeleportP1 ? "P1 teleported to P2"
                        : note == kSaveProp   ? "Held object saved"
                        : note == kUnlockAll  ? "All upgrades unlocked"
                        : note == kSurvey     ? "Buildings near you logged"
                                              : "Saved object spawned ahead";
        const float size = io.DisplaySize.y * 0.035f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, t);
        ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size,
                                                {(io.DisplaySize.x - ts.x) / 2, io.DisplaySize.y * 0.47f},
                                                IM_COL32(120, 255, 120, 255), t);
    }
    for (int who = 0; who < 2; who++)
        if (g_menu_open[who].load()) DrawOneMenu(who);
}
}  // namespace

// the chase camera update (game/hooks.cpp) asks each camera's zoom: P2's camera has P2's, every other P1's
float HulkCoopCameraZoom(uint32_t camera) {
    // hc37: only side by side; top and bottom (and one screen) keep the game's distance as before
    if (!Enabled() || !g_ui_split.load() || !SplitVertical()) return 1.0f;
    ZoomInit();
    return g_zoom[camera && camera == g_camera2 ? 1 : 0].load();
}

// hc43 FRIENDLY FIRE (mod menu / coop.ini friendly_fire, default off). The fight code checks every lock-on and
// auto-aim candidate (throws, attacks, the LT lock: 0x396A0 and the others) with 0x39440 (thiscall character;
// candidate target info (+0xC type, 1 = character; +8 its actor), out position, out priority, flag; ret 0x10, al = can
// be targeted). With friendly fire off, the partner is never a candidate for either player.
X86_EXTERN_FUNC(__imp__sub_00039440);
X86_FUNC(sub_00039440) {
    const uint32_t me = ctx.ecx, info = X86_MEM32(ctx.esp + 4);
    if (info && !FriendlyFire() && g_p2_present.load()) {
        const uint32_t p1 = g_player[0].load(), p2 = g_player[1].load();
        const uint32_t partner = me == p1 ? p2 : me == p2 ? p1 : 0;
        if (partner && R32(info + 0xC) == 1 && R32(info + 8) == partner) {
            static uint32_t skipped = 0;
            if (++skipped % 600 == 1) LOGI("coop: friendly fire off: partner skipped as a target (%u times)", skipped);
            ctx.eax &= ~0xFFu;      // al = 0: not a target
            ctx.esp += 4 + 0x10;    // return address + 4 arguments (ret 0x10)
            return;
        }
    }
    __imp__sub_00039440(ctx, base);
}

// hc44: a target can also be set without the candidate check (hulk_048: 6600 partner candidates refused, yet thrown
// cars still homed in on the partner). CCharacter::SetTarget (0x3FFA0, thiscall (target info, flag), ret 8): with
// friendly fire off, the partner as a player's target becomes "no target" (0 clears it, as the game does).
X86_EXTERN_FUNC(__imp__sub_0003FFA0);
X86_FUNC(sub_0003FFA0) {
    const uint32_t me = ctx.ecx, info = X86_MEM32(ctx.esp + 4);
    if (info && !FriendlyFire() && g_p2_present.load()) {
        const uint32_t p1 = g_player[0].load(), p2 = g_player[1].load();
        const uint32_t partner = me == p1 ? p2 : me == p2 ? p1 : 0;
        if (partner && R32(info + 0xC) == 1 && R32(info + 8) == partner) {
            static uint32_t n = 0;
            if (++n % 100 == 1) LOGI("coop: friendly fire off: %s's target set to the partner refused (%u times)",
                                     me == p1 ? "P1" : "P2", n);
            X86_MEM32(ctx.esp + 4) = 0;
        }
    }
    __imp__sub_0003FFA0(ctx, base);
}

// hc47: the level's Hulk skin (see Skin()): after the game's own choice, the mod menu's one if set
X86_EXTERN_FUNC(__imp__sub_00203A10);
X86_FUNC(sub_00203A10) {
    const uint32_t stream = ctx.ecx;
    g_stream = stream;
    __imp__sub_00203A10(ctx, base);
    const int skin = Skin();
    if (stream && skin >= 0 && skin <= 12) {
        Call(ctx, stream, 0x203890, {uint32_t(skin)});
        LOGI("coop: skin %d (%s) for this level", skin, kSkinNames[skin]);
    }
}

// hc48 FRIENDLY FIRE OFF, HITS: a character takes a hit through 0x43350 (thiscall victim; arg 1 the damage, arg 2 the
// attacker's target info (+0xC 1 = character, +8 the actor), ...; ret 0x18, al = hit taken). With friendly fire off a
// hit from the other player is refused (no damage, and whatever the game does after the hit is skipped).
// (cheat_stats.cpp owns the 0x43350 hook: it calls this first and, on true, returns al = 0 / ret 0x18 itself.)
bool HulkCoopRefuseHit(uint32_t victim, uint32_t info) {
    if (info && !FriendlyFire() && g_p2_present.load()) {
        const uint32_t p1 = g_player[0].load(), p2 = g_player[1].load();
        const uint32_t partner = victim == p1 ? p2 : victim == p2 ? p1 : 0;
        const uint32_t type = partner ? R32(info + 0xC) : 0;
        // hc49: a prop the partner threw (it has its own target info, not his)
        const bool by_prop = partner && type != 1 && HeldLately(victim == p1 ? 1 : 0, info);
        if (partner && type != 1 && !by_prop) {
            static int seen = 0;  // what else hits a player, in case the partner's car still hurts
            if (seen < 12) {
                seen++;
                LOGI("coop: hit on %s by target info %08X type %u owner %08X (vtable %08X)", victim == p1 ? "P1" : "P2",
                     info, type, R32(info + 8), R32(info + 8) ? R32(R32(info + 8)) : 0);
            }
        }
        if (by_prop || (partner && type == 1 && R32(info + 8) == partner)) {
            static uint32_t n = 0;
            if (++n % 50 == 1) LOGI("coop: friendly fire off: %s's %s on %s refused (%u times)",
                                    victim == p1 ? "P2" : "P1", by_prop ? "thrown prop" : "hit", victim == p1 ? "P1" : "P2", n);
            return true;
        }
    }
    return false;
}

// hc86: the carrier's arms over his head. Just before his mesh is skinned, his pose is brought up to date, the
// shoulders' (19 L, 27 R) and elbows' (20, 28) local matrices are turned, and the pose rebuilt from its locals
// (+0x23, as hc79). Skeleton "hulk_skeleton" (38 joints): 18 Clavicle_L 19 Shoulder_L 20 Elbow_L ... 26 Clavicle_R
// 27 Shoulder_R 28 Elbow_R. Normal-shape skins only (family 0); Abomination / Savage Banner keep their animation.
// (hc97: TurnLocal, the hc86-hc88 local-angle arm turning, removed: replaced by the world-space aim below)
// hc87: applied where the pose becomes skin matrices (0x2DFC90, thiscall skin (?, pose): skin[j] = invbind[j] x
// world[j] via 0x22CA90(dst; a, b) = a x b). The arms' locals are turned, joints 18-33 rebuilt from their parents
// (world[j] = local[j] x world[parent], parents: uint16 at skeleton +0x28), the mesh is skinned, then locals and
// worlds are put back, so the game's own pose is never changed (hc86 rebuilt the whole pose: the body went to the
// world origin, the same trap as hc79).
int CarryPoseWho() {
    const Settings& c = Config();
    if (!c.carry_pose) return -1;
    if (c.carry_pose_test) return c.carry_pose_test - 1;
    return g_carry.who >= 0 && g_carry.phase == 2 ? g_carry.who : -1;
}
X86_EXTERN_FUNC(__imp__sub_002DFC90);
X86_FUNC(sub_002DFC90) {
    const uint32_t pose = R32(ctx.esp + 8);
    // hc96: the carried player lies flat across the carrier's raised hands: his whole drawn pose is turned 90 degrees
    // about his own sideways axis through the pelvis and lowered so the pelvis sits just above his root (= on the
    // hands, the carry code puts his root there). Drawn only; the game's pose is put back after skinning.
    if (Enabled() && pose && Config().carry_pose && g_carry.who >= 0 && g_carry.phase == 2) {
        const int o = g_carry.who ^ 1;
        const uint32_t oo = g_pm[o].obj;
        if (oo && R32(oo) == 0x4218C8) {
            const bool rm = g_remap[o].clone.load() && g_remap[o].clone.load() == R32(oo + 0x10);
            const int f = rm ? g_remap[o].to.load() : 0;
            const uint32_t wantp = rm ? g_remap[o].pose.load() : R32(oo + 0x18);
            const int n = (f >= 0 && f <= 2) ? kJointCount[f] : 0;
            const uint32_t Wd = R32(pose + 0x1C);
            if (n && pose == wantp && R32(pose + 0xC) == uint32_t(n) && Wd) {
                uint8_t save[38 * 0x40];
                std::memcpy(save, kernel::HostPtr(Wd), 0x40 * size_t(n));
                const int pel = kJointMap[0][f][3];
                float pv[3], rt[3], ax[3] = {F32(Wd + 0), 0.0f, F32(Wd + 8)};
                for (int k = 0; k < 3; k++) pv[k] = F32(Wd + 0x40 * uint32_t(pel) + 0x30 + 4 * uint32_t(k)), rt[k] = F32(Wd + 0x30 + 4 * uint32_t(k));
                float al = std::sqrt(ax[0] * ax[0] + ax[2] * ax[2]);
                if (al < 1e-4f) ax[0] = 1, ax[2] = 0, al = 1;
                ax[0] /= al, ax[2] /= al;
                // 90 degrees about ax (Rodrigues: sin 1, cos 0 -> M = I + K + K^2)
                const float K[3][3] = {{0, -ax[2], ax[1]}, {ax[2], 0, -ax[0]}, {-ax[1], ax[0], 0}};
                float M[3][3];
                for (int i = 0; i < 3; i++)
                    for (int jj = 0; jj < 3; jj++) {
                        float kk = 0;
                        for (int m = 0; m < 3; m++) kk += K[i][m] * K[m][jj];
                        M[i][jj] = (i == jj ? 1.0f : 0.0f) + K[i][jj] + kk;
                    }
                const float drop = -(pv[1] - rt[1]) + 0.4f;
                for (int j = 0; j < n; j++) {
                    const uint32_t mtx = Wd + 0x40 * uint32_t(j);
                    for (int r = 0; r < 4; r++) {
                        const uint32_t row = mtx + 0x10 * uint32_t(r);
                        float v[3] = {F32(row), F32(row + 4), F32(row + 8)};
                        if (r == 3) for (int k = 0; k < 3; k++) v[k] -= pv[k];
                        for (int i = 0; i < 3; i++) {
                            float x = M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2];
                            if (r == 3) x += pv[i] + (i == 1 ? drop : 0.0f);
                            W32(row + 4 * uint32_t(i), std::bit_cast<uint32_t>(x));
                        }
                    }
                }
                __imp__sub_002DFC90(ctx, base);
                std::memcpy(kernel::HostPtr(Wd), save, 0x40 * size_t(n));
                return;
            }
        }
    }
    const int w = Enabled() ? CarryPoseWho() : -1;
    const uint32_t obj = w >= 0 ? g_pm[w].obj : 0;
    if (!obj || R32(obj) != 0x4218C8 || !pose) { __imp__sub_002DFC90(ctx, base); return; }
    // hc93: the carrier's drawn pose: his own (normal-shape skins) or his remapped skin's (Abomination / Savage
    // Banner: g_remap's pose, family 1 / 2); joints found by name through kJointMap[0][family][hulk joint]
    const bool remapped = g_remap[w].clone.load() && g_remap[w].clone.load() == R32(obj + 0x10);
    const int fam = remapped ? g_remap[w].to.load() : 0;
    const uint32_t want = remapped ? g_remap[w].pose.load() : R32(obj + 0x18);
    const int n = kJointCount[fam];
    if (pose != want || fam < 0 || fam > 2 || R32(pose + 0xC) != uint32_t(n) || !R32(pose + 0x14) || !R32(pose + 0x1C) ||
        !R32(pose + 8) || !R32(R32(pose + 8) + 0x28)) {
        __imp__sub_002DFC90(ctx, base);
        return;
    }
    const Settings& c = Config();
    const uint32_t L = R32(pose + 0x14), Wd = R32(pose + 0x1C), par = R32(R32(pose + 8) + 0x28);
    auto J = [&](int hulk_joint) { return int(kJointMap[0][fam][hulk_joint]); };
    auto Parent = [&](int q) { return int((R32(par + 2 * uint32_t(q) & ~3u) >> ((q & 1) * 16)) & 0xFFFF); };
    uint8_t saveW[38 * 0x40];
    std::memcpy(saveW, kernel::HostPtr(Wd), 0x40 * size_t(n));
    auto P = [&](int j, int k) { return F32(Wd + 0x40 * uint32_t(j) + 0x30 + 4 * uint32_t(k)); };
    const uint32_t esp0 = ctx.esp;
    // aim joint j so that j -> child points along t (world), then rebuild every joint below j (index order)
    auto Aim = [&](int j, int child, const float* tin) {
        float d[3], t[3] = {tin[0], tin[1], tin[2]};
        for (int k = 0; k < 3; k++) d[k] = P(child, k) - P(j, k);
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (dl < 1e-4f || tl < 1e-4f) return;
        for (int k = 0; k < 3; k++) d[k] /= dl, t[k] /= tl;
        float ax[3] = {d[1] * t[2] - d[2] * t[1], d[2] * t[0] - d[0] * t[2], d[0] * t[1] - d[1] * t[0]};
        const float sn = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), cs = d[0] * t[0] + d[1] * t[1] + d[2] * t[2];
        if (sn < 1e-5f) return;
        for (float& v : ax) v /= sn;
        const float K[3][3] = {{0, -ax[2], ax[1]}, {ax[2], 0, -ax[0]}, {-ax[1], ax[0], 0}};
        float M[3][3];
        for (int i = 0; i < 3; i++)
            for (int jj = 0; jj < 3; jj++) {
                float kk = 0;
                for (int m = 0; m < 3; m++) kk += K[i][m] * K[m][jj];
                M[i][jj] = (i == jj ? 1.0f : 0.0f) + sn * K[i][jj] + (1 - cs) * kk;
            }
        for (int r = 0; r < 3; r++) {  // each axis row of j's world matrix, turned (v' = M v)
            const uint32_t row = Wd + 0x40 * uint32_t(j) + 0x10 * uint32_t(r);
            const float v[3] = {F32(row), F32(row + 4), F32(row + 8)};
            for (int i = 0; i < 3; i++)
                W32(row + 4 * uint32_t(i), std::bit_cast<uint32_t>(M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2]));
        }
        for (int q = 0; q < n; q++) {
            bool below = false;
            for (int u = Parent(q), guard = 0; q != j && u < n && u != q && guard < 40; u = Parent(u), guard++) {
                if (u == j) { below = true; break; }
                if (u == 0 || u == Parent(u)) break;
            }
            if (!below) continue;
            const int pq = Parent(q);
            Call(ctx, Wd + 0x40 * uint32_t(q), 0x22CA90, {L + 0x40 * uint32_t(q), Wd + 0x40 * uint32_t(pq)});
        }
    };
    const int spine = J(14);
    for (int side = 0; side < 2; side++) {
        const int sj = J(side ? 27 : 19), ej = J(side ? 28 : 20), wj = J(side ? 30 : 22);
        float h[3] = {P(sj, 0) - P(spine, 0), 0, P(sj, 2) - P(spine, 2)};
        const float hl = std::sqrt(h[0] * h[0] + h[2] * h[2]);
        if (hl < 1e-4f) continue;
        h[0] /= hl, h[2] /= hl;
        const float up[3] = {h[0] * c.carry_out, 1.0f, h[2] * c.carry_out};     // upper arm: up, a bit out
        Aim(sj, ej, up);
        const float fore[3] = {-h[0] * c.carry_in, 1.0f, -h[2] * c.carry_in};  // forearm: up, a bit in
        Aim(ej, wj, fore);
    }
    ctx.esp = esp0;
    g_hands_h[w] = std::max(P(J(22), 1), P(J(30), 1)) - P(0, 1);
    static int told = 0;
    if (told < 4) {
        told++;
        LOGI("coop: carry - P%d's arms up (family %d, pose %08X): wrists %.2f / %.2f m above the root", w + 1, fam, pose,
             P(J(22), 1) - P(0, 1), P(J(30), 1) - P(0, 1));
    }
    __imp__sub_002DFC90(ctx, base);
    std::memcpy(kernel::HostPtr(Wd), saveW, 0x40 * size_t(n));
}

// hc53: a part of a remapped skin (Abomination / Savage Banner on a player whose animation is for another skeleton):
// the player's pose is updated and copied into the skin's own pose joint by joint just before the part is skinned
// (see Remap). 0x2B7900: PolySkin vtable +0x20, thiscall part (2 args).
X86_EXTERN_FUNC(__imp__sub_002B7900);
X86_FUNC(sub_002B7900) { Crumb crumb_("sub_002B7900 skin");
    const uint32_t part = ctx.ecx;
    for (Remap& r : g_remap) {
        const uint32_t clone = r.clone.load();
        if (!Enabled() || !clone || !part) continue;
        uint32_t parts[kMaxParts];
        const int n = Parts(clone, parts);
        bool mine = false;
        for (int i = 0; i < n && !mine; i++) mine = parts[i] == part;
        if (!mine) continue;
        const uint32_t dst = r.pose.load(), obj = r.obj.load();
        const uint32_t src = (obj && R32(obj) == 0x4218C8 && R32(obj + 0x10) == clone) ? R32(obj + 0x18) : 0;
        const int to = r.to.load(), from = r.from.load();
        if (!dst || !src || R32(dst + 0xC) != uint32_t(kJointCount[to]) || R32(src + 0xC) != uint32_t(kJointCount[from]))
            break;
        if (R32(part + 0x60) != dst) Call(ctx, part, R32(R32(part) + 0x28), {dst});  // its own pose, not the player's
        Call(ctx, src, 0x284820, {0});                                                 // the player's, up to date
        if (r.real.load() && r.rest.size() == 0x40 * size_t(kJointCount[to])) {
            // real model: each joint's local rotation from the animation, its own bone length (rest translation),
            // the roots' movement from the animation; the pose's update builds the body down its own hierarchy
            const uint32_t sl = R32(src + 0x14), dl = R32(dst + 0x14);
            if (!sl || !dl) break;
            for (int j = 0; j < kJointCount[to]; j++) {
                uint8_t* d = static_cast<uint8_t*>(kernel::HostPtr(dl + 0x40 * uint32_t(j)));
                const uint8_t* m = static_cast<const uint8_t*>(kernel::HostPtr(sl + 0x40 * uint32_t(kJointMap[to][from][j])));
                std::memcpy(d, m, 0x30);
                std::memcpy(d + 0x30, RootJoint(to, j) ? m + 0x30 : r.rest.data() + 0x40 * size_t(j) + 0x30, 0x10);
            }
            // hc79: the root joint's local matrix becomes the player's root WORLD matrix: the pose update builds
            // joint 0's world from its local (no root matrix passed), so in hc77 the body was built at the world
            // origin (x 0 instead of about -2713) and P2 was drawn far off. Everything below follows joint 0.
            if (const uint32_t sw0 = R32(src + 0x1C))
                std::memcpy(kernel::HostPtr(dl), kernel::HostPtr(sw0 + 0x40 * uint32_t(kJointMap[to][from][0])), 0x40);
            kernel::G<uint8_t>(dst + 0x20) = 0, kernel::G<uint8_t>(dst + 0x22) = 0, kernel::G<uint8_t>(dst + 0x23) = 1;
            // hc77: and build its world matrices now (0x284460 with +0x23 set: locals down the hierarchy); nothing
            // else updates this pose, so in hc76 P2's Abomination was drawn from never-built (empty) matrices
            Call(ctx, dst, 0x284820, {0});
        } else {
            const uint32_t sw = R32(src + 0x1C), dw = R32(dst + 0x1C);
            if (!sw || !dw) break;
            for (int j = 0; j < kJointCount[to]; j++)
                std::memcpy(kernel::HostPtr(dw + 0x40 * uint32_t(j)), kernel::HostPtr(sw + 0x40 * uint32_t(kJointMap[to][from][j])), 0x40);
            kernel::G<uint8_t>(dst + 0x20) = 0, kernel::G<uint8_t>(dst + 0x22) = 0, kernel::G<uint8_t>(dst + 0x23) = 0;
        }
        static int logged = 0;
        if (logged < 2) {
            logged++;
            LOGI("coop: skins - remapped part %08X: %d joints copied from pose %08X into %08X", part, kJointCount[to], src, dst);
        }
        break;
    }
    __imp__sub_002B7900(ctx, base);
}

// hc67: the level / checkpoint load (0x206320, thiscall world stream manager (location), ret 4). It loads the game's
// skin package with 0x203E90 only when the wanted skin (+0xAA4) differs from the loaded one (+0xAC4); while a player
// has a skin picked, +0xAC4 is cleared first so that block always runs, and the 0x203E90 hook loads the players'
// packages right after the game's own (the one place a second load is known to work: hc55-hc66 crashed elsewhere).
std::atomic<bool> g_in_level_load{false};
X86_EXTERN_FUNC(__imp__sub_00206320);
X86_FUNC(sub_00206320) {
    const uint32_t stream = ctx.ecx;
    if (Enabled() && Config().skins_test && stream && (PlayerSkin(0) >= 0 || PlayerSkin(1) >= 0)) {
        g_stream = stream;
        kernel::G<uint8_t>(stream + 0xAC4) = 0;
    }
    g_in_level_load = true;
    g_level_epoch.fetch_add(1);  // hc81
    __imp__sub_00206320(ctx, base);
    g_in_level_load = false;
}
// hc67: the game's load call (0x203E90, thiscall stream (path, wait?, screen?, handle, section, ?, heap), ret 0x1C).
// Right after the level load's "art/global_<skin>.p3d" (into the skin section), the players' packages (SkinPreload).
X86_EXTERN_FUNC(__imp__sub_00203E90);
X86_FUNC(sub_00203E90) {
    const uint32_t stream = ctx.ecx, path = X86_MEM32(ctx.esp + 4), section = X86_MEM32(ctx.esp + 20);
    bool skin_load = false;
    if (Enabled() && Config().skins_test && g_in_level_load.load() && path && stream && section &&
        section == R32(stream + 0xAA0)) {
        char p[40] = {};
        std::memcpy(p, kernel::HostPtr(path), sizeof(p) - 1);
        skin_load = !std::strncmp(p, "art/global_", 11) && std::strncmp(p, "art/global_nonchar", 18) != 0;
    }
    if (skin_load) LogHeapTree("before the game's own skin load");
    if (skin_load) {
        // hc76: the players' packages first, each through this same call (args rewritten in place, then put back)
        static uint8_t* s_base = nullptr;
        static uint32_t s_esp = 0, s_words[8], s_stream = 0;
        s_base = base, s_esp = ctx.esp, s_stream = stream;
        for (int i = 0; i < 8; i++) s_words[i] = R32(s_esp + 4 * i);
        g_issue_load = [](X86Context& c, uint32_t p, uint32_t sec, uint32_t heap) -> uint32_t {
            const uint32_t esp = c.esp, ecx = c.ecx, ebx = c.ebx, esi = c.esi, edi = c.edi, ebp = c.ebp;
            c.esp = s_esp;
            for (int i = 0; i < 8; i++) W32(s_esp + 4 * i, s_words[i]);
            W32(s_esp + 4, p);        // path
            W32(s_esp + 20, sec);     // section
            W32(s_esp + 28, heap);    // heap
            c.ecx = s_stream;
            __imp__sub_00203E90(c, s_base);
            const uint32_t req = c.eax;
            for (int i = 0; i < 8; i++) W32(s_esp + 4 * i, s_words[i]);
            c.esp = esp, c.ecx = ecx, c.ebx = ebx, c.esi = esi, c.edi = edi, c.ebp = ebp;
            LOGI("coop: skins - in-place load returned request %08X", req);
            return req;
        };
        const uint32_t ecx = ctx.ecx;
        SkinPreload(ctx, stream);
        g_issue_load = nullptr;
        ctx.esp = s_esp, ctx.ecx = ecx;
        for (int i = 0; i < 8; i++) W32(s_esp + 4 * i, s_words[i]);
    }
    __imp__sub_00203E90(ctx, base);
    if (skin_load) LogHeapTree("after the game's own skin load");
    if (skin_load) {
        // hc75: not here: the loader fiber is still finishing the game's request (it is released by the caller
        // right after, 0x2065D5 -> 0x263E90); a second request queued now crashed the loader fiber (hc69-74).
        // (hc76 on: the packages are loaded in place before the game's own skin, see g_issue_load)
        (void)0;  // hc76: the packages are loaded before the game's own skin (above)
    }
}
// hc85: the hc69-hc76 loader diagnostics (0x263E90 / 0x265390 / 0x266710 / 0x266950 / 0x2666D0 hooks) removed
