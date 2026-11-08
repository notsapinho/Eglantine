// SPDX-FileCopyrightText: Copyright 2026 Eglantine contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Ported from featureJosh/Recompia (GPL-3.0-or-later), games/pkp/pkp_menu.cpp, which found the
// layout of the game's UI in the same build (main 2.0.1). Its notes, shortened:
//
// The UI is KTGL screen layouts. A layout holds its panes in a list; each pane has a name, a size
// and a world matrix in a 1920x1080 canvas (origin at the centre, y up). The game never hit-tests
// its menus: it moves a cursor part ("...cursor...") next to the selected entry when the D-pad
// moves the selection. So every tick while a menu is up, every layout in use in the UI manager's
// pools is walked and each visible pane's rectangle computed; the entry under the pointer is the
// smallest visible "item" pane holding it, and the selected entry is the item pane nearest the
// game's cursor pane.
//
// Which layouts and panes are on screen comes from hooks on every pane class's draw (each sets
// flag 0x20 on the pane first): pooled layouts are never drawn but still look visible.
//
// Recompia does not move the selection to the pointer: its guess of the selection is read off the
// cursor part, which bobs in some menus, and open-loop D-pad steps from a wrong guess sent the
// selection around the menu. Here a click steers in closed loop instead: one step, wait for the
// menu to redraw, read the selection again, until it is on the clicked entry (then A) or a step
// and time limit runs out (then nothing).

#include "core/pokopia/menu_mouse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/pokopia/hooks.h"
#include "hid_core/frontend/emulated_devices.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"

namespace Core::Pokopia {

namespace {

using Button = Core::HID::NpadButton;

// ---- offsets (Recompia, main 2.0.1) ----
constexpr u64 UiMgr = 0x35aebf8; // [ptr] the UI manager
// Two placeable pools: +0x1030 {entry** data, u64 count} of the entries in use.
//   pool A entry: +8 u32 id, +12 u32 sub, +32 -> {placeable** data, u64 count} = every instance
//     of that layout, +40 -> ring {placeable** data, u64 size, u64 cap, u64 head, u64 count} = the
//     instances not in use.
//   pool B entry: +0 placeable*, +8 u32 id, +12 u32 sub.
constexpr u64 PoolA = 0x84808, PoolB = 0x8c858;
constexpr u64 PoolEntries = 0x1030;
constexpr u64 EntInstances = 32, EntFree = 40;
constexpr u64 MaxPoolEntries = 512, MaxInstances = 64;
constexpr u64 PlaceableObj = 40, ObjLayout = 16; // placeable +40 -> object, +16 -> the layout
// layout: u16 pane count; pane list: node +8 next, +16 pane
constexpr u64 LayPaneCount = 146, LayFirstNode = 32, LaySentinel = 64;
// pane
constexpr u64 PaneName = 176, PaneTag = 184, PaneParent = 208, PaneLayout = 216, PaneWorld = 224,
              PaneSize = 240, PaneFlags = 260, PaneAnchor = 268, PaneNameLen = 272,
              PaneAlpha = 275;
constexpr float CanvasW = 1920.f, CanvasH = 1080.f;
constexpr int MaxPanes = 4096;

// Every pane class's draw, at its flag store (pane in x0, x21 for one).
constexpr std::array<u64, 11> DrawSitesX0{0x1f5ac6c, 0x1f5b26c, 0x1f63094, 0x1f642a8,
                                          0x1f6a028, 0x1f6a8c4, 0x1f76168, 0x1f765b4,
                                          0x1f78124, 0x1f7aa2c, 0x1f7fe48};
constexpr u64 DrawSiteX21 = 0x1f78030;
constexpr u32 DrawnFrames = 3;

// The title's "(-) Settings" key guide (Recompia's menu_key_guides default): a click on it is -.
constexpr u32 KeyGuideSettings = 881468973;

// Steering and taps, in ticks of TickNs.
constexpr auto TickNs = std::chrono::nanoseconds{16'000'000};
constexpr int PressTicks = 3, ReleaseTicks = 2, SettleTicks = 3;
constexpr int MaxSteps = 40, MaxSteerTicks = 200, MaxLostTicks = 12;

Core::System* g_system{};
u64 g_main_base{};
std::shared_ptr<Core::Timing::EventType> g_tick_event;

Core::Memory::Memory& Mem() {
    return g_system->ApplicationMemory();
}

bool Ok(u64 address, u64 size) {
    return address >= 0x1000 && size != 0 && size <= 0x10000 &&
           Mem().IsValidVirtualAddressRange(address, size);
}

template <typename T>
T Rd(u64 address) {
    T value{};
    Mem().ReadBlock(address, &value, sizeof(T));
    return value;
}

u64 RdPtr(u64 address) {
    return Ok(address, 8) ? Rd<u64>(address) : 0;
}

u32 Frame() {
    return static_cast<u32>(Core::g_nx2_present_count.load(std::memory_order_relaxed)) + 1;
}

// ---- the panes the game draws (hooks on the guest's render threads) ----

constexpr std::size_t DrawnSlots = 4096;
std::array<std::atomic<u64>, DrawnSlots> g_drawn_key{};
std::array<std::atomic<u32>, DrawnSlots> g_drawn_frame{};
std::atomic<u64> g_draw_calls{0};

std::size_t DrawnSlot(u64 layout) {
    return static_cast<std::size_t>((layout >> 4) * 0x9e3779b97f4a7c15ULL >> 52) &
           (DrawnSlots - 1);
}

void DrawnMark(u64 layout, u32 frame) {
    std::size_t h = DrawnSlot(layout);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (DrawnSlots - 1)) {
        u64 key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0 && g_drawn_key[h].compare_exchange_strong(key, layout)) {
            key = layout;
        }
        if (key == layout) {
            g_drawn_frame[h].store(frame, std::memory_order_relaxed);
            return;
        }
    }
}

