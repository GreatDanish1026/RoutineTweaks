// RoutineTweaks: a ReShade add-on for Routine (UE 5.5, DirectX 12) that raises the game's
// lighting, ray tracing and detail settings beyond its menu, live, from an in-game overlay.
// HDR is left to RenoDX.
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, UE 5.5 vtable layout read from the game's own call sites, the same as Directive
// 8020). Settings are kept in ReShade.ini [RoutineTweaks] and applied once the console manager is
// found. If the game re-applies its own settings, the add-on checks its values every second and
// puts back any the game changed. F8 switches all of it off and on for comparison.
//
// No Engine.ini is written; nothing is left behind when the add-on is removed.
// Writes RoutineTweaks.log next to the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Lighting, ray tracing and detail settings beyond " DT_GAME "'s menu, live from the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring file(path, n);
    file.resize(file.find_last_of(L"\\/") + 1);
    file += L"RoutineTweaks.log";
    g_log = _wfopen(file.c_str(), L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. The "Game" choice shows the game's live value; RoutineTweaks.log lists them
// all at connect time (docs/FINDINGS.md).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

std::vector<Option> g_options = {
    // ------------------------------------------------ lighting (what the game renders with)
    // Routine uses classic cascaded and per-light shadow maps (virtual shadow maps are off), SSAO,
    // screen-space reflections and no dynamic GI. Game values in the tips are its highest menu
    // levels (log of 2026-11-01).
    {"Lighting", "Shadows", "SoftShadows", "Soft shadows",
     "Contact-hardening shadows (PCSS): sharp where an object touches a surface, softer\n"
     "further away, like real light. The game uses plain filtered shadows of even softness.\n"
     "Works on the sun and spot lights such as the flashlight. Experimental.",
     L"r.Shadow.FilterMethod",
     {{"Game", 0, {}}, {"Contact hardening", 1, {{L"r.Shadow.FilterMethod", L"1"}}}}},
    {"Lighting", "Shadows", "ShadowDistance", "Shadow draw distance",
     "How far away shadows are drawn: fewer shadows popping in across large rooms.",
     L"r.Shadow.DistanceScale",
     {{"Game", 0, {}}, {"x2", 2, {{L"r.Shadow.DistanceScale", L"2"}}}}},
    {"Lighting", "Ambient occlusion", "Gtao", "Ambient occlusion",
     "Darkening in corners, creases and under objects. GTAO is a more accurate method\n"
     "than the game's SSAO: deeper, better-placed contact shading with less haloing.",
     L"r.AmbientOcclusion.Method",
     {{"Game", 0, {}}, {"GTAO", 1, {{L"r.AmbientOcclusion.Method", L"1"}}}}},
    {"Lighting", "Reflections", "SsrRoughness", "Reflections on rougher surfaces",
     "Screen-space reflections stop at a roughness set per scene. Higher shows them on\n"
     "worn metal, plastic and painted floors too.",
     L"r.SSR.MaxRoughness",
     {{"Game", 0, {}},
      {"0.8", 8, {{L"r.SSR.MaxRoughness", L"0.8"}}},
      {"All (1.0)", 10, {{L"r.SSR.MaxRoughness", L"1"}}}}},
    {"Lighting", "Reflections", "SsrTemporal", "Steadier reflections",
     "Blends reflections over several frames: less noise and shimmer on floors and\n"
     "screens. The game leaves it off.",
     L"r.SSR.Temporal",
     {{"Game", 0, {}}, {"On", 1, {{L"r.SSR.Temporal", L"1"}}}}},
    // Dynamic GI isn't offered: the game took r.DynamicGlobalIlluminationMethod 1 (Lumen) and 2
    // (SSGI) but neither changed the picture (docs/FINDINGS.md).
    // Contact shadows on all lights aren't offered: r.ContactShadows.OverrideLength only covers
    // directional lights, and Routine is almost all interiors (docs/FINDINGS.md).

    // ------------------------------------------------ upscaling
    // DLSS presets from the plugin's own help text: 11 = K, 12 = L, 13 = M (transformer models;
    // need DLSS 310.2 or newer, the user swapped in 310.9.16).
    // The menu has only a resolution-scale slider (r.ScreenPercentage); DLSS's quality modes are
    // fixed scales, so they're set here and held over the slider.
    {"Detail", "DLSS", "DlssMode", "DLSS quality",
     "DLSS's usual quality modes, which the game's menu doesn't offer (it has a resolution\n"
     "scale slider instead). Sets the render resolution and holds it: while a mode is\n"
     "chosen here, the menu slider is overridden. Game uses the slider.\n"
     "DLAA 100%, Quality 66.7%, Balanced 58%, Performance 50%, Ultra Performance 33.3%.",
     L"r.ScreenPercentage",
     {{"Game", 0, {}},
      {"DLAA (100%)", 100, {{L"r.ScreenPercentage", L"100"}}},
      {"Quality (66.7%)", 67, {{L"r.ScreenPercentage", L"66.667"}}},
      {"Balanced (58%)", 58, {{L"r.ScreenPercentage", L"58"}}},
      {"Performance (50%)", 50, {{L"r.ScreenPercentage", L"50"}}},
      {"Ultra Performance (33.3%)", 33, {{L"r.ScreenPercentage", L"33.333"}}}}},
    {"Detail", "DLSS", "DlssModel", "DLSS model",
     "Which DLSS model upscales the image (with DLSS on in the game's menu).\n"
     "K: DLSS 4 transformer, sharp and stable at Quality and Balanced.\n"
     "M: DLSS 4.5 second-generation transformer, best at Performance.\n"
     "L: like M, tuned for Ultra Performance.\n"
     "Needs DLSS 310.2 or newer in the game folder (DLSS Swapper). If the picture doesn't\n"
     "change, switch DLSS quality in the menu once so the game restarts DLSS.",
     L"r.NGX.DLSS.Preset",
     {{"Game", 0, {}},
      {"K (DLSS 4)", 11, {{L"r.NGX.DLSS.Preset", L"11"}}},
      {"M (DLSS 4.5)", 13, {{L"r.NGX.DLSS.Preset", L"13"}}},
      {"L (DLSS 4.5, Ultra Performance)", 12, {{L"r.NGX.DLSS.Preset", L"12"}}}}},

    // ------------------------------------------------ detail
    {"Detail", "Textures", "TexturePool", "Texture memory",
     "Video memory for streamed textures. The game uses 3 GB at its highest setting, so\n"
     "textures can go blurry and reload when you turn around. The RTX 5080 has 16 GB.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"4 GB", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB", 6144, {{L"r.Streaming.PoolSize", L"6144"}}},
      {"8 GB", 8192, {{L"r.Streaming.PoolSize", L"8192"}}}}},
    {"Detail", "Textures", "Anisotropy", "Anisotropic filtering",
     "Sharper floors and walls at shallow angles; the game uses 8x. Textures already loaded\n"
     "keep their filtering until they reload (or the game restarts).",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}, {L"r.VT.MaxAnisotropy", L"16"}}}}},
    {"Detail", "Textures", "MipBias", "Texture sharpness",
     "Uses sharper texture detail levels than the engine picks. Helps textures that look\n"
     "soft after DLSS upscaling; too sharp can shimmer on fine patterns (grates, fabric).",
     L"r.MipMapLODBias",
     {{"Game", 0, {}},
      {"Sharper (-0.5)", 5, {{L"r.MipMapLODBias", L"-0.5"}}},
      {"Sharpest (-1)", 10, {{L"r.MipMapLODBias", L"-1"}}}}},
    {"Detail", "Geometry", "NaniteDetail", "Geometry detail",
     "Triangle size of the detailed (Nanite) models, in pixels. Smaller keeps more detail\n"
     "on distant and mid-range surfaces.",
     L"r.Nanite.MaxPixelsPerEdge",
     {{"Game", 0, {}}, {"Finer (0.5 px)", 1, {{L"r.Nanite.MaxPixelsPerEdge", L"0.5"}}}}},
    {"Detail", "Geometry", "ViewDistance", "Object draw distance",
     "Objects and their detail models switch out further away.",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}}, {"x2", 2, {{L"r.ViewDistanceScale", L"2"}}}}},
    {"Detail", "Post-processing", "Bloom", "Bloom", "Glow around bright lights.",
     L"r.BloomQuality", {{"Game", 0, {}}, {"Off", 2, {{L"r.BloomQuality", L"0"}}}}},
    {"Detail", "Post-processing", "MotionBlur", "Motion blur", "Blur when the camera or objects move fast.",
     L"r.MotionBlurQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.MotionBlurQuality", L"0"}}}}},
    {"Detail", "Post-processing", "Fringe", "Chromatic aberration", "Coloured fringes towards the screen edges.",
     L"r.SceneColorFringeQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.SceneColorFringeQuality", L"0"}}}}},
    {"Detail", "Post-processing", "LensFlares", "Lens flares", "Ghost reflections of bright lights across the screen.",
     L"r.LensFlareQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.LensFlareQuality", L"0"}}}}},
    {"Detail", "Post-processing", "DepthOfField", "Depth of field",
     "Background and foreground blur in close-ups.",
     L"r.DepthOfFieldQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.DepthOfFieldQuality", L"0"}}}}},
    {"Detail", "Post-processing", "FilmGrain", "Film grain",
     "Unreal's film grain noise over the picture. RenoDX may add its own grain on top.",
     L"r.FilmGrain", {{"Game", 0, {}}, {"Off", 1, {{L"r.FilmGrain", L"0"}}}}},
    {"Detail", "Post-processing", "Vignette", "Vignette",
     "Darkened screen corners. Off drops UE's tonemapper to quality 1, which also removes\n"
     "the old-style grain. With RenoDX, its own vignette setting may already cover this.",
     L"r.Tonemapper.Quality", {{"Game", 0, {}}, {"Off", 1, {{L"r.Tonemapper.Quality", L"1"}}}}},
};