bool DrawnRecent(u64 layout) {
    const u32 now = Frame();
    std::size_t h = DrawnSlot(layout);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (DrawnSlots - 1)) {
        const u64 key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0) {
            return false;
        }
        if (key == layout) {
            return now - g_drawn_frame[h].load(std::memory_order_relaxed) <= DrawnFrames;
        }
    }
    return false;
}

bool DrawHooksLive() {
    return g_draw_calls.load(std::memory_order_relaxed) != 0;
}

// Each drawn pane: the frame, and the world matrix, size and anchor as the draw read them.
struct DrawnPane {
    std::atomic<u64> key{0};
    std::atomic<u32> frame{0};
    float m[16]{};
    u32 size = 0;
    u8 anchor = 0;
};
constexpr std::size_t PaneSlots = 16384;
std::unique_ptr<std::array<DrawnPane, PaneSlots>> g_drawn_panes;

std::size_t PaneSlot(u64 pane) {
    return static_cast<std::size_t>((pane >> 3) * 0x9e3779b97f4a7c15ULL >> 50) & (PaneSlots - 1);
}

// True when the pane was already marked this frame.
bool PaneMark(u64 pane, u32 frame) {
    auto& panes = *g_drawn_panes;
    std::size_t h = PaneSlot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (PaneSlots - 1)) {
        DrawnPane& d = panes[h];
        u64 key = d.key.load(std::memory_order_relaxed);
        if (key == 0 && d.key.compare_exchange_strong(key, pane)) {
            key = pane;
        }
        if (key != pane) {
            continue;
        }
        if (d.frame.load(std::memory_order_relaxed) == frame) {
            return true;
        }
        const u64 world = RdPtr(pane + PaneWorld);
        if (world != 0 && Ok(world, sizeof(d.m))) {
            Mem().ReadBlock(world, d.m, sizeof(d.m));
        }
        d.size = Rd<u32>(pane + PaneSize);
        d.anchor = Rd<u8>(pane + PaneAnchor);
        d.frame.store(world != 0 ? frame : 0, std::memory_order_release);
        return false;
    }
    return false;
}

// Entries and cursors are mostly null panes and parts that hold the drawn pictures and texts
// without a draw of their own, so a drawn pane marks its ancestors too.
void MarkWithAncestors(u64 pane, u32 frame) {
    for (int depth = 0; pane != 0 && depth < 64; ++depth) {
        if (!Ok(pane, PaneAlpha + 1) || PaneMark(pane, frame)) {
            return;
        }
        pane = Rd<u64>(pane + PaneParent);
    }
}

void OnPaneDraw(u64 pane) {
    if (pane == 0 || !Ok(pane, PaneAlpha + 1)) {
        return;
    }
    const u32 frame = Frame();
    thread_local u64 last_layout = 0;
    thread_local u32 last_frame = 0;
    const u64 layout = Rd<u64>(pane + PaneLayout);
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    const u32 flags = Rd<u32>(pane + PaneFlags);
    if ((flags & 0x4000) == 0) {
        // The draw goes on only for panes it shows (0x100, no hidden ancestor, alpha).
        bool shown = (flags & 0x100) != 0 && Rd<u8>(pane + PaneAlpha) != 0;
        for (u64 q = pane; shown && q != 0 && Ok(q, PaneParent + 8);
             q = Rd<u64>(q + PaneParent)) {
            if (Rd<u32>(q + PaneFlags) & 0x40) {
                shown = false;
            }
        }
        if (shown) {
            MarkWithAncestors(pane, frame);
        }
    } else {
        MarkWithAncestors(pane, frame);
    }
    if (layout == 0 || (layout == last_layout && frame == last_frame)) {
        return;
    }
    last_layout = layout;
    last_frame = frame;
    DrawnMark(layout, frame);
}

void HookPaneDrawX0(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    OnPaneDraw(ctx.r[0]);
}

void HookPaneDrawX21(Core::System&, Kernel::Svc::ThreadContext& ctx) {
    OnPaneDraw(ctx.r[21]);
}

// ---- the scan ----

struct Pane {
    u64 ptr = 0;
    u32 lay_id = 0, lay_sub = 0;
    int lay = -1;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // canvas pixels, y down
    u8 alpha = 0;
    u32 flags = 0;
    bool item = false, cursor = false;
    char name[48] = {};
    float cx() const {
        return (x0 + x1) * 0.5f;
    }
    float cy() const {
        return (y0 + y1) * 0.5f;
    }
    float area() const {
        return (x1 - x0) * (y1 - y0);
    }
    bool contains(float x, float y) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    float dist(float x, float y) const {
        const float dx = std::max({x0 - x, 0.f, x - x1});
        const float dy = std::max({y0 - y, 0.f, y - y1});
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Layout {
    u64 ptr = 0;
    u32 id = 0, sub = 0;
    int visible = 0;
};

enum { NoRecord = 0, Stale = 1, Fresh = 2 };

struct State {
    std::vector<Layout> layouts;
    std::vector<Pane> panes;
    int hovered = -1, focused = -1, cursor = -1;
    int mode = 0;
    std::vector<int> group;
    // in-game menus (a layout with the game's cursor on an entry while the game is in play)
    bool ig_up = false;
    int ig_tick = 0, ig_hits = 0, ig_miss = 0;
    bool active = false;
    // mouse buttons at the previous tick
    bool left = false, right = false;
    // taps: the buttons queued, the one held, its ticks left
    std::vector<u64> taps;
    u64 held = 0;
    int held_ticks = 0;
    int gap_ticks = 0;
    // steering toward a clicked entry
    bool steering = false;
    u64 target = 0;
    u32 target_lay = 0;
    float target_x = 0, target_y = 0;
    int steps = 0, steer_ticks = 0, lost_ticks = 0, settle = 0;
    u32 log_lines = 0;
};

State& St() {
    static State state;
    return state;
}

bool LogOk() {
    return ++St().log_lines < 2000;
}

bool Has(const std::string& s, const char* k) {
    return s.find(k) != std::string::npos;
}

void ReadStr(u64 p, std::size_t len_hint, char* out, std::size_t n) {
    out[0] = 0;
    if (p == 0) {
        return;
    }
    std::size_t len = len_hint ? std::min(len_hint, n - 1) : n - 1;
    if (!Ok(p, len)) {
        len = 0;
        while (len < n - 1 && Ok(p + len, 1) && Rd<char>(p + len)) {
            ++len;
        }
    }
    std::array<char, 64> buf{};
    Mem().ReadBlock(p, buf.data(), std::min(len, buf.size()));
    std::size_t i = 0;
    for (; i < len && i < buf.size(); ++i) {
        const char c = buf[i];
        if (c == 0) {
            break;
        }
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}

void Classify(Pane& p, const char* tag) {
    std::string n = p.name;
    n += '|';
    n += tag;
    for (char& c : n) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    p.cursor = Has(n, "cursor") && !Has(n, "mouse");
    static const char* const ItemWords[] = {"btn",  "button", "item",   "cell", "choice", "list",
                                            "tab",  "slot",   "chip",   "select", "menu"};
    static const char* const NotWords[] = {"cursor", "txt",  "text", "icon", "shadow", "_bg",
                                           "bg_",    "frame", "base", "line", "effect", "eff_"};
    bool item = false;
    for (const char* k : ItemWords) {
        item = item || Has(n, k);
    }
    for (const char* k : NotWords) {
        item = item && !Has(n, k);
    }
    p.item = item;
}

// A pane's rectangle on the canvas from its world matrix, size and anchor (the corners the pane
// draw builds).
bool RectFrom(const float* m, u32 size, u8 anchor, Pane* out) {
    const float w = static_cast<float>(size & 0xffff), h = static_cast<float>(size >> 16);
    if (w <= 0.f || h <= 0.f) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) {
            return false;
        }
    }
    const int ah = anchor >> 4, av = anchor & 0xf;
    const float ox = ah == 0 ? w * 0.5f : (ah == 2 ? -w * 0.5f : 0.f);
    const float oy = av == 0 ? -h * 0.5f : (av == 2 ? h * 0.5f : 0.f);
    const float cx = m[12] + m[0] * ox + m[4] * oy, cy = m[13] + m[1] * ox + m[5] * oy;
    const float hx = (std::fabs(m[0]) * w + std::fabs(m[4]) * h) * 0.5f;
    const float hy = (std::fabs(m[1]) * w + std::fabs(m[5]) * h) * 0.5f;
    out->x0 = CanvasW * 0.5f + cx - hx;
    out->x1 = CanvasW * 0.5f + cx + hx;
    out->y0 = CanvasH * 0.5f - cy - hy;
    out->y1 = CanvasH * 0.5f - cy + hy;
    return true;
}

bool PaneRect(u64 pane, Pane* out) {
    if (!Ok(pane, PaneAlpha + 1)) {
        return false;
    }
    const u64 world = Rd<u64>(pane + PaneWorld);
    if (!Ok(world, 64)) {
        return false;
    }
    float m[16];
    Mem().ReadBlock(world, m, sizeof(m));
    if (!RectFrom(m, Rd<u32>(pane + PaneSize), Rd<u8>(pane + PaneAnchor), out)) {
        return false;
    }
    out->alpha = Rd<u8>(pane + PaneAlpha);
    out->flags = Rd<u32>(pane + PaneFlags);
    return true;
}

int PaneRecord(u64 pane, Pane* out) {
    const u32 now = Frame();
    auto& panes = *g_drawn_panes;
    std::size_t h = PaneSlot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (PaneSlots - 1)) {
        const DrawnPane& d = panes[h];
        const u64 key = d.key.load(std::memory_order_relaxed);
        if (key == 0) {
            return NoRecord;
        }
        if (key != pane) {
            continue;
        }
        const u32 f = d.frame.load(std::memory_order_acquire);
        if (f == 0) {
            return NoRecord;
        }
        if (now - f > DrawnFrames) {
            return Stale;
        }
        float m[16];
        std::memcpy(m, d.m, sizeof(m));
        return RectFrom(m, d.size, d.anchor, out) ? Fresh : Stale;
    }
    return NoRecord;
}