bool g_enabled = true;  // F8

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

void LoadSettings() {
    for (Option& o : g_options) {
        reshade::get_config_value(nullptr, DT_PRODUCT, o.key, o.value);
        if (!Chosen(o)) o.value = 0;
    }
}

void SaveSettings() {
    for (const Option& o : g_options) reshade::set_config_value(nullptr, DT_PRODUCT, o.key, o.value);
}

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    return m;
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
int g_sinkFlag = -1;
std::string g_liveError;

std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)
std::set<std::wstring> g_consolePriority;          // values only taken at console priority (logged once)
bool g_dirty = true;                               // options changed: apply at the next present
int g_reapplied = 0;                               // values put back after the game changed them

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

void FindManager() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (%d of %d matches), manager %p", singleton, votes, matches, manager);
    if (!manager || !cvars::HasModuleVtable(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    // r.ScreenPercentage is 0 in Routine (UE 5's "automatic"), so a second float with a positive
    // value confirms float reads: r.ViewDistanceScale (1.0 by default, scaled by the menu).
    void* view = cvars::FindVariable(manager, L"r.ViewDistanceScale");
    float d = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s) && view &&
                 cvars::HasModuleVtable(base, view) && cvars::GetFloat(view, d);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, IsVariableBool at +0x%x, values %d / %g / view "
        "distance %g",
        aniso, screen, cvars::g_isVariableBool, a, s, d);
    if (!reads || a < 1 || a > 16 || !(s == 0 || (s >= 10 && s <= 400)) || !(d > 0.01f && d <= 10)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    g_sinkFlag = cvars::FindSinkFlagOffset(manager);
    Log("console variable sinks flag at +0x%x (now %d)", g_sinkFlag, cvars::ReadSinkFlag(manager, g_sinkFlag));
    g_manager = manager;
    g_live = Live::Ready;
}