// The game's own visibility rule for a pane.
bool PaneShown(u64 pane, u32 flags) {
    if (flags & 0x4000) {
        return true;
    }
    if (!(flags & 0x100)) {
        return false;
    }
    u64 q = pane;
    for (int depth = 0; q != 0 && depth < 64; ++depth) {
        if (!Ok(q + PaneFlags, 4) || (Rd<u32>(q + PaneFlags) & 0x40)) {
            return false;
        }
        q = RdPtr(q + PaneParent);
    }
    return true;
}

void AddLayout(u64 placeable, u32 id, u32 sub) {
    State& s = St();
    const u64 obj = RdPtr(placeable + PlaceableObj);
    const u64 lay = RdPtr(obj + ObjLayout);
    if (lay == 0 || !Ok(lay, LaySentinel + 8)) {
        return;
    }
    for (const Layout& l : s.layouts) {
        if (l.ptr == lay) {
            return;
        }
    }
    const bool live = DrawHooksLive();
    if (live && !DrawnRecent(lay)) {
        return;
    }
    Layout layout{lay, id, sub, 0};
    const int count = Rd<u16>(lay + LayPaneCount);
    u64 node = Rd<u64>(lay + LayFirstNode);
    const int li = static_cast<int>(s.layouts.size());
    struct Cand {
        u64 pane;
        Pane p;
        int rec;
    };
    std::vector<Cand> cands;
    int fresh = 0;
    for (int k = 0; k < count && k < MaxPanes && node != 0 && node != lay + LaySentinel; ++k) {
        if (!Ok(node, 24)) {
            break;
        }
        const u64 pane = Rd<u64>(node + 16);
        node = Rd<u64>(node + 8);
        Pane p;
        if (!PaneRect(pane, &p) || p.alpha == 0 || !PaneShown(pane, p.flags)) {
            continue;
        }
        if (!live && !(p.flags & 0x20)) {
            continue; // never went through a draw
        }
        Cand cd{pane, p, NoRecord};
        if (live) {
            Pane r;
            cd.rec = PaneRecord(pane, &r);
            if (cd.rec == Fresh) {
                ++fresh;
                cd.p.x0 = r.x0;
                cd.p.y0 = r.y0;
                cd.p.x1 = r.x1;
                cd.p.y1 = r.y1;
            }
        }
        cands.push_back(cd);
    }
    // With fresh records for most of the layout's panes, the ones without were not drawn. Some
    // pane classes draw without the hooked draws (the bag's empty slots), so only then.
    const bool refine = live && fresh > 0 && fresh * 2 >= static_cast<int>(cands.size());
    for (Cand& cd : cands) {
        if (refine && cd.rec != Fresh) {
            continue;
        }
        Pane& p = cd.p;
        if (p.x1 < 0.f || p.y1 < 0.f || p.x0 > CanvasW || p.y0 > CanvasH) {
            continue;
        }
        p.ptr = cd.pane;
        p.lay = li;
        p.lay_id = id;
        p.lay_sub = sub;
        ReadStr(Rd<u64>(cd.pane + PaneName), Rd<u16>(cd.pane + PaneNameLen), p.name,
                sizeof(p.name));
        char tag[48];
        ReadStr(RdPtr(cd.pane + PaneTag), 0, tag, sizeof(tag));
        Classify(p, tag);
        ++layout.visible;
        s.panes.push_back(p);
    }
    s.layouts.push_back(layout);
}

bool ReadPool(u64 mgr, u64 pool, u64* data, u64* count) {
    const u64 a = mgr + pool + PoolEntries;
    if (!Ok(a, 16)) {
        return false;
    }
    *data = Rd<u64>(a);
    *count = Rd<u64>(a + 8);
    return *data != 0 && *count != 0 && *count <= MaxPoolEntries && Ok(*data, *count * 8);
}

void Scan() {
    State& s = St();
    s.layouts.clear();
    s.panes.clear();
    const u64 mgr = RdPtr(g_main_base + UiMgr);
    if (mgr == 0) {
        return;
    }
    u64 data = 0, count = 0;
    if (ReadPool(mgr, PoolA, &data, &count)) {
        std::vector<u64> entries(count);
        Mem().ReadBlock(data, entries.data(), count * 8);
        for (const u64 e : entries) {
            if (!Ok(e, 48)) {
                continue;
            }
            const u32 id = Rd<u32>(e + 8), sub = Rd<u32>(e + 12);
            const u64 vec = Rd<u64>(e + EntInstances), ring = Rd<u64>(e + EntFree);
            u64 vdata = 0, vcount = 0;
            if (Ok(vec, 16)) {
                vdata = Rd<u64>(vec);
                vcount = Rd<u64>(vec + 8);
            }
            std::vector<u64> free_ptrs;
            if (Ok(ring, 40)) {
                const u64 rdata = Rd<u64>(ring), rsize = Rd<u64>(ring + 8);
                const u64 rcap = Rd<u64>(ring + 16), rhead = Rd<u64>(ring + 24);
                const u64 rcount = Rd<u64>(ring + 32);
                if (rdata && rsize && rcap && rcount <= MaxInstances && rsize <= 4096 &&
                    Ok(rdata, rsize * 8)) {
                    for (u64 k = 0; k < rcount; ++k) {
                        const u64 idx = std::min((rhead + k) % rcap, rsize - 1);
                        free_ptrs.push_back(Rd<u64>(rdata + 8 * idx));
                    }
                }
            }
            if (vdata == 0 || vcount == 0 || vcount > MaxInstances || !Ok(vdata, vcount * 8)) {
                continue;
            }
            for (u64 k = 0; k < vcount; ++k) {
                const u64 placeable = Rd<u64>(vdata + 8 * k);
                if (placeable == 0 || std::find(free_ptrs.begin(), free_ptrs.end(), placeable) !=
                                          free_ptrs.end()) {
                    continue; // free: not on screen
                }
                AddLayout(placeable, id, sub);
            }
        }
    }
    if (ReadPool(mgr, PoolB, &data, &count)) {
        for (u64 i = 0; i < count; ++i) {
            const u64 e = Rd<u64>(data + 8 * i);
            if (Ok(e, 16)) {
                AddLayout(Rd<u64>(e), Rd<u32>(e + 8), Rd<u32>(e + 12));
            }
        }
    }
}

// A message box's "next" arrow is a cursor pane with no choice behind it.
bool AdvanceMark(const Pane& p) {
    std::string n = p.name;
    for (char& c : n) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return Has(n, "triangle");
}