bool SetVar(const std::wstring& key, const std::wstring& value) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str())) return false;
    std::wstring now;
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        // Refused: the game set it at a higher priority than SetByCode. Retry as the console.
        if (cvars::Set(v.var, value.c_str(), cvars::kSetByConsole) && Read(key, now) && SameValue(v.type, now, value)) {
            if (g_consolePriority.insert(key).second) Log("%ls = %ls taken at console priority", key.c_str(), value.c_str());
            g_rejected.erase(key);
            return true;
        }
        if (g_rejected.insert(key).second) Log("%ls = %ls not taken (reads %ls)", key.c_str(), value.c_str(), now.c_str());
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's.
void Apply() {
    std::map<std::wstring, std::wstring> want = Wanted();
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) g_originals[key] = old;
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        auto it = g_originals.find(key);
        if (it != g_originals.end() && SetVar(key, it->second)) ++restored;
    }
    g_applied = std::move(want);
    if (g_sinkFlag >= 0) cvars::RequestSinks(g_manager, g_sinkFlag);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
}

// Every second: any value the game changed (a menu change re-applying its settings) becomes the
// new game value, and ours goes back in.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    int fixed = 0;
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        g_originals[key] = cur;
        if (SetVar(key, value)) ++fixed;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (the game set %ls; %d times so far)", key.c_str(), value.c_str(), cur.c_str(), n);
    }
    if (fixed) {
        g_reapplied += fixed;
        if (g_sinkFlag >= 0) cvars::RequestSinks(g_manager, g_sinkFlag);
    }
}