// Menus outside play build their entries from parts (Recompia's rules 1 and 2).
void PickExtend(const std::vector<int>& items, float big) {
    State& s = St();
    s.mode = 0;
    s.group.clear();
    const int nl = static_cast<int>(items.size());
    if (s.focused >= 0 && s.cursor >= 0) {
        const Pane& cp = s.panes[s.cursor];
        // a frame entry around several entries: the entry of that layout under the cursor
        if (!AdvanceMark(cp)) {
            const Pane& f = s.panes[s.focused];
            int inner = 0, under = -1;
            for (std::size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (static_cast<int>(i) == s.focused || !p.item || p.lay != f.lay ||
                    p.area() >= f.area() || !f.contains(p.cx(), p.cy())) {
                    continue;
                }
                ++inner;
                if (p.contains(cp.cx(), cp.cy()) &&
                    (under < 0 || p.area() < s.panes[under].area())) {
                    under = static_cast<int>(i);
                }
            }
            if (inner >= 2 && under >= 0) {
                s.focused = under;
            }
        }
        // rule 1: the chosen entry belongs to a part placed in an entry of a list layout
        const Pane& f = s.panes[s.focused];
        if (f.lay >= 0 && f.lay < nl && items[f.lay] <= 4) {
            int up = -1;
            for (std::size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (!p.item || p.area() > big || p.lay < 0 || p.lay >= nl || p.lay == f.lay ||
                    items[p.lay] < 2 || !p.contains(f.cx(), f.cy()) ||
                    f.area() > p.area() * 1.5f) {
                    continue;
                }
                if (up < 0 || p.area() < s.panes[up].area()) {
                    up = static_cast<int>(i);
                }
            }
            if (up >= 0) {
                s.focused = up;
                s.mode = 1;
            }
        }
        return;
    }
    // rule 2: sibling instances of one button layout, the cursor in one of them
    if (!DrawHooksLive()) {
        return;
    }
    std::vector<int> rep(s.layouts.size(), -1);
    for (std::size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        if (!p.item || p.area() > big || p.lay < 0 || p.lay >= static_cast<int>(rep.size())) {
            continue;
        }
        if (rep[p.lay] < 0 || p.area() > s.panes[rep[p.lay]].area()) {
            rep[p.lay] = static_cast<int>(i);
        }
    }
    float best = 160.f;
    for (std::size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big || cp.lay < 0 ||
            cp.lay >= static_cast<int>(rep.size())) {
            continue;
        }
        const int mine = rep[cp.lay];
        if (mine < 0) {
            continue;
        }
        const u32 id = s.layouts[cp.lay].id;
        std::vector<int> g;
        int apart = 0;
        for (std::size_t li = 0; li < s.layouts.size(); ++li) {
            if (s.layouts[li].id != id || rep[li] < 0) {
                continue;
            }
            g.push_back(rep[li]);
            const Pane& q = s.panes[rep[li]];
            if (std::fabs(q.cx() - s.panes[mine].cx()) > 8.f ||
                std::fabs(q.cy() - s.panes[mine].cy()) > 8.f) {
                ++apart;
            }
        }
        if (g.size() < 2 || apart == 0) {
            continue;
        }
        const float d = s.panes[mine].dist(cp.cx(), cp.cy());
        if (d > best) {
            continue;
        }
        best = d;
        s.cursor = static_cast<int>(c);
        s.focused = mine;
        s.group = std::move(g);
        s.mode = 2;
    }
}

// The game's cursor and the entry it marks, and the entry under the pointer.
void Pick(float px, float py, bool inside) {
    State& s = St();
    s.hovered = s.focused = s.cursor = -1;
    const float big = CanvasW * CanvasH * 0.5f;
    std::vector<int> items(s.layouts.size(), 0);
    for (const Pane& p : s.panes) {
        if (p.item && p.area() <= big && p.lay >= 0 && p.lay < static_cast<int>(items.size())) {
            ++items[p.lay];
        }
    }
    float best = 1e30f;
    for (std::size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big || cp.lay < 0 ||
            cp.lay >= static_cast<int>(items.size())) {
            continue;
        }
        // The cursor's own layout holds the entries, or the cursor is a layout of its own over
        // another drawn layout's list (the bag's triangle over the inventory).
        const bool own = items[cp.lay] >= 2;
        if (!own && !DrawHooksLive()) {
            continue;
        }
        for (std::size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.item || p.area() > big || p.lay < 0) {
                continue;
            }
            if (own ? p.lay != cp.lay : items[p.lay] < 2) {
                continue;
            }
            if (AdvanceMark(cp) && items[p.lay] < 3) {
                continue;
            }
            // a triangle pointing down ('..._180_..', the bag) points a quarter of its height
            // below its bottom
            const bool down = AdvanceMark(cp) && Has(std::string(cp.name), "180");
            const float qx = cp.cx(), qy = down ? cp.y1 + (cp.y1 - cp.y0) * 0.25f : cp.cy();
            const float d = p.dist(qx, qy) + 0.001f * std::hypot(p.cx() - qx, p.cy() - qy);
            if (d < best ||
                (d == best && s.focused >= 0 && p.area() < s.panes[s.focused].area())) {
                best = d;
                s.cursor = static_cast<int>(c);
                s.focused = static_cast<int>(i);
            }
        }
    }
    if (best > 160.f) {
        s.cursor = s.focused = -1;
    }
    PickExtend(items, big);
    if (!inside) {
        return;
    }
    float hb = 1e30f;
    if (s.mode == 2) {
        for (const int i : s.group) {
            const Pane& p = s.panes[i];
            if (p.contains(px, py) && p.area() < hb) {
                hb = p.area();
                s.hovered = i;
            }
        }
        return;
    }
    for (std::size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        if (!p.contains(px, py) || !p.item || p.area() > big) {
            continue;
        }
        if (s.focused >= 0 && p.lay != s.panes[s.focused].lay) {
            continue; // the entries' layout
        }
        if (p.area() < hb) {
            hb = p.area();
            s.hovered = static_cast<int>(i);
        }
    }
}