// Every console variable whose name starts with one of <prefixes>, with its value, to the log.
// The names come from the game's read-only data (UTF-16 strings), so nothing is enumerated. One
// pass over the data for all prefixes. Returns how many were found.
int DumpPrefixes(const std::vector<std::string>& prefixes) {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    cvars::Section rdata;
    if (!cvars::FindSection(base, ".rdata", rdata)) return 0;
    std::vector<std::wstring> ps;
    for (const std::string& p : prefixes) ps.emplace_back(p.begin(), p.end());
    std::set<std::wstring> names;
    for (size_t i = 2; i + 8 < rdata.size; i += 2) {
        if (rdata.begin[i - 2] || rdata.begin[i - 1]) continue;
        const wchar_t* s = reinterpret_cast<const wchar_t*>(rdata.begin + i);
        bool match = false;
        for (const std::wstring& p : ps)
            if ((i + p.size() * 2 < rdata.size) && !wmemcmp(s, p.data(), p.size())) {
                match = true;
                break;
            }
        if (!match) continue;
        size_t len = 0;
        while (len < 120 && i + len * 2 + 2 < rdata.size && s[len] > 32 && s[len] < 127) ++len;
        if (s[len] == 0) names.insert(std::wstring(s, len));
    }
    int found = 0;
    for (const std::wstring& name : names) {
        VarInfo v = Lookup(name);
        std::wstring value;
        if (!v.var) continue;
        ++found;
        Log("  %ls (%s) = %ls", name.c_str(), cvars::TypeName(v.type), Read(name, value) ? value.c_str() : L"?");
    }
    std::string list;
    for (const std::string& p : prefixes) list += (list.empty() ? "" : " ") + p;
    Log("%d console variables starting with %s", found, list.c_str());
    return found;
}

// What the first connect writes to the log: everything the options touch or might.
const std::vector<std::string> kProbePrefixes = {
    "sg.", "r.Lumen", "r.RayTracing", "r.Shadow.", "r.Streaming.PoolSize", "r.VolumetricFog", "r.Nanite.MaxPixels",
    "r.ViewDistance", "r.SSR.", "r.AmbientOcclusion", "r.Bloom", "r.MotionBlur", "r.SceneColorFringe",
    "r.Tonemapper", "r.DepthOfField", "r.LensFlare", "r.MaxAnisotropy", "r.AntiAliasingMethod", "r.ScreenPercentage",
    "r.TSR.", "r.NGX.", "r.Streamline.", "r.FidelityFX.", "r.DynamicGlobalIlluminationMethod", "r.ReflectionMethod",
    "r.HDR.", "r.SkyLight", "r.Translucency", "r.SSS.", "r.Fog", "r.Routine", "Routine."};
bool g_probed = false;

void LiveOnPresent() {
    Live state = g_live.load();
    if (state == Live::Idle) {
        g_live = Live::Searching;
        std::thread(FindManager).detach();
        return;
    }
    if (state != Live::Ready) return;
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys = {L"r.RayTracing", L"r.GenerateMeshDistanceFields", L"r.AllowStaticLighting",
                                       L"r.Shadow.Virtual.Enable", L"r.Lumen.Supported",
                                       L"r.DynamicGlobalIlluminationMethod", L"r.ReflectionMethod",
                                       L"r.AntiAliasingMethod", L"r.ScreenPercentage"};
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s) = %ls", key.c_str(), cvars::TypeName(info.type), Read(key, v) ? v.c_str() : L"(not found)");
        }
        if (!g_probed) {  // first connect of this version: everything relevant, to tune the options
            g_probed = true;
            DumpPrefixes(kProbePrefixes);
        }
    }
    if (g_dirty) {
        g_dirty = false;
        Apply();
    }
    Hold();
}

// The game's value of <name> as text: the one we saved if we're overriding it, else the current.
std::string GameValue(const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = g_originals.find(name);
    if (it != g_originals.end() && g_applied.count(name)) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

int ReadInt(const wchar_t* name) {
    std::wstring v;
    return g_live.load() == Live::Ready && Read(name, v) ? _wtoi(v.c_str()) : -1;
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality options (option key -> choice id); every other quality option goes
// to Game. Options in no preset (post-processing taste) are left alone. There's only Cinematic:
// an Ultra+ measured about 1% and wasn't worth a separate step (2026-11-01). The preset shown is derived from the options, "Custom" when none matches.

// Costs measured 2026-11-01 with "Measure preset costs": RTX 5080, 4K, DLSS at 70%, no Frame
// Generation, starting room. Game 106.6 / Cinematic 103.4 fps (docs/FINDINGS.md).
struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

const QualityPreset kPresets[] = {
    {"Game", "", "The game's own settings: everything below at Game.", {}},
    {"Cinematic", "about 3% slower",
     "Everything at its highest: contact-hardening shadows drawn twice as far,\n"
     "GTAO, reflections on rougher surfaces, 8 GB textures, finer geometry and longer\n"
     "draw distance.",
     {{"SoftShadows", 1}, {"ShadowDistance", 2}, {"Gtao", 1}, {"SsrRoughness", 8},
      {"SsrTemporal", 1}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"NaniteDetail", 1}, {"ViewDistance", 2}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

// The options presets control: every key any preset names.
std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, id] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (auto& [k, id] : p.values)
        if (key == k) return id;
    return 0;
}

// The preset the options match, or -1 (Custom).
int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys) {
            const Option* o = FindOption(key.c_str());
            if (o && o->value != PresetValue(kPresets[i], key)) match = false;
        }
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = PresetValue(kPresets[index], key);
    g_dirty = true;
}

// ---------------------------------------------------------------- frame time
//
// Present-to-present times while the game has focus. With DLSS Frame Generation on, every
// other present is a generated frame, so the rate shown is the displayed one (2x the rendered).

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

// The average, not the median: with Frame Generation the present intervals alternate short and
// long, and the median picks one side (it showed Ultra+ faster than Game).
float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// Cost measurement: each preset in turn, settle then sample; then the options as they were.
constexpr ULONGLONG kSettleMs = 5000, kSampleMs = 8000;
struct Measure {
    bool running = false;
    int stage = 0;  // preset index being measured
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;  // option values before
    float resultMs[kPresetCount] = {};
    bool done = false;
    std::string note;
} g_measure;

void StartMeasure() {
    g_measure = {};
    g_measure.running = true;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stageStart = GetTickCount64();
    ApplyPreset(0);
    Log("measure: started");
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= kSettleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < kSampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", kPresets[g_measure.stage].label, avg, 1000.0f / avg,
        g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < kPresetCount) {
        ApplyPreset(g_measure.stage);
        return;
    }
    g_measure.done = true;
    char line[200];
    int n = snprintf(line, sizeof(line), "measure: result");
    for (int i = 1; i < kPresetCount; ++i)
        n += snprintf(line + n, sizeof(line) - n, ", %s %+.0f%% frame time", kPresets[i].label,
                      (g_measure.resultMs[i] / g_measure.resultMs[0] - 1) * 100);
    Log("%s", line);
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- present

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb  ? "sRGB"
                                                                               : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled;
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(pid == GetCurrentProcessId());
    LiveOnPresent();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// A marker before a control whose value differs from the game's.
void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

std::string ChoiceLabel(const Option& o, const Choice& c) {
    if (c.id != 0) return c.label;
    std::string v = GameValue(o.shown);
    return v.empty() ? "Game" : "Game (" + v + ")";
}