bool KeyGuideDrawn(bool camera_live) {
    if (camera_live) {
        return false; // gameplay's HUD draws the same key guide layout
    }
    for (const Layout& l : St().layouts) {
        if (l.visible > 0 && l.id == KeyGuideSettings) {
            return true;
        }
    }
    return false;
}

// ---- input ----

void QueueTap(u64 button) {
    St().taps.push_back(button);
}

// One button at a time: held PressTicks, then released ReleaseTicks.
void RunTaps(Core::HID::HIDCore& hid) {
    State& s = St();
    if (s.held_ticks > 0) {
        if (--s.held_ticks == 0) {
            s.held = 0;
            s.gap_ticks = ReleaseTicks;
        }
    } else if (s.gap_ticks > 0) {
        --s.gap_ticks;
    } else if (!s.taps.empty()) {
        s.held = s.taps.front();
        s.taps.erase(s.taps.begin());
        s.held_ticks = PressTicks;
    }
    hid.SetMenuButtons(s.held);
}

bool TapsIdle() {
    const State& s = St();
    return s.taps.empty() && s.held_ticks == 0 && s.gap_ticks == 0;
}

void StopSteering(const char* why) {
    State& s = St();
    if (s.steering && LogOk()) {
        LOG_INFO(Loader, "Pokopia menu mouse: {} after {} steps", why, s.steps);
    }
    s.steering = false;
}

// One steering decision: A when the selection is on the target, else one D-pad step toward it.
void Steer() {
    State& s = St();
    if (!TapsIdle()) {
        return;
    }
    if (s.settle > 0) {
        --s.settle;
        return;
    }
    if (++s.steer_ticks > MaxSteerTicks || s.steps >= MaxSteps) {
        StopSteering("gave up reaching the clicked entry");
        return;
    }
    // The target in this tick's scan (the same pane, or failing that the pane at its place in
    // its layout).
    int target = -1;
    for (std::size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        if (p.ptr == s.target) {
            target = static_cast<int>(i);
            break;
        }
        if (target < 0 && p.item && p.lay_id == s.target_lay &&
            p.contains(s.target_x, s.target_y)) {
            target = static_cast<int>(i);
        }
    }
    if (s.focused < 0 || target < 0) {
        if (++s.lost_ticks > MaxLostTicks) {
            StopSteering("lost the menu");
        }
        return;
    }
    s.lost_ticks = 0;
    const Pane& f = s.panes[s.focused];
    const Pane& t = s.panes[target];
    if (s.focused == target || t.contains(f.cx(), f.cy())) {
        QueueTap(static_cast<u64>(Button::A));
        StopSteering("reached the clicked entry, A");
        return;
    }
    const float dx = (t.cx() - f.cx()) / std::max(f.x1 - f.x0, 1.f);
    const float dy = (t.cy() - f.cy()) / std::max(f.y1 - f.y0, 1.f);
    Button step;
    if (std::fabs(dx) >= std::fabs(dy)) {
        step = dx > 0 ? Button::Right : Button::Left;
    } else {
        step = dy > 0 ? Button::Down : Button::Up;
    }
    QueueTap(static_cast<u64>(step));
    ++s.steps;
    s.settle = SettleTicks;
}

void OnClick(float px, float py, bool inside, bool camera_live) {
    State& s = St();
    s.steering = false;
    if (inside && KeyGuideDrawn(camera_live)) {
        for (const Pane& p : s.panes) {
            if (p.item && p.lay_id == KeyGuideSettings && p.contains(px, py)) {
                QueueTap(static_cast<u64>(Button::Minus));
                if (LogOk()) {
                    LOG_INFO(Loader, "Pokopia menu mouse: click on key guide '{}' -> -", p.name);
                }
                return;
            }
        }
    }
    if (s.hovered >= 0 && s.focused >= 0 && s.hovered != s.focused) {
        const Pane& h = s.panes[s.hovered];
        s.steering = true;
        s.target = h.ptr;
        s.target_lay = h.lay_id;
        s.target_x = h.cx();
        s.target_y = h.cy();
        s.steps = s.steer_ticks = s.lost_ticks = s.settle = 0;
        if (LogOk()) {
            LOG_INFO(Loader, "Pokopia menu mouse: click on '{}' (selected '{}'): steering to it",
                     h.name, s.panes[s.focused].name);
        }
        return;
    }
    QueueTap(static_cast<u64>(Button::A));
    if (LogOk()) {
        LOG_INFO(Loader, "Pokopia menu mouse: click -> A on '{}'",
                 s.focused >= 0 ? s.panes[s.focused].name : "");
    }
}