// One option's combo; true if it changed.
bool DrawOption(Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

// All options of a tab, grouped under their section headings; true if one changed.
bool DrawTab(const char* tab) {
    bool changed = false;
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        changed |= DrawOption(o);
    }
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

char g_cvarName[128] = "r.Shadow.FilterMethod";
char g_cvarValue[64] = "";
std::string g_cvarResult;
char g_dumpPrefix[64] = "r.Shadow.";

void DumpPrefix(const char* prefix) {
    g_cvarResult = std::to_string(DumpPrefixes({prefix})) + " settings written to RoutineTweaks.log";
}

void DrawAdvanced() {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Holding %zu settings; put back %d times after the game changed them", g_applied.size(),
                        g_reapplied);
    if (!g_rejected.empty()) {
        std::string list;
        for (const std::wstring& k : g_rejected) list += (list.empty() ? "" : ", ") + Narrow(k);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
    }
    ImGui::TextDisabled("Log: RoutineTweaks.log next to the game's exe");
    if (live != Live::Ready) return;

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s%s", cvars::TypeName(v.type), Read(name, value) ? Narrow(value).c_str() : "?",
                            g_applied.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        bool ok = SetVar(name, val);
        Log("console: %ls = %ls %s", name.c_str(), val.c_str(), ok ? "set" : "NOT taken");
        g_cvarResult = ok ? "Set (until the game or an option changes it)" : "Not taken";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value now. It isn't saved, and the game's menu or an\n"
         "option above can change it again.");
    ImGui::SetNextItemWidth(ControlWidth());
    ImGui::InputText("##prefix", g_dumpPrefix, sizeof(g_dumpPrefix));
    ImGui::SameLine();
    if (ImGui::Button("Write all to log")) DumpPrefix(g_dumpPrefix);
    Help("Writes every console variable whose name starts with this text, with its value,\n"
         "to RoutineTweaks.log.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");

    bool changed = false;
    const bool measuring = g_measure.running;

    // Quality preset, live frame rate and cost measurement.
    ImGui::Spacing();
    ImGui::BeginDisabled(measuring || live != Live::Ready);
    const int current = CurrentPreset();
    ChangedMark(current != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
        for (int i = 0; i < kPresetCount; ++i) {
            std::string item = kPresets[i].label;
            if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
            if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                ApplyPreset(i);
                changed = true;
                Log("preset: %s", kPresets[i].label);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s%s", kPresets[i].tip,
                                  *kPresets[i].cost ? "\n\nMeasured on an RTX 5080 at 4K with DLSS in one scene:\n"
                                                      "heavier scenes may cost more. Yours may differ."
                                                    : "");
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Help("Sets the lighting and detail options together. Measure preset costs shows what\n"
         "it costs on your PC.\n"
         "The post-processing switches are left as they are. Changing an option afterwards\n"
         "shows Custom.");
    ImGui::SameLine();
    float ms = Mean(g_frameMs);
    if (ms > 0) ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);

    if (measuring) {
        ImGui::TextColored(kAccent, "Measuring %s (%d of %d)... keep the camera still.", kPresets[g_measure.stage].label,
                           g_measure.stage + 1, kPresetCount);
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
    } else {
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure preset costs")) StartMeasure();
        ImGui::EndDisabled();
        Help("Runs Game and Cinematic for 13 s each (5 s to settle, 8 s measured) and shows\n"
             "how much longer each frame takes with Cinematic. Stand still in a typical scene and\n"
             "leave the mouse alone; the overlay can stay open. Your options come back afterwards.\n"
             "With Frame Generation on, the frame rate is the displayed one.");
        if (g_measure.done) {
            ImGui::SameLine();
            std::string r = "Game " + std::to_string(static_cast<int>(1000.0f / g_measure.resultMs[0] + 0.5f)) + " fps";
            for (int i = 1; i < kPresetCount; ++i) {
                char b[80];
                snprintf(b, sizeof(b), ", %s %.0f fps (%+.0f%% frame time)", kPresets[i].label,
                         1000.0f / g_measure.resultMs[i], (g_measure.resultMs[i] / g_measure.resultMs[0] - 1) * 100);
                r += b;
            }
            ImGui::TextDisabled("%s", r.c_str());
        } else if (!g_measure.note.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Lighting")) {
            changed |= DrawTab("Lighting");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Detail")) {
            changed |= DrawTab("Detail");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            LoadSettings();
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_overlay(DT_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