// While the game is in play its menus (bag, crafting, shops, choices) draw over the world: a
// layout with the game's cursor on an entry is such a menu. Closed: one scan every PollTicks
// ticks; up after OpenHits positive polls in a row, down after CloseTicks negative ticks.
bool InGameMenu(bool camera_live) {
    State& s = St();
    constexpr int PollTicks = 6, OpenHits = 2, CloseTicks = 10;
    if (!s.ig_up) {
        if (++s.ig_tick < PollTicks) {
            return false;
        }
        s.ig_tick = 0;
        Scan();
        Pick(-1.f, -1.f, false);
    }
    const bool present = (s.cursor >= 0 && s.focused >= 0) || KeyGuideDrawn(camera_live);
    if (!s.ig_up) {
        s.ig_hits = present ? s.ig_hits + 1 : 0;
        if (s.ig_hits >= OpenHits) {
            s.ig_up = true;
            s.ig_miss = 0;
            if (LogOk()) {
                LOG_INFO(Loader, "Pokopia menu mouse: menu up ({} layouts, {} panes)",
                         s.layouts.size(), s.panes.size());
            }
        }
    } else {
        s.ig_miss = present ? 0 : s.ig_miss + 1;
        if (s.ig_miss >= CloseTicks) {
            s.ig_up = false;
            s.ig_hits = 0;
            if (LogOk()) {
                LOG_INFO(Loader, "Pokopia menu mouse: menu closed");
            }
        }
    }
    return s.ig_up;
}

void Tick() {
    State& s = St();
    auto& hid = g_system->HIDCore();
    RunTaps(hid);
    const bool camera_live = hid.IsCameraLive();
    // Out of play (title, settings, pause, dialogues) every screen is a menu; in play, only a
    // detected one. Without the camera hook there is no telling play apart: detected menus only.
    const bool was_up = s.ig_up;
    const bool menu = InGameMenu(camera_live) || (hid.IsMouseCameraHooked() && !camera_live);
    hid.SetMenuUp(menu);
    const auto* devices = hid.GetEmulatedDevices();
    const auto buttons = devices->GetMouseButtons();
    const bool left = buttons.left != 0, right = buttons.right != 0;
    const bool left_click = left && !s.left, right_click = right && !s.right;
    s.left = left;
    s.right = right;
    if (!menu) {
        s.active = false;
        StopSteering("menu closed");
        return;
    }
    s.active = true;
    const auto pos = devices->GetMousePosition();
    const bool inside = pos.x > 0.f && pos.x < 1.f && pos.y > 0.f && pos.y < 1.f;
    const float px = pos.x * CanvasW, py = pos.y * CanvasH;
    if (was_up || !s.ig_up) {
        // up already (or out of play): scan this tick; InGameMenu scanned only when it polled
        Scan();
    }
    Pick(px, py, inside);
    if (right_click) {
        s.steering = false;
        QueueTap(static_cast<u64>(Button::B));
    } else if (left_click) {
        OnClick(px, py, inside, camera_live);
    } else if (s.steering) {
        Steer();
    }
}

} // namespace

bool InstallMenuMouse(Core::System& system, HookInstaller& hooks, u64 main_base) {
    g_system = &system;
    g_main_base = main_base;
    St() = State{};
    if (!g_drawn_panes) {
        g_drawn_panes = std::make_unique<std::array<DrawnPane, PaneSlots>>();
    }
    for (auto& key : g_drawn_key) {
        key.store(0, std::memory_order_relaxed);
    }
    for (auto& pane : *g_drawn_panes) {
        pane.key.store(0, std::memory_order_relaxed);
        pane.frame.store(0, std::memory_order_relaxed);
    }
    g_draw_calls = 0;
    int drawn = hooks.Add(DrawSiteX21, &HookPaneDrawX21, "menu mouse (pane draw)") ? 1 : 0;
    for (const u64 site : DrawSitesX0) {
        drawn += hooks.Add(site, &HookPaneDrawX0, "menu mouse (pane draw)") ? 1 : 0;
    }
    LOG_INFO(Loader, "Pokopia menu mouse: {} of {} pane draw hooks in", drawn,
             DrawSitesX0.size() + 1);

    auto& timing = system.CoreTiming();
    if (g_tick_event) {
        timing.UnscheduleEvent(g_tick_event);
    } else {
        g_tick_event = Core::Timing::CreateEvent(
            "Pokopia menu mouse",
            [](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                if (g_system != nullptr) {
                    Tick();
                }
                return std::nullopt;
            });
    }
    timing.ScheduleLoopingEvent(TickNs, TickNs, g_tick_event);
    return true;
}

} // namespace Core::Pokopia
