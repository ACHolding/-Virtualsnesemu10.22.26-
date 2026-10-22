// kondovsnesemu0.1.1a.cpp — Kondo vsnes 0.1.1a by Mr. Kondo : Virtual SNES Emulator, single-file C++ port of
// virtualsnesemu1.x.py (vsnes 0.1).  A.C Holdings / Team Flames.
//
// Everything lives in this one file: 65C816 CPU, SPC700 sound CPU + HLE IPL, Snes9x S-DSP
// (blargg snes_spc 0.9.0 DSP, LGPL 2.1+, ported line-for-line), LoROM/HiROM/ExHiROM bus,
// PPU (modes 0-7, sprites, windows, colour math, per-scanline raster/HDMA), DMA + HDMA,
// H/V IRQ, NMI, mul/div, auto-joypad, SRAM, save states, SA-1, DSP-1, SuperFX/GSU, OBC1,
// S-RTC, S-DD1, CX4, ST010/ST011, SPC7110, SGB/ICD2 bridge, built-in demo cart, ROM library
// (.sfc/.smc/.zip with a built-in inflater), SingleStepTests runner, optional SNES9x
// libretro host, and a classic File/CPU/Options/Help SDL2 desktop GUI.
//
// Build (GUI, needs SDL2 dev headers):
//   g++ -O2 -std=c++17 kondovsnesemu0.1.1a.cpp -o vsnes $(sdl2-config --cflags --libs) -ldl -pthread
// Build (headless only, no dependencies):
//   g++ -O2 -std=c++17 -DVSNES_NO_SDL kondovsnesemu0.1.1a.cpp -o vsnes -ldl
// Windows (MinGW): g++ -O2 -std=c++17 kondovsnesemu0.1.1a.cpp -o vsnes.exe -lmingw32 -lSDL2main -lSDL2 -lcomdlg32
//
// Run:      vsnes [game.sfc|game.smc|game.zip]        (no ROM -> built-in demo)
//           vsnes --roms ~/ROMs/SNES                   (set commercial library folder)
// GUI:      600x400 window laid out like the classic Snes9x Win32 front-end: File / Emulation /
//           Input / Sound / Video / Cheat / Netplay / Help, Open ROM / Rom Info / Input / Hotkey /
//           Sound / Display / Emulator Settings dialogs, messages + frame rate over the picture.
// Joypad 1: Arrows, Start=Space, Select=Enter, A=V B=C X=D Y=X L=A R=S (Snes9x defaults;
//           Joypad 2: IJKL, Start=P Select=O A=H B=G X=T Y=F L=Y R=U). Rebind: Alt+F7.
// Hotkeys:  F1-F10 load slot 0-9, Shift+F1-F10 save, Pause, \ frame advance, Tab fast forward,
//           = / - speed, F12 screenshot, Ctrl+Alt+R reset (rebind: Alt+F9). Ctrl+O load game,
//           Esc hide menubar, Alt+Enter full screen, Alt+F4 exit, Alt = menu keyboard navigation.
// Tests:    vsnes --headless 300 --shot out.png --wav out.wav [rom]
//           vsnes --cputest DIR [--per-file N]    (SingleStepTests 65816 / spc700 JSON)
//
// No game data is bundled. Not affiliated with or endorsed by Nintendo.
//
// What's new in 0.1.1a, the C++ edition (vs the Python virtualsnesemu1.x.py it was ported from)
//   * Whole emulator in one C++17 file; no Python, numpy, Cython or pygame needed.
//   * Verified bit-identical to the Python virtualsnesemu1.x.py: demo cart bytes, every video frame and audio sample
//     of headless runs, 64 random PPU states (all modes, windows, colour math, mode 7, OBJ),
//     and random chip-hammering carts for SA-1, DSP-1, SuperFX, OBC1, S-DD1, CX4, ST010,
//     SPC7110 and SGB. The optional SNES9x libretro host matches too (video + resampled audio).
//   * 65816 fixes over the Python core: JMP (a,X) / JSR (a,X) pointers wrap inside the program bank,
//     and 16-bit direct-page / stack-relative accesses wrap inside bank 0. SingleStepTests:
//     65816 5,080,400/5,080,400 and SPC700 256,000/256,000 pass.
//   * DSP-1 HLE: two out-of-range accesses that made the Python version raise IndexError now read 0 /
//     drop the extra word instead.
//   * libretro host pads the ROM buffer: the snes9x core reads data+$FFC0 even for 32 KB ROMs.
//   * Save states use a new binary format (.vs00-.vs99: bank*10+slot); Python .vs0 pickles do not load.
//
// Honest limits (inherited): mosaic, offset-per-tile (modes 2/4/6), EXTBG and ST018 are not
// implemented; the PPU renders once per frame from per-scanline register snapshots; the chip
// HLEs are approximations from public docs; the vsnes "--dsp numpy" approximate engine is not
// ported (the Snes9x S-DSP port is always used). PNGs are written uncompressed (no zlib).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfenv>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <memory>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <functional>
#include <map>
#include <ctime>
#include <stdexcept>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  include <shlobj.h>
#else
#  include <dlfcn.h>
#endif

#if !defined(VSNES_NO_SDL)
#  if __has_include(<SDL2/SDL.h>)
#    define SDL_MAIN_HANDLED
#    include <SDL2/SDL.h>
#    define VSNES_HAVE_SDL 1
#  elif __has_include(<SDL.h>)
#    define SDL_MAIN_HANDLED
#    include <SDL.h>
#    define VSNES_HAVE_SDL 1
#  endif
#endif

namespace fs = std::filesystem;
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int64_t i64; typedef uint64_t u64;

static const char* APP = "Kondo vsnes 0.1.1a";
static const char* WINDOW_TITLE = "Kondo vsnes 0.1.1a";
static const char* COPY = "Kondo vsnes 0.1.1a by Mr. Kondo - Virtual SNES Emulator (C++)";

// ---------------------------------------------------------------- helpers
static const double CL_PI = 3.14159265358979323846;
static inline i64 floordiv(i64 a, i64 b) { i64 q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
static inline i64 floormod(i64 a, i64 b) { i64 m = a % b; if (m != 0 && ((m < 0) != (b < 0))) m += b; return m; }
static inline int s16v(int v) { v &= 0xFFFF; return v >= 0x8000 ? v - 0x10000 : v; }
static inline int u16v(i64 v) { return (int)(v & 0xFFFF); }
static inline int pyround(double x) { int old = std::fegetround(); std::fesetround(FE_TONEAREST); double r = std::nearbyint(x); std::fesetround(old); return (int)r; }

// ---------------------------------------------------------------- timing
static const int LINE_CYC = 170;     // 1364 master clocks / ~8 per CPU cycle
static const int HBLANK_CYC = 137;
static const int LINES = 262;
static const int VIS = 224;

// ---------------------------------------------------------------- bus interface
struct Bus {
    u64 wc = 0;                         // write counter (idle-loop skip key)
    virtual u8 read(u32 a) = 0;
    virtual void write(u32 a, u8 v) = 0;
    virtual ~Bus() {}
};

// ---------------------------------------------------------------- 65C816 base cycle table
static const u8 CYC[256] = {
    7,6,7,4,5,3,5,6,3,2,2,4,6,4,6,5, 2,5,5,7,5,4,6,6,2,4,2,2,6,4,7,5,
    6,6,8,4,3,3,5,6,4,2,2,5,4,4,6,5, 2,5,5,7,4,4,6,6,2,4,2,2,4,4,7,5,
    6,6,2,4,7,3,5,6,3,2,2,3,3,4,6,5, 2,5,5,7,7,4,6,6,2,4,3,2,4,4,7,5,
    6,6,6,4,3,3,5,6,4,2,2,6,5,4,6,5, 2,5,5,7,4,4,6,6,2,4,4,2,6,4,7,5,
    3,6,4,4,3,3,3,6,2,2,2,3,4,4,4,5, 2,6,5,7,4,4,4,6,2,5,2,2,4,5,5,5,
    2,6,2,4,3,3,3,6,2,2,2,4,4,4,4,5, 2,5,5,7,4,4,4,6,2,4,2,2,4,4,4,5,
    2,6,3,4,3,3,5,6,2,2,2,3,4,4,6,5, 2,5,5,7,6,4,6,6,2,4,3,3,6,4,7,5,
    2,6,3,4,3,3,5,6,2,2,2,3,4,4,6,5, 2,5,5,7,5,4,6,6,2,4,4,2,8,4,7,5};

// addressing modes
enum { M_IMM, M_DP, M_DPX, M_DPY, M_AB, M_ABX, M_ABY, M_LG, M_LGX, M_IND, M_INDX, M_INDY,
       M_INDL, M_INDLY, M_SR, M_SRY };

static signed char G1MODE[32];
static void init_g1() {
    for (auto& m : G1MODE) m = -1;
    G1MODE[0x01] = M_INDX; G1MODE[0x03] = M_SR; G1MODE[0x05] = M_DP; G1MODE[0x07] = M_INDL;
    G1MODE[0x09] = M_IMM; G1MODE[0x0D] = M_AB; G1MODE[0x0F] = M_LG; G1MODE[0x11] = M_INDY;
    G1MODE[0x12] = M_IND; G1MODE[0x13] = M_SRY; G1MODE[0x15] = M_DPX; G1MODE[0x17] = M_INDLY;
    G1MODE[0x19] = M_ABY; G1MODE[0x1D] = M_ABX; G1MODE[0x1F] = M_LGX;
}

struct CPU65816 {
    Bus* bus;
    int A = 0, X = 0, Y = 0, S = 0x01FF, D = 0, DB = 0, PB = 0, PC = 0, P = 0x34, E = 1;
    i64 cyc = 0, target = 0;
    bool waiting = false, stopped = false, nmi_pending = false, irq_line = false;
    // idle-loop key (npc, A, X, Y, P, bus.wc)
    bool ik_valid = false; int ik_pc = 0, ik_a = 0, ik_x = 0, ik_y = 0, ik_p = 0; u64 ik_wc = 0;
    bool b0wrap = false;   // last effective address is a direct-page / stack one: 16-bit accesses wrap in bank 0

    explicit CPU65816(Bus* b) : bus(b) {}

    inline int rd(u32 a) { return bus->read(a); }
    inline void wr(u32 a, int v) { bus->write(a, (u8)v); }

    void reset() {
        E = 1; P = 0x34; D = 0; DB = 0; PB = 0; S = 0x01FF; X &= 0xFF; Y &= 0xFF;
        int lo = rd(0xFFFC); int hi = rd(0xFFFD); PC = lo | (hi << 8);
        waiting = stopped = false; nmi_pending = irq_line = false;
    }

    inline int f8() { int v = rd((PB << 16) | PC); PC = (PC + 1) & 0xFFFF; return v; }
    inline int f16() { int b = PB << 16, pc = PC; int lo = rd(b | pc); int hi = rd(b | ((pc + 1) & 0xFFFF)); PC = (pc + 2) & 0xFFFF; return lo | (hi << 8); }
    inline int f24() { int v = f16(); return v | (f8() << 16); }
    inline void push8(int v) { wr(S, v & 0xFF); S = E ? (0x100 | ((S - 1) & 0xFF)) : ((S - 1) & 0xFFFF); }
    inline int pull8() { S = E ? (0x100 | ((S + 1) & 0xFF)) : ((S + 1) & 0xFFFF); return rd(S); }
    inline void push16(int v) { push8(v >> 8); push8(v); }
    inline int pull16() { int lo = pull8(); return lo | (pull8() << 8); }
    inline void npush8(int v) { wr(S, v & 0xFF); S = (S - 1) & 0xFFFF; }
    inline void npush16(int v) { npush8(v >> 8); npush8(v); }
    inline int npull8() { S = (S + 1) & 0xFFFF; return rd(S); }
    inline int npull16() { int lo = npull8(); return lo | (npull8() << 8); }
    inline void nfix() { if (E) S = 0x100 | (S & 0xFF); }

    void setP(int v) { if (E) v |= 0x30; P = v & 0xFF; if (v & 0x10) { X &= 0xFF; Y &= 0xFF; } }

    void interrupt(int vn, int ve, bool brk = false) {
        if (!E) push8(PB);
        push16(PC);
        int p = P;
        if (E) p = (p & ~0x10) | (brk ? 0x10 : 0) | 0x20;
        push8(p);
        P = (P | 0x04) & ~0x08;
        PB = 0;
        int v = E ? ve : vn;
        int lo = rd(v); int hi = rd(v + 1); PC = lo | (hi << 8);
        cyc += 8;
    }

    void run(i64 tgt) {
        target = tgt;
        while (cyc < tgt) {
            if (nmi_pending || irq_line) {
                if (nmi_pending) { nmi_pending = false; waiting = false; interrupt(0xFFEA, 0xFFFA); }
                else if (!(P & 4)) { waiting = false; interrupt(0xFFEE, 0xFFFE); }
                else if (waiting) waiting = false;
            }
            if (waiting || stopped) { cyc = tgt; return; }
            int op = rd((PB << 16) | PC);
            PC = (PC + 1) & 0xFFFF;
            cyc += CYC[op];
            exec(op);
        }
    }

    // ---- memory helpers
    inline int r16(u32 a) { int lo = rd(a); int hi = rd((a + 1) & 0xFFFFFF); return lo | (hi << 8); }
    inline void w16(u32 a, int v) { wr(a, v & 0xFF); wr((a + 1) & 0xFFFFFF, (v >> 8) & 0xFF); }
    inline int r16z(u32 a) { int lo = rd(a); int hi = rd((a + 1) & 0xFFFF); return lo | (hi << 8); }
    inline int dpi(int n) { int d = D; return (E && !(d & 0xFF)) ? (d | (n & 0xFF)) : ((d + n) & 0xFFFF); }
    inline int dptr(int n) {
        int d = D;
        if (E && !(d & 0xFF)) { int lo = rd(d | (n & 0xFF)); int hi = rd(d | ((n + 1) & 0xFF)); return lo | (hi << 8); }
        return r16z((d + n) & 0xFFFF);
    }
    // 16-bit access at an effective address; direct page / stack-relative wrap inside bank 0
    // (fix over the Python core, which let them carry into bank 1; caught by SingleStepTests c4.n #1616)
    inline int r16e(u32 a) { return b0wrap ? r16z(a) : r16(a); }
    inline void w16e(u32 a, int v) { if (b0wrap) { wr(a, v & 0xFF); wr((a + 1) & 0xFFFF, (v >> 8) & 0xFF); } else w16(a, v); }
    u32 ea(int m) {
        b0wrap = (m == M_DP || m == M_DPX || m == M_DPY || m == M_SR);
        switch (m) {
        case M_DP: return (D + f8()) & 0xFFFF;
        case M_DPX: { int o = f8(); return dpi(o + X); }
        case M_DPY: { int o = f8(); return dpi(o + Y); }
        case M_AB: return (DB << 16) | f16();
        case M_ABX: return (((DB << 16) | f16()) + X) & 0xFFFFFF;
        case M_ABY: return (((DB << 16) | f16()) + Y) & 0xFFFFFF;
        case M_LG: return f24();
        case M_LGX: return (f24() + X) & 0xFFFFFF;
        case M_IND: return (DB << 16) | dptr(f8());
        case M_INDX: { int o = f8(); return (DB << 16) | dptr(o + X); }
        case M_INDY: return (((DB << 16) | dptr(f8())) + Y) & 0xFFFFFF;
        case M_INDL: { int p = (D + f8()) & 0xFFFF; int lo = r16z(p); int hi = rd((p + 2) & 0xFFFF); return lo | (hi << 16); }
        case M_INDLY: { int p = (D + f8()) & 0xFFFF; int lo = r16z(p); int hi = rd((p + 2) & 0xFFFF); return ((lo | (hi << 16)) + Y) & 0xFFFFFF; }
        case M_SR: return (S + f8()) & 0xFFFF;
        case M_SRY: { int o = f8(); return (((DB << 16) | r16z((S + o) & 0xFFFF)) + Y) & 0xFFFFFF; }
        }
        return 0;
    }
    inline int getM(int m) {
        if (m == M_IMM) return (P & 0x20) ? f8() : f16();
        u32 a = ea(m); return (P & 0x20) ? rd(a) : r16e(a);
    }
    inline int getX(int m) {
        if (m == M_IMM) return (P & 0x10) ? f8() : f16();
        u32 a = ea(m); return (P & 0x10) ? rd(a) : r16e(a);
    }
    inline void nz8(int r) { P = (P & 0x7D) | (r & 0x80) | (r ? 0 : 2); }
    inline void nz16(int r) { P = (P & 0x7D) | ((r >> 8) & 0x80) | (r ? 0 : 2); }

    void adc8(int v, bool sbc) {
        int p = P, c = p & 1, a = A & 0xFF, r;
        if (sbc) v ^= 0xFF;
        if (p & 8) {
            r = (a & 0xF) + (v & 0xF) + c;
            if (sbc) { if (r <= 0xF) r -= 6; } else if (r > 9) r += 6;
            r = (a & 0xF0) + (v & 0xF0) + (r > 0xF ? 0x10 : 0) + (r & 0xF);
        } else r = a + v + c;
        int ov = ~(a ^ v) & (a ^ r) & 0x80;
        if (p & 8) { if (sbc) { if (r <= 0xFF) r -= 0x60; } else if (r > 0x9F) r += 0x60; }
        int cf = r > 0xFF ? 1 : 0;
        r &= 0xFF;
        A = (A & 0xFF00) | r;
        P = (p & 0x3C) | (ov ? 0x40 : 0) | cf | (r & 0x80) | (r ? 0 : 2);
    }
    void adc16(int v, bool sbc) {
        int p = P, c = p & 1, a = A, r;
        if (sbc) v ^= 0xFFFF;
        if (p & 8) {
            r = (a & 0xF) + (v & 0xF) + c;
            if (sbc) { if (r <= 0xF) r -= 6; } else if (r > 9) r += 6;
            r = (a & 0xF0) + (v & 0xF0) + (r > 0xF ? 0x10 : 0) + (r & 0xF);
            if (sbc) { if (r <= 0xFF) r -= 0x60; } else if (r > 0x9F) r += 0x60;
            r = (a & 0xF00) + (v & 0xF00) + (r > 0xFF ? 0x100 : 0) + (r & 0xFF);
            if (sbc) { if (r <= 0xFFF) r -= 0x600; } else if (r > 0x9FF) r += 0x600;
            r = (a & 0xF000) + (v & 0xF000) + (r > 0xFFF ? 0x1000 : 0) + (r & 0xFFF);
        } else r = a + v + c;
        int ov = ~(a ^ v) & (a ^ r) & 0x8000;
        if (p & 8) { if (sbc) { if (r <= 0xFFFF) r -= 0x6000; } else if (r > 0x9FFF) r += 0x6000; }
        int cf = r > 0xFFFF ? 1 : 0;
        r &= 0xFFFF;
        A = r;
        P = (p & 0x3C) | (ov ? 0x40 : 0) | cf | ((r >> 8) & 0x80) | (r ? 0 : 2);
    }
    void cmpv(int a, int v, bool wide) {
        if (wide) { int r = (a - v) & 0xFFFF; P = (P & 0x7C) | (a >= v ? 1 : 0) | ((r >> 8) & 0x80) | (r ? 0 : 2); }
        else { a &= 0xFF; int r = (a - v) & 0xFF; P = (P & 0x7C) | (a >= v ? 1 : 0) | (r & 0x80) | (r ? 0 : 2); }
    }
    void BIT(int v, bool imm) {
        bool z;
        if (P & 0x20) { z = (A & v & 0xFF) == 0; if (!imm) P = (P & 0x3F) | (v & 0xC0); }
        else { z = (A & v) == 0; if (!imm) P = (P & 0x3F) | ((v >> 8) & 0xC0); }
        P = (P & 0xFD) | (z ? 2 : 0);
    }
    // rmw kinds: 0 asl 1 rol 2 lsr 3 ror 4 inc 5 dec 6 tsb 7 trb
    int alu8(int k, int v) {
        int r;
        switch (k) {
        case 0: r = (v << 1) & 0xFF; P = (P & 0x7C) | (v >> 7) | (r & 0x80) | (r ? 0 : 2); return r;
        case 1: r = ((v << 1) | (P & 1)) & 0xFF; P = (P & 0x7C) | (v >> 7) | (r & 0x80) | (r ? 0 : 2); return r;
        case 2: r = v >> 1; P = (P & 0x7C) | (v & 1) | (r ? 0 : 2); return r;
        case 3: r = (v >> 1) | ((P & 1) << 7); P = (P & 0x7C) | (v & 1) | (r & 0x80) | (r ? 0 : 2); return r;
        case 4: r = (v + 1) & 0xFF; nz8(r); return r;
        case 5: r = (v - 1) & 0xFF; nz8(r); return r;
        case 6: P = (P & 0xFD) | ((A & v & 0xFF) ? 0 : 2); return (v | A) & 0xFF;
        default: P = (P & 0xFD) | ((A & v & 0xFF) ? 0 : 2); return v & ~A & 0xFF;
        }
    }
    int alu16(int k, int v) {
        int r;
        switch (k) {
        case 0: r = (v << 1) & 0xFFFF; P = (P & 0x7C) | (v >> 15) | ((r >> 8) & 0x80) | (r ? 0 : 2); return r;
        case 1: r = ((v << 1) | (P & 1)) & 0xFFFF; P = (P & 0x7C) | (v >> 15) | ((r >> 8) & 0x80) | (r ? 0 : 2); return r;
        case 2: r = v >> 1; P = (P & 0x7C) | (v & 1) | (r ? 0 : 2); return r;
        case 3: r = (v >> 1) | ((P & 1) << 15); P = (P & 0x7C) | (v & 1) | ((r >> 8) & 0x80) | (r ? 0 : 2); return r;
        case 4: r = (v + 1) & 0xFFFF; nz16(r); return r;
        case 5: r = (v - 1) & 0xFFFF; nz16(r); return r;
        case 6: P = (P & 0xFD) | ((A & v) ? 0 : 2); return v | A;
        default: P = (P & 0xFD) | ((A & v) ? 0 : 2); return v & ~A & 0xFFFF;
        }
    }
    void rmw(int m, int k) {
        u32 a = ea(m);
        if (P & 0x20) { int v = rd(a); wr(a, alu8(k, v)); }
        else { int v = r16e(a); w16e(a, alu16(k, v)); }
    }
    void accop(int k) {
        if (P & 0x20) A = (A & 0xFF00) | alu8(k, A & 0xFF);
        else A = alu16(k, A);
    }
    void br(int mask, int want) {
        int off = f8();
        if ((P & mask) == want) {
            cyc += 1;
            if (off < 0x80) PC = (PC + off) & 0xFFFF;
            else {
                int npc = (PC + off - 256) & 0xFFFF;
                if (off >= 0xE0) {
                    if (ik_valid && ik_pc == npc && ik_a == A && ik_x == X && ik_y == Y && ik_p == P && ik_wc == bus->wc) {
                        if (cyc < target) cyc = target;
                    } else { ik_valid = true; ik_pc = npc; ik_a = A; ik_x = X; ik_y = Y; ik_p = P; ik_wc = bus->wc; }
                }
                PC = npc;
            }
        }
    }
    void LDXv(int v, int& reg) { reg = v; if (P & 0x10) nz8(v); else nz16(v); }
    void STr(int m, int reg, bool xflag) {
        u32 a = ea(m);
        if (P & (xflag ? 0x10 : 0x20)) wr(a, reg & 0xFF); else w16e(a, reg);
    }

    void exec(int op) {
        int gm = G1MODE[op & 0x1F];
        if (gm >= 0 && op != 0x89) {
            int grp = op >> 5;
            if (grp == 4) { u32 a = ea(gm); if (P & 0x20) wr(a, A & 0xFF); else w16e(a, A); return; }
            int v = getM(gm);
            bool m8 = P & 0x20;
            switch (grp) {
            case 0: if (m8) { int r = (A | v) & 0xFF; A = (A & 0xFF00) | r; nz8(r); } else { int r = A | v; A = r; nz16(r); } break;
            case 1: if (m8) { int r = A & v & 0xFF; A = (A & 0xFF00) | r; nz8(r); } else { int r = A & v; A = r; nz16(r); } break;
            case 2: if (m8) { int r = (A ^ v) & 0xFF; A = (A & 0xFF00) | r; nz8(r); } else { int r = A ^ v; A = r; nz16(r); } break;
            case 3: if (m8) adc8(v, false); else adc16(v, false); break;
            case 5: if (m8) { A = (A & 0xFF00) | v; nz8(v); } else { A = v; nz16(v); } break;
            case 6: cmpv(A, v, !m8); break;
            case 7: if (m8) adc8(v, true); else adc16(v, true); break;
            }
            return;
        }
        switch (op) {
        case 0x89: BIT(getM(M_IMM), true); break;
        case 0x24: BIT(getM(M_DP), false); break;
        case 0x2C: BIT(getM(M_AB), false); break;
        case 0x34: BIT(getM(M_DPX), false); break;
        case 0x3C: BIT(getM(M_ABX), false); break;
        // RMW
        case 0x06: rmw(M_DP, 0); break; case 0x0E: rmw(M_AB, 0); break; case 0x16: rmw(M_DPX, 0); break; case 0x1E: rmw(M_ABX, 0); break;
        case 0x26: rmw(M_DP, 1); break; case 0x2E: rmw(M_AB, 1); break; case 0x36: rmw(M_DPX, 1); break; case 0x3E: rmw(M_ABX, 1); break;
        case 0x46: rmw(M_DP, 2); break; case 0x4E: rmw(M_AB, 2); break; case 0x56: rmw(M_DPX, 2); break; case 0x5E: rmw(M_ABX, 2); break;
        case 0x66: rmw(M_DP, 3); break; case 0x6E: rmw(M_AB, 3); break; case 0x76: rmw(M_DPX, 3); break; case 0x7E: rmw(M_ABX, 3); break;
        case 0xE6: rmw(M_DP, 4); break; case 0xEE: rmw(M_AB, 4); break; case 0xF6: rmw(M_DPX, 4); break; case 0xFE: rmw(M_ABX, 4); break;
        case 0xC6: rmw(M_DP, 5); break; case 0xCE: rmw(M_AB, 5); break; case 0xD6: rmw(M_DPX, 5); break; case 0xDE: rmw(M_ABX, 5); break;
        case 0x04: rmw(M_DP, 6); break; case 0x0C: rmw(M_AB, 6); break;
        case 0x14: rmw(M_DP, 7); break; case 0x1C: rmw(M_AB, 7); break;
        case 0x0A: accop(0); break; case 0x2A: accop(1); break; case 0x4A: accop(2); break;
        case 0x6A: accop(3); break; case 0x1A: accop(4); break; case 0x3A: accop(5); break;
        // loads/stores of index registers
        case 0xA2: LDXv(getX(M_IMM), X); break; case 0xA6: LDXv(getX(M_DP), X); break; case 0xAE: LDXv(getX(M_AB), X); break;
        case 0xB6: LDXv(getX(M_DPY), X); break; case 0xBE: LDXv(getX(M_ABY), X); break;
        case 0xA0: LDXv(getX(M_IMM), Y); break; case 0xA4: LDXv(getX(M_DP), Y); break; case 0xAC: LDXv(getX(M_AB), Y); break;
        case 0xB4: LDXv(getX(M_DPX), Y); break; case 0xBC: LDXv(getX(M_ABX), Y); break;
        case 0x86: STr(M_DP, X, true); break; case 0x8E: STr(M_AB, X, true); break; case 0x96: STr(M_DPY, X, true); break;
        case 0x84: STr(M_DP, Y, true); break; case 0x8C: STr(M_AB, Y, true); break; case 0x94: STr(M_DPX, Y, true); break;
        case 0x64: STr(M_DP, 0, false); break; case 0x74: STr(M_DPX, 0, false); break;
        case 0x9C: STr(M_AB, 0, false); break; case 0x9E: STr(M_ABX, 0, false); break;
        case 0xE0: { int v = getX(M_IMM); cmpv(X, v, !(P & 0x10)); break; }
        case 0xE4: { int v = getX(M_DP); cmpv(X, v, !(P & 0x10)); break; }
        case 0xEC: { int v = getX(M_AB); cmpv(X, v, !(P & 0x10)); break; }
        case 0xC0: { int v = getX(M_IMM); cmpv(Y, v, !(P & 0x10)); break; }
        case 0xC4: { int v = getX(M_DP); cmpv(Y, v, !(P & 0x10)); break; }
        case 0xCC: { int v = getX(M_AB); cmpv(Y, v, !(P & 0x10)); break; }
        // branches
        case 0x10: br(0x80, 0); break; case 0x30: br(0x80, 0x80); break;
        case 0x50: br(0x40, 0); break; case 0x70: br(0x40, 0x40); break;
        case 0x90: br(0x01, 0); break; case 0xB0: br(0x01, 0x01); break;
        case 0xD0: br(0x02, 0); break; case 0xF0: br(0x02, 0x02); break;
        case 0x80: br(0, 0); break;
        case 0x82: { int o = f16(); PC = (PC + o - ((o & 0x8000) ? 0x10000 : 0)) & 0xFFFF; break; }
        // jumps / calls
        case 0x4C: { int a = f16(); if (a == ((PC - 3) & 0xFFFF) && cyc < target) cyc = target; PC = a; break; }
        case 0x5C: { int v = f24(); PC = v & 0xFFFF; PB = v >> 16; break; }
        case 0x6C: PC = r16z(f16()); break;
        case 0x7C: { int a = (f16() + X) & 0xFFFF; int lo = rd((PB << 16) | a); int hi = rd((PB << 16) | ((a + 1) & 0xFFFF)); PC = lo | (hi << 8); break; }  // pointer wraps in PB (the Python core crossed banks)
        case 0xDC: { int a = f16(); PC = r16z(a); PB = rd((a + 2) & 0xFFFF); break; }
        case 0x20: { int a = f16(); push16((PC - 1) & 0xFFFF); PC = a; break; }
        case 0x22: { int a = f24(); npush8(PB); npush16((PC - 1) & 0xFFFF); nfix(); PB = a >> 16; PC = a & 0xFFFF; break; }
        case 0xFC: { int a = f16(); npush16((PC - 1) & 0xFFFF); nfix(); int p = (a + X) & 0xFFFF; int lo = rd((PB << 16) | p); int hi = rd((PB << 16) | ((p + 1) & 0xFFFF)); PC = lo | (hi << 8); break; }
        case 0x60: PC = (pull16() + 1) & 0xFFFF; break;
        case 0x6B: PC = (npull16() + 1) & 0xFFFF; PB = npull8(); nfix(); break;
        case 0x40: setP(pull8()); PC = pull16(); if (!E) PB = pull8(); break;
        case 0x00: f8(); interrupt(0xFFE6, 0xFFFE, true); break;
        case 0x02: f8(); interrupt(0xFFE4, 0xFFF4, true); break;
        // stack
        case 0x48: if (P & 0x20) push8(A); else push16(A); break;
        case 0x68: if (P & 0x20) { int v = pull8(); A = (A & 0xFF00) | v; nz8(v); } else { int v = pull16(); A = v; nz16(v); } break;
        case 0xDA: if (P & 0x10) push8(X); else push16(X); break;
        case 0x5A: if (P & 0x10) push8(Y); else push16(Y); break;
        case 0xFA: if (P & 0x10) { int v = pull8(); X = v; nz8(v); } else { int v = pull16(); X = v; nz16(v); } break;
        case 0x7A: if (P & 0x10) { int v = pull8(); Y = v; nz8(v); } else { int v = pull16(); Y = v; nz16(v); } break;
        case 0x08: push8(P | (E ? 0x30 : 0)); break;
        case 0x28: setP(pull8()); break;
        case 0x8B: push8(DB); break;
        case 0xAB: DB = npull8(); nfix(); nz8(DB); break;
        case 0x0B: npush16(D); nfix(); break;
        case 0x2B: D = npull16(); nfix(); nz16(D); break;
        case 0x4B: push8(PB); break;
        case 0xF4: npush16(f16()); nfix(); break;
        case 0xD4: { int o = f8(); npush16(r16z((D + o) & 0xFFFF)); nfix(); break; }
        case 0x62: { int v = f16(); npush16((PC + v) & 0xFFFF); nfix(); break; }
        // transfers
        case 0xAA: if (P & 0x10) { X = A & 0xFF; nz8(X); } else { X = A; nz16(X); } break;
        case 0xA8: if (P & 0x10) { Y = A & 0xFF; nz8(Y); } else { Y = A; nz16(Y); } break;
        case 0x8A: if (P & 0x20) { A = (A & 0xFF00) | (X & 0xFF); nz8(X & 0xFF); } else { A = X; nz16(A); } break;
        case 0x98: if (P & 0x20) { A = (A & 0xFF00) | (Y & 0xFF); nz8(Y & 0xFF); } else { A = Y; nz16(A); } break;
        case 0x9B: Y = X; if (P & 0x10) nz8(Y); else nz16(Y); break;
        case 0xBB: X = Y; if (P & 0x10) nz8(X); else nz16(X); break;
        case 0xBA: if (P & 0x10) { X = S & 0xFF; nz8(X); } else { X = S; nz16(X); } break;
        case 0x9A: S = E ? (0x100 | (X & 0xFF)) : X; break;
        case 0x1B: S = E ? (0x100 | (A & 0xFF)) : A; break;
        case 0x3B: A = S; nz16(A); break;
        case 0x5B: D = A; nz16(D); break;
        case 0x7B: A = D; nz16(A); break;
        case 0xEB: A = ((A >> 8) | (A << 8)) & 0xFFFF; nz8(A & 0xFF); break;
        case 0xFB: { int c = P & 1, e = E; E = c; P = (P & 0xFE) | e;
                     if (E) { P |= 0x30; X &= 0xFF; Y &= 0xFF; S = 0x100 | (S & 0xFF); } break; }
        // index inc/dec
        case 0xE8: if (P & 0x10) { X = (X + 1) & 0xFF; nz8(X); } else { X = (X + 1) & 0xFFFF; nz16(X); } break;
        case 0xC8: if (P & 0x10) { Y = (Y + 1) & 0xFF; nz8(Y); } else { Y = (Y + 1) & 0xFFFF; nz16(Y); } break;
        case 0xCA: if (P & 0x10) { X = (X - 1) & 0xFF; nz8(X); } else { X = (X - 1) & 0xFFFF; nz16(X); } break;
        case 0x88: if (P & 0x10) { Y = (Y - 1) & 0xFF; nz8(Y); } else { Y = (Y - 1) & 0xFFFF; nz16(Y); } break;
        // flags
        case 0x18: P &= ~1; break; case 0x38: P |= 1; break; case 0x58: P &= ~4; break; case 0x78: P |= 4; break;
        case 0xD8: P &= ~8; break; case 0xF8: P |= 8; break; case 0xB8: P &= ~0x40; break;
        case 0xC2: setP(P & ~f8()); break;
        case 0xE2: setP(P | f8()); break;
        // block moves
        case 0x54: case 0x44: {
            int dst = f8(), src = f8(); DB = dst;
            int xm = (P & 0x10) ? 0xFF : 0xFFFF; i64 n = 0; int step = op == 0x54 ? 1 : -1;
            while (true) {
                int v = rd((src << 16) | X); wr((dst << 16) | Y, v);
                X = (X + step) & xm; Y = (Y + step) & xm; n++;
                A = (A - 1) & 0xFFFF;
                if (A == 0xFFFF) break;
            }
            cyc += 7 * n; break;
        }
        case 0xCB: waiting = true; break;
        case 0xDB: stopped = true; break;
        case 0xEA: break;
        case 0x42: f8(); break;
        default: break;
        }
    }
};

// ===================================================================== SPC700 (S-SMP) sound CPU
static const u8 SPC_CYC[256] = {
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8, 2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4, 2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6, 2,8,4,5,4,5,5,6,5,5,4,5,2,2,4,3,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,5,5, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5,
    3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,
    3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9, 2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,
    2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3, 2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,3};

struct SPCBus { virtual u8 read(int a) = 0; virtual void write(int a, int v) = 0; virtual ~SPCBus() {} };

struct SPC700 {
    SPCBus* bus;
    int A = 0, X = 0, Y = 0, SP = 0xEF, PC = 0xFFC0, PSW = 0x02;
    i64 cyc = 0, target = 0; bool stopped = false;
    int trap = 0x10000;            // PC >= trap returns to the APU (HLE boot ROM)
    explicit SPC700(SPCBus* b) : bus(b) {}

    inline int rd(int a) { return bus->read(a); }
    inline void wr(int a, int v) { bus->write(a, v & 0xFF); }

    void run(i64 tgt) {
        target = tgt;
        while (cyc < tgt) {
            int pc = PC;
            if (pc >= trap) return;
            if (stopped) { cyc = tgt; return; }
            int op = rd(pc);
            PC = (pc + 1) & 0xFFFF;
            cyc += SPC_CYC[op];
            exec(op);
        }
    }
    inline int f8() { int pc = PC; PC = (pc + 1) & 0xFFFF; return rd(pc); }
    inline int f16() { int lo = f8(); return lo | (f8() << 8); }
    inline int dpb() { return (PSW & 0x20) << 3; }
    inline int a_d() { return dpb() | f8(); }
    inline int a_dx() { return dpb() | ((f8() + X) & 0xFF); }
    inline int a_dy() { return dpb() | ((f8() + Y) & 0xFF); }
    inline int a_abs() { return f16(); }
    inline int a_absx() { return (f16() + X) & 0xFFFF; }
    inline int a_absy() { return (f16() + Y) & 0xFFFF; }
    inline int a_ix() { return dpb() | X; }
    inline int rdw_dp(int n) { int b = dpb(); int lo = rd(b | (n & 0xFF)); int hi = rd(b | ((n + 1) & 0xFF)); return lo | (hi << 8); }
    inline int a_idx() { int o = f8(); return rdw_dp(o + X); }
    inline int a_idy() { return (rdw_dp(f8()) + Y) & 0xFFFF; }
    inline void nz(int v) { PSW = (PSW & 0x7D) | (v & 0x80) | (v ? 0 : 2); }
    inline void push(int v) { wr(0x100 | SP, v & 0xFF); SP = (SP - 1) & 0xFF; }
    inline int pop() { SP = (SP + 1) & 0xFF; return rd(0x100 | SP); }
    inline void push16(int v) { push(v >> 8); push(v); }
    inline int pop16() { int lo = pop(); return lo | (pop() << 8); }
    inline void nz16(int r) { PSW = (PSW & 0x7D) | ((r >> 8) & 0x80) | (r ? 0 : 2); }
    inline void branch(int r) { PC = (PC + r - ((r & 0x80) ? 256 : 0)) & 0xFFFF; }

    // ALU: 0 or 1 and 2 eor 3 cmp 4 adc 5 sbc
    int alu(int k, int a, int b) {
        switch (k) {
        case 0: { int r = a | b; nz(r); return r; }
        case 1: { int r = a & b; nz(r); return r; }
        case 2: { int r = a ^ b; nz(r); return r; }
        case 3: { int r = a - b; PSW = (PSW & 0x7C) | (r >= 0 ? 1 : 0) | (r & 0x80) | ((r & 0xFF) ? 0 : 2); return a; }
        case 5: b ^= 0xFF; [[fallthrough]];
        default: {
            int c = PSW & 1; int r = a + b + c;
            int p = (PSW & 0x34) | (r > 0xFF ? 1 : 0) | ((~(a ^ b) & (a ^ r) & 0x80) >> 1);
            if ((a & 15) + (b & 15) + c > 15) p |= 8;
            r &= 0xFF;
            PSW = p | (r & 0x80) | (r ? 0 : 2);
            return r;
        }
        }
    }
    // shifts: 0 asl 1 rol 2 lsr 3 ror 4 dec 5 inc
    int sh(int k, int v) {
        int r;
        switch (k) {
        case 0: r = (v << 1) & 0xFF; PSW = (PSW & 0xFE) | (v >> 7); nz(r); return r;
        case 1: r = ((v << 1) | (PSW & 1)) & 0xFF; PSW = (PSW & 0xFE) | (v >> 7); nz(r); return r;
        case 2: r = v >> 1; PSW = (PSW & 0xFE) | (v & 1); nz(r); return r;
        case 3: r = (v >> 1) | ((PSW & 1) << 7); PSW = (PSW & 0xFE) | (v & 1); nz(r); return r;
        case 4: r = (v - 1) & 0xFF; nz(r); return r;
        default: r = (v + 1) & 0xFF; nz(r); return r;
        }
    }
    void wdp(int& lo, int& hi) { int n = f8(); int b = dpb(); lo = b | n; hi = b | ((n + 1) & 0xFF); }

    void exec(int op) {
        int hi = op >> 4, lo = op & 15;
        // ---- ALU group (bases 00 20 40 60 80 A0)
        if ((op & 0x1F) >= 0x04 && hi <= 0xB && !(hi & 1 ? false : false)) {
            int base = op & 0xE0; int off = op & 0x1F;
            if (base <= 0xA0) {
                int k = base >> 5;
                bool handled = true;
                switch (off) {
                case 0x04: { int v = rd(a_d()); A = alu(k, A, v); break; }
                case 0x14: { int v = rd(a_dx()); A = alu(k, A, v); break; }
                case 0x05: { int v = rd(a_abs()); A = alu(k, A, v); break; }
                case 0x15: { int v = rd(a_absx()); A = alu(k, A, v); break; }
                case 0x06: { int v = rd(a_ix()); A = alu(k, A, v); break; }
                case 0x16: { int v = rd(a_absy()); A = alu(k, A, v); break; }
                case 0x07: { int v = rd(a_idx()); A = alu(k, A, v); break; }
                case 0x17: { int v = rd(a_idy()); A = alu(k, A, v); break; }
                case 0x08: { int v = f8(); A = alu(k, A, v); break; }
                case 0x18: { int i = f8(); int a = a_d(); int r = alu(k, rd(a), i); if (k != 3) wr(a, r); break; }
                case 0x09: { int v = rd(a_d()); int a = a_d(); int r = alu(k, rd(a), v); if (k != 3) wr(a, r); break; }
                case 0x19: { int b = dpb(); int v = rd(b | Y); int a = b | X; int r = alu(k, rd(a), v); if (k != 3) wr(a, r); break; }
                default: handled = false;
                }
                if (handled) return;
            }
        }
        (void)hi; (void)lo;
        // ---- shifts / inc / dec (bases 00 20 40 60 80 A0 at +0B +1B +0C +1C)
        {
            int base = op & 0xE0, off = op & 0x1F;
            if (base <= 0xA0 && (off == 0x0B || off == 0x1B || off == 0x0C || off == 0x1C)) {
                int k = base >> 5;
                if (off == 0x0B) { int a = a_d(); wr(a, sh(k, rd(a))); }
                else if (off == 0x1B) { int a = a_dx(); wr(a, sh(k, rd(a))); }
                else if (off == 0x0C) { int a = a_abs(); wr(a, sh(k, rd(a))); }
                else A = sh(k, A);
                return;
            }
        }
        // ---- bit ops SET1/CLR1/BBS/BBC
        if ((op & 0x0F) == 0x02 || (op & 0x0F) == 0x03) {
            int b = op >> 5, m = 1 << b; bool upper = op & 0x10;
            if ((op & 0x0F) == 0x02) {
                int a = a_d();
                if (!upper) wr(a, rd(a) | m); else wr(a, rd(a) & ~m & 0xFF);
            } else {
                int v = rd(a_d()); int r = f8();
                bool take = upper ? !(v & m) : (v & m);
                if (take) { branch(r); cyc += 2; }
            }
            return;
        }
        // ---- TCALL
        if ((op & 0x0F) == 0x01) {
            int n = op >> 4; int v = 0xFFDE - 2 * n;
            push16(PC); int l = rd(v); int h = rd(v + 1); PC = l | (h << 8); return;
        }
        switch (op) {
        // compare X/Y
        case 0xC8: { int v = f8(); alu(3, X, v); break; }
        case 0x3E: { int v = rd(a_d()); alu(3, X, v); break; }
        case 0x1E: { int v = rd(a_abs()); alu(3, X, v); break; }
        case 0xAD: { int v = f8(); alu(3, Y, v); break; }
        case 0x7E: { int v = rd(a_d()); alu(3, Y, v); break; }
        case 0x5E: { int v = rd(a_abs()); alu(3, Y, v); break; }
        // stores of A
        case 0xC4: { int a = a_d(); wr(a, A); break; }
        case 0xD4: { int a = a_dx(); wr(a, A); break; }
        case 0xC5: { int a = a_abs(); wr(a, A); break; }
        case 0xD5: { int a = a_absx(); wr(a, A); break; }
        case 0xC6: { int a = a_ix(); wr(a, A); break; }
        case 0xD6: { int a = a_absy(); wr(a, A); break; }
        case 0xC7: { int a = a_idx(); wr(a, A); break; }
        case 0xD7: { int a = a_idy(); wr(a, A); break; }
        // loads of A
        case 0xE4: A = rd(a_d()); nz(A); break;
        case 0xF4: A = rd(a_dx()); nz(A); break;
        case 0xE5: A = rd(a_abs()); nz(A); break;
        case 0xF5: A = rd(a_absx()); nz(A); break;
        case 0xE6: A = rd(a_ix()); nz(A); break;
        case 0xF6: A = rd(a_absy()); nz(A); break;
        case 0xE7: A = rd(a_idx()); nz(A); break;
        case 0xF7: A = rd(a_idy()); nz(A); break;
        case 0xE8: A = f8(); nz(A); break;
        case 0xCD: X = f8(); nz(X); break;
        case 0xF8: X = rd(a_d()); nz(X); break;
        case 0xF9: X = rd(a_dy()); nz(X); break;
        case 0xE9: X = rd(a_abs()); nz(X); break;
        case 0x8D: Y = f8(); nz(Y); break;
        case 0xEB: Y = rd(a_d()); nz(Y); break;
        case 0xFB: Y = rd(a_dx()); nz(Y); break;
        case 0xEC: Y = rd(a_abs()); nz(Y); break;
        case 0xD8: { int a = a_d(); wr(a, X); break; }
        case 0xD9: { int a = a_dy(); wr(a, X); break; }
        case 0xC9: { int a = a_abs(); wr(a, X); break; }
        case 0xCB: { int a = a_d(); wr(a, Y); break; }
        case 0xDB: { int a = a_dx(); wr(a, Y); break; }
        case 0xCC: { int a = a_abs(); wr(a, Y); break; }
        // transfers
        case 0x7D: A = X; nz(A); break;
        case 0xDD: A = Y; nz(A); break;
        case 0x5D: X = A; nz(X); break;
        case 0xFD: Y = A; nz(Y); break;
        case 0x9D: X = SP; nz(X); break;
        case 0xBD: SP = X; break;
        case 0xAF: wr(dpb() | X, A); X = (X + 1) & 0xFF; break;
        case 0xBF: { int v = rd(dpb() | X); X = (X + 1) & 0xFF; A = v; nz(v); break; }
        case 0xFA: { int v = rd(a_d()); wr(a_d(), v); break; }
        case 0x8F: { int i = f8(); wr(a_d(), i); break; }
        // index inc/dec
        case 0xDC: Y = sh(4, Y); break;
        case 0xFC: Y = sh(5, Y); break;
        case 0x1D: X = sh(4, X); break;
        case 0x3D: X = sh(5, X); break;
        // 16-bit
        case 0x3A: { int l, h; wdp(l, h); int lo_ = rd(l); int hi_ = rd(h); int v = ((lo_ | (hi_ << 8)) + 1) & 0xFFFF; wr(l, v & 0xFF); wr(h, v >> 8); nz16(v); break; }
        case 0x1A: { int l, h; wdp(l, h); int lo_ = rd(l); int hi_ = rd(h); int v = ((lo_ | (hi_ << 8)) - 1) & 0xFFFF; wr(l, v & 0xFF); wr(h, v >> 8); nz16(v); break; }
        case 0xBA: { int l, h; wdp(l, h); A = rd(l); Y = rd(h); nz16((Y << 8) | A); break; }
        case 0xDA: { int l, h; wdp(l, h); wr(l, A); wr(h, Y); break; }
        case 0x5A: { int l, h; wdp(l, h); int lo_ = rd(l); int hi_ = rd(h); int w = lo_ | (hi_ << 8); int ya = (Y << 8) | A;
                     int r = ya - w; PSW = (PSW & 0x7C) | (r >= 0 ? 1 : 0) | ((r >> 8) & 0x80) | ((r & 0xFFFF) ? 0 : 2); break; }
        case 0x7A: { int l, h; wdp(l, h); int lo_ = rd(l); int hi_ = rd(h); int w = lo_ | (hi_ << 8); int ya = (Y << 8) | A;
                     int r = ya + w;
                     int p = (PSW & 0x34) | (r > 0xFFFF ? 1 : 0) | ((~(ya ^ w) & (ya ^ r) & 0x8000) >> 9);
                     if ((ya & 0xFFF) + (w & 0xFFF) > 0xFFF) p |= 8;
                     r &= 0xFFFF; A = r & 0xFF; Y = r >> 8; PSW = p | ((r >> 8) & 0x80) | (r ? 0 : 2); break; }
        case 0x9A: { int l, h; wdp(l, h); int lo_ = rd(l); int hi_ = rd(h); int w = lo_ | (hi_ << 8); int ya = (Y << 8) | A;
                     int r = ya - w;
                     int p = (PSW & 0x34) | (r >= 0 ? 1 : 0) | (((ya ^ w) & (ya ^ r) & 0x8000) >> 9);
                     if ((ya & 0xFFF) >= (w & 0xFFF)) p |= 8;
                     r &= 0xFFFF; A = r & 0xFF; Y = r >> 8; PSW = p | ((r >> 8) & 0x80) | (r ? 0 : 2); break; }
        // carry bit ops
        case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: {
            int w = f16(); int a = w & 0x1FFF, b = w >> 13; int bit = (rd(a) >> b) & 1; int c = PSW & 1;
            switch (op) {
            case 0x0A: c |= bit; break; case 0x2A: c |= bit ^ 1; break; case 0x4A: c &= bit; break;
            case 0x6A: c &= bit ^ 1; break; case 0x8A: c ^= bit; break; default: c = bit; break;
            }
            PSW = (PSW & 0xFE) | c; break;
        }
        case 0xCA: { int w = f16(); int a = w & 0x1FFF, b = w >> 13; int v = rd(a); wr(a, (v & ~(1 << b) & 0xFF) | ((PSW & 1) << b)); break; }
        case 0xEA: { int w = f16(); int a = w & 0x1FFF, b = w >> 13; wr(a, rd(a) ^ (1 << b)); break; }
        case 0x0E: { int a = f16(); int v = rd(a); nz((A - v) & 0xFF); wr(a, v | A); break; }
        case 0x4E: { int a = f16(); int v = rd(a); nz((A - v) & 0xFF); wr(a, v & ~A & 0xFF); break; }
        // branches
        case 0x10: { int r = f8(); if (!(PSW & 0x80)) { branch(r); cyc += 2; } break; }
        case 0x30: { int r = f8(); if (PSW & 0x80) { branch(r); cyc += 2; } break; }
        case 0x50: { int r = f8(); if (!(PSW & 0x40)) { branch(r); cyc += 2; } break; }
        case 0x70: { int r = f8(); if (PSW & 0x40) { branch(r); cyc += 2; } break; }
        case 0x90: { int r = f8(); if (!(PSW & 1)) { branch(r); cyc += 2; } break; }
        case 0xB0: { int r = f8(); if (PSW & 1) { branch(r); cyc += 2; } break; }
        case 0xD0: { int r = f8(); if (!(PSW & 2)) { branch(r); cyc += 2; } break; }
        case 0xF0: { int r = f8(); if (PSW & 2) { branch(r); cyc += 2; } break; }
        case 0x2F: { int r = f8(); branch(r); break; }
        case 0x2E: { int v = rd(a_d()); int r = f8(); if (A != v) { branch(r); cyc += 2; } break; }
        case 0xDE: { int v = rd(a_dx()); int r = f8(); if (A != v) { branch(r); cyc += 2; } break; }
        case 0x6E: { int a = a_d(); int v = (rd(a) - 1) & 0xFF; wr(a, v); int r = f8(); if (v) { branch(r); cyc += 2; } break; }
        case 0xFE: { Y = (Y - 1) & 0xFF; int r = f8(); if (Y) { branch(r); cyc += 2; } break; }
        // jumps / calls / stack
        case 0x5F: PC = f16(); break;
        case 0x1F: { int a = (f16() + X) & 0xFFFF; int l = rd(a); int h = rd((a + 1) & 0xFFFF); PC = l | (h << 8); break; }
        case 0x3F: { int a = f16(); push16(PC); PC = a; break; }
        case 0x4F: { int u = f8(); push16(PC); PC = 0xFF00 | u; break; }
        case 0x6F: PC = pop16(); break;
        case 0x7F: PSW = pop(); PC = pop16(); break;
        case 0x0F: { push16(PC); push(PSW); PSW = (PSW | 0x10) & ~0x04; int l = rd(0xFFDE); int h = rd(0xFFDF); PC = l | (h << 8); break; }
        case 0x0D: push(PSW); break; case 0x2D: push(A); break; case 0x4D: push(X); break; case 0x6D: push(Y); break;
        case 0x8E: PSW = pop(); break; case 0xAE: A = pop(); break; case 0xCE: X = pop(); break; case 0xEE: Y = pop(); break;
        // flags
        case 0x20: PSW &= ~0x20 & 0xFF; break;
        case 0x40: PSW |= 0x20; break;
        case 0x60: PSW &= ~1 & 0xFF; break;
        case 0x80: PSW |= 1; break;
        case 0xA0: PSW |= 4; break;
        case 0xC0: PSW &= ~4 & 0xFF; break;
        case 0xE0: PSW &= ~0x48 & 0xFF; break;
        case 0xED: PSW ^= 1; break;
        // math
        case 0x9F: { int v = ((A >> 4) | (A << 4)) & 0xFF; A = v; nz(v); break; }
        case 0xCF: { int r = Y * A; Y = r >> 8; A = r & 0xFF; nz(Y); break; }
        case 0x9E: {
            int ya = (Y << 8) | A, x = X, y = Y;
            int p = PSW & ~0x48 & 0xFF;
            if (y >= x) p |= 0x40;
            if ((y & 15) >= (x & 15)) p |= 0x08;
            i64 a, yy;
            if (y < (x << 1)) { a = ya / x; yy = ya % x; }
            else { a = 255 - floordiv(ya - (x << 9), 256 - x); yy = x + floormod(ya - (x << 9), 256 - x); }
            A = (int)(a & 0xFF); Y = (int)(yy & 0xFF); PSW = p; nz(A); break;
        }
        case 0xDF: { int a = A, p = PSW; if ((p & 1) || a > 0x99) { a += 0x60; p |= 1; } if ((p & 8) || (a & 15) > 9) a += 6; PSW = p; A = a & 0xFF; nz(A); break; }
        case 0xBE: { int a = A, p = PSW; if (!(p & 1) || a > 0x99) { a -= 0x60; p &= 0xFE; } if (!(p & 8) || (a & 15) > 9) a -= 6; PSW = p; A = a & 0xFF; nz(A); break; }
        case 0x00: break;
        case 0xEF: case 0xFF: stopped = true; break;
        default: break;
        }
    }
};

// ===================================================================== Snes9x S-DSP
// Line-for-line port of Snes9x apu/bapu/dsp/SPC_DSP.cpp (blargg's snes_spc 0.9.0 DSP with
// Snes9x register semantics).
//
// Copyright (C) 2007 Shay Green. This module is free software; you can redistribute it
// and/or modify it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation; either version 2.1 of the License, or (at your option)
// any later version. This module is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
// C++ port for Kondo vsnes 0.1.1a, same license (LGPL 2.1+) for this module.

static const short GAUSS[512] = {
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,3,3,3,3,3,4,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,8,8,8,9,9,9,10,10,10,11,11,11,12,12,13,13,14,14,15,15,15,16,16,17,17,18,19,19,20,20,21,21,22,23,23,24,24,25,26,27,27,28,29,29,30,31,32,32,33,34,35,36,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,58,59,60,61,62,64,65,66,67,69,70,71,73,74,76,77,78,80,81,83,84,86,87,89,90,92,94,95,97,99,100,102,104,106,107,109,111,113,115,117,118,120,122,124,126,128,130,132,134,137,139,141,143,145,147,150,152,154,156,159,161,163,166,168,171,173,175,178,180,183,186,188,191,193,196,199,201,204,207,210,212,215,218,221,224,227,230,233,236,239,242,245,248,251,254,257,260,263,267,270,273,276,280,283,286,290,293,297,300,304,307,311,314,318,321,325,328,332,336,339,343,347,351,354,358,362,366,370,374,378,381,385,389,393,397,401,405,410,414,418,422,426,430,434,439,443,447,451,456,460,464,469,473,477,482,486,491,495,499,504,508,513,517,522,527,531,536,540,545,550,554,559,563,568,573,577,582,587,592,596,601,606,611,615,620,625,630,635,640,644,649,654,659,664,669,674,678,683,688,693,698,703,708,713,718,723,728,732,737,742,747,752,757,762,767,772,777,782,787,792,797,802,806,811,816,821,826,831,836,841,846,851,855,860,865,870,875,880,884,889,894,899,904,908,913,918,923,927,932,937,941,946,951,955,960,965,969,974,978,983,988,992,997,1001,1005,1010,1014,1019,1023,1027,1032,1036,1040,1045,1049,1053,1057,1061,1066,1070,1074,1078,1082,1086,1090,1094,1098,1102,1106,1109,1113,1117,1121,1125,1128,1132,1136,1139,1143,1146,1150,1153,1157,1160,1164,1167,1170,1174,1177,1180,1183,1186,1190,1193,1196,1199,1202,1205,1207,1210,1213,1216,1219,1221,1224,1227,1229,1232,1234,1237,1239,1241,1244,1246,1248,1251,1253,1255,1257,1259,1261,1263,1265,1267,1269,1270,1272,1274,1275,1277,1279,1280,1282,1283,1284,1286,1287,1288,1290,1291,1292,1293,1294,1295,1296,1297,1297,1298,1299,1300,1300,1301,1302,1302,1303,1303,1303,1304,1304,1304,1304,1304,1305,1305};
static const u8 INITIAL_REGS[128] = {
69,139,90,154,228,130,27,120,0,0,170,150,137,14,224,128,42,73,61,186,20,160,172,197,0,0,81,187,156,78,123,255,244,253,87,50,55,217,66,34,0,0,91,60,159,27,135,154,111,39,175,123,229,104,10,217,0,0,154,197,156,78,123,255,234,33,120,79,221,237,36,20,0,0,119,177,209,54,193,103,82,87,70,61,89,244,135,164,0,0,126,68,0,78,123,255,117,245,6,151,16,195,36,187,0,0,123,122,224,96,18,15,247,116,28,229,57,61,115,193,0,0,122,179,255,78,123,255};
static const unsigned COUNTER_RATES[32] = {30720 + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128,
    96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
static const unsigned COUNTER_OFFSETS[32] = {1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040,
    536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

class SPCDSP {
public:
    enum { R_MVOLL = 0x0C, R_EVOLL = 0x2C, R_KON = 0x4C, R_KOFF = 0x5C, R_FLG = 0x6C, R_ENDX = 0x7C,
           R_EFB = 0x0D, R_PMON = 0x2D, R_NON = 0x3D, R_EON = 0x4D, R_DIR = 0x5D, R_ESA = 0x6D,
           R_EDL = 0x7D, R_FIR = 0x0F, V_VOLL = 0, V_PITCHL = 2, V_PITCHH = 3, V_SRCN = 4,
           V_ADSR0 = 5, V_ADSR1 = 6, V_GAIN = 7, V_ENVX = 8, V_OUTX = 9, BRR_BUF = 12, BRR_BLOCK = 9,
           ENV_RELEASE = 0, ENV_ATTACK = 1, ENV_DECAY = 2, ENV_SUSTAIN = 3, SIMPLE_RANGE = 30720 };
    struct Voice { int buf[24]; int buf_pos, interp_pos, brr_addr, brr_offset, regs, vbit, kon_delay, env_mode, env, hidden_env, t_envx_out; };
    struct State {
        u8 regs[128]; u8 external_regs[128];
        int echo_hist[16][2]; int echo_hist_pos, every_other_sample, kon, noise, counter, echo_offset, echo_length, phase;
        int kon_check, new_kon, endx_buf, envx_buf, outx_buf, t_pmon, t_non, t_eon, t_dir, t_koff, t_brr_next_addr;
        int t_adsr0, t_brr_header, t_brr_byte, t_srcn, t_esa, t_echo_enabled, t_dir_addr, t_pitch, t_output, t_looped, t_echo_ptr;
        int t_main_out[2], t_echo_out[2], t_echo_in[2];
        Voice voices[8]; int mute_mask;
    } m;
    u8* ram;
    std::vector<int16_t> out;     // interleaved L/R
    size_t out_cap;

    SPCDSP(u8* r, size_t frames = 1 << 17) : ram(r), out_cap(frames) { out.reserve(4096); reset(); }

    void reset() { load(INITIAL_REGS); }
    void load(const u8* regs) {
        memset(&m, 0, sizeof(State));
        for (int i = 0; i < 128; i++) m.external_regs[i] = regs[i];
        m.regs[66] = 0x01; m.regs[82] = 0x01; m.regs[R_FLG] = 0xE0;
        for (int i = 7; i >= 0; i--) { Voice* v = &m.voices[i]; v->brr_offset = 1; v->vbit = 1 << i; v->regs = i * 0x10; }
        m.new_kon = m.regs[R_KON]; m.t_dir = m.regs[R_DIR]; m.t_esa = m.regs[R_ESA];
        soft_reset_common();
    }
    void soft_reset_common() { m.noise = 0x4000; m.echo_hist_pos = 0; m.every_other_sample = 1; m.echo_offset = 0; m.phase = 0; m.counter = 0; }
    int read(int addr) { return m.external_regs[addr & 0x7F]; }
    void write(int addr, int data) {
        addr &= 0x7F; data &= 0xFF;
        m.regs[addr] = data; m.external_regs[addr] = data;
        int low = addr & 0x0F;
        if (low == V_ENVX) m.envx_buf = data;
        else if (low == V_OUTX) m.outx_buf = data;
        else if (low == 0x0C) {
            if (addr == R_KON) m.new_kon = data;
            if (addr == R_ENDX) { m.endx_buf = 0; m.regs[R_ENDX] = 0; }
        }
    }
    std::vector<int16_t> take() { std::vector<int16_t> r; r.swap(out); out.reserve(4096); return r; }

    static inline int clamp16(int io) { if ((short)io != io) io = (io >> 31) ^ 0x7FFF; return io; }
    inline unsigned read_counter(int rate) { return ((unsigned)m.counter + COUNTER_OFFSETS[rate]) % COUNTER_RATES[rate]; }
    inline void run_counters() { if (--m.counter < 0) m.counter = SIMPLE_RANGE - 1; }

    inline void run_envelope(Voice* v) {
        int env = v->env, rate, env_data, mode;
        if (v->env_mode == ENV_RELEASE) { env -= 0x8; if (env < 0) env = 0; v->env = env; return; }
        env_data = m.regs[v->regs + V_ADSR1];
        if (m.t_adsr0 & 0x80) {
            if (v->env_mode >= ENV_DECAY) {
                env--; env -= env >> 8;
                rate = env_data & 0x1F;
                if (v->env_mode == ENV_DECAY) rate = (m.t_adsr0 >> 3 & 0x0E) + 0x10;
            } else {
                rate = (m.t_adsr0 & 0x0F) * 2 + 1;
                env += rate < 31 ? 0x20 : 0x400;
            }
        } else {
            env_data = m.regs[v->regs + V_GAIN];
            mode = env_data >> 5;
            if (mode < 4) { env = env_data * 0x10; rate = 31; }
            else {
                rate = env_data & 0x1F;
                if (mode == 4) env -= 0x20;
                else if (mode < 6) { env--; env -= env >> 8; }
                else { env += 0x20; if (mode > 6 && (unsigned)v->hidden_env >= 0x600) env += 0x8 - 0x20; }
            }
        }
        if ((env >> 8) == (env_data >> 5) && v->env_mode == ENV_DECAY) v->env_mode = ENV_SUSTAIN;
        v->hidden_env = env;
        if ((unsigned)env > 0x7FF) { env = env < 0 ? 0 : 0x7FF; if (v->env_mode == ENV_ATTACK) v->env_mode = ENV_DECAY; }
        if (!read_counter(rate)) v->env = env;
    }
    inline void decode_brr(Voice* v) {
        int nybbles = m.t_brr_byte * 0x100 + ram[(v->brr_addr + v->brr_offset + 1) & 0xFFFF];
        int header = m.t_brr_header, pos = v->buf_pos, end = pos + 4;
        v->buf_pos += 4; if (v->buf_pos >= BRR_BUF) v->buf_pos = 0;
        while (pos < end) {
            int s = ((short)nybbles) >> 12;
            int shift = header >> 4;
            if (shift <= 12) s = (int)((unsigned)s << shift) >> 1; else s &= ~0x7FF;
            int flt = header & 0x0C;
            int p1 = v->buf[pos + BRR_BUF - 1];
            int p2 = v->buf[pos + BRR_BUF - 2] >> 1;
            if (flt >= 8) {
                s += p1; s -= p2;
                if (flt == 8) { s += p2 >> 4; s += (p1 * -3) >> 6; }
                else { s += (p1 * -13) >> 7; s += (p2 * 3) >> 4; }
            } else if (flt) { s += p1 >> 1; s += (-p1) >> 5; }
            s = clamp16(s);
            s = (short)(s * 2);
            v->buf[pos + BRR_BUF] = s; v->buf[pos] = s;
            pos++; nybbles <<= 4;
        }
    }
    inline int interpolate(Voice* v) {
        int offset = v->interp_pos >> 4 & 0xFF;
        int* in = &v->buf[(v->interp_pos >> 12) + v->buf_pos];
        int fwd = 255 - offset, rev = offset;
        int o = (GAUSS[fwd] * in[0]) >> 11;
        o += (GAUSS[fwd + 256] * in[1]) >> 11;
        o += (GAUSS[rev + 256] * in[2]) >> 11;
        o = (short)o;
        o += (GAUSS[rev] * in[3]) >> 11;
        o = clamp16(o); o &= ~1;
        return o;
    }
    inline void misc_27() { m.t_pmon = m.regs[R_PMON] & 0xFE; }
    inline void misc_28() { m.t_non = m.regs[R_NON]; m.t_eon = m.regs[R_EON]; m.t_dir = m.regs[R_DIR]; }
    inline void misc_29() { if ((m.every_other_sample ^= 1) != 0) m.new_kon &= ~m.kon; }
    inline void misc_30() {
        if (m.every_other_sample) { m.kon = m.new_kon; m.t_koff = m.regs[R_KOFF] | m.mute_mask; }
        run_counters();
        if (!read_counter(m.regs[R_FLG] & 0x1F)) {
            int feedback = (m.noise << 13) ^ (m.noise << 14);
            m.noise = (feedback & 0x4000) ^ (m.noise >> 1);
        }
    }
    inline void V1(Voice* v) { m.t_dir_addr = (m.t_dir * 0x100 + m.t_srcn * 4) & 0xFFFF; m.t_srcn = m.regs[v->regs + V_SRCN]; }
    inline void V2(Voice* v) {
        int entry = m.t_dir_addr; if (!v->kon_delay) entry += 2;
        m.t_brr_next_addr = ram[entry & 0xFFFF] | (ram[(entry + 1) & 0xFFFF] << 8);
        m.t_adsr0 = m.regs[v->regs + V_ADSR0];
        m.t_pitch = m.regs[v->regs + V_PITCHL];
    }
    inline void V3a(Voice* v) { m.t_pitch += (m.regs[v->regs + V_PITCHH] & 0x3F) << 8; }
    inline void V3b(Voice* v) { m.t_brr_byte = ram[(v->brr_addr + v->brr_offset) & 0xFFFF]; m.t_brr_header = ram[v->brr_addr]; }
    inline void V3c(Voice* v) {
        if (m.t_pmon & v->vbit) m.t_pitch += ((m.t_output >> 5) * m.t_pitch) >> 10;
        if (v->kon_delay) {
            if (v->kon_delay == 5) { v->brr_addr = m.t_brr_next_addr; v->brr_offset = 1; v->buf_pos = 0; m.t_brr_header = 0; m.kon_check = 1; }
            v->env = 0; v->hidden_env = 0; v->interp_pos = 0;
            if (--v->kon_delay & 3) v->interp_pos = 0x4000;
            m.t_pitch = 0;
        }
        int output = interpolate(v);
        if (m.t_non & v->vbit) output = (short)(m.noise * 2);
        m.t_output = (output * v->env) >> 11 & ~1;
        v->t_envx_out = (v->env >> 4) & 0xFF;
        if (m.regs[R_FLG] & 0x80 || (m.t_brr_header & 3) == 1) { v->env_mode = ENV_RELEASE; v->env = 0; }
        if (m.every_other_sample) {
            if (m.t_koff & v->vbit) v->env_mode = ENV_RELEASE;
            if (m.kon & v->vbit) { v->kon_delay = 5; v->env_mode = ENV_ATTACK; }
        }
        if (!v->kon_delay) run_envelope(v);
    }
    inline void voice_output(Voice* v, int ch) {
        int amp = (m.t_output * (signed char)m.regs[v->regs + V_VOLL + ch]) >> 7;
        m.t_main_out[ch] = clamp16(m.t_main_out[ch] + amp);
        if (m.t_eon & v->vbit) m.t_echo_out[ch] = clamp16(m.t_echo_out[ch] + amp);
    }
    inline void V3(Voice* v) { V3a(v); V3b(v); V3c(v); }
    inline void V4(Voice* v) {
        m.t_looped = 0;
        if (v->interp_pos >= 0x4000) {
            decode_brr(v);
            if ((v->brr_offset += 2) >= BRR_BLOCK) {
                v->brr_addr = (v->brr_addr + BRR_BLOCK) & 0xFFFF;
                if (m.t_brr_header & 1) { v->brr_addr = m.t_brr_next_addr; m.t_looped = v->vbit; }
                v->brr_offset = 1;
            }
        }
        v->interp_pos = (v->interp_pos & 0x3FFF) + m.t_pitch;
        if (v->interp_pos > 0x7FFF) v->interp_pos = 0x7FFF;
        voice_output(v, 0);
    }
    inline void V5(Voice* v) {
        voice_output(v, 1);
        int endx_buf = m.regs[R_ENDX] | m.t_looped;
        if (v->kon_delay == 5) endx_buf &= ~v->vbit;
        m.endx_buf = endx_buf & 0xFF;
    }
    inline void V6(Voice*) { m.outx_buf = (m.t_output >> 8) & 0xFF; }
    inline void V7(Voice* v) { m.regs[R_ENDX] = m.endx_buf; m.external_regs[R_ENDX] = m.endx_buf; m.envx_buf = v->t_envx_out; }
    inline void V8(Voice* v) { m.regs[v->regs + V_OUTX] = m.outx_buf; m.external_regs[v->regs + V_OUTX] = m.outx_buf; }
    inline void V9(Voice* v) { m.regs[v->regs + V_ENVX] = m.envx_buf; m.external_regs[v->regs + V_ENVX] = m.envx_buf; }
    inline void V7_V4_V1(Voice* v) { V7(v); V1(v + 3); V4(v + 1); }
    inline void V8_V5_V2(Voice* v) { V8(v); V5(v + 1); V2(v + 2); }
    inline void V9_V6_V3(Voice* v) { V9(v); V6(v + 1); V3(v + 2); }
    inline int calc_fir(int i, int ch) { return (m.echo_hist[m.echo_hist_pos + i + 1][ch] * (signed char)m.regs[R_FIR + i * 0x10]) >> 6; }
    inline void echo_read(int ch) {
        int a = m.t_echo_ptr + ch * 2;
        int s = (short)(ram[a & 0xFFFF] | (ram[(a + 1) & 0xFFFF] << 8));
        m.echo_hist[m.echo_hist_pos][ch] = s >> 1;
        m.echo_hist[m.echo_hist_pos + 8][ch] = s >> 1;
    }
    inline void echo_22() {
        if (++m.echo_hist_pos >= 8) m.echo_hist_pos = 0;
        m.t_echo_ptr = (m.t_esa * 0x100 + m.echo_offset) & 0xFFFF;
        echo_read(0);
        int l = calc_fir(0, 0), r = calc_fir(0, 1);
        m.t_echo_in[0] = l; m.t_echo_in[1] = r;
    }
    inline void echo_23() {
        int l = calc_fir(1, 0) + calc_fir(2, 0), r = calc_fir(1, 1) + calc_fir(2, 1);
        m.t_echo_in[0] += l; m.t_echo_in[1] += r;
        echo_read(1);
    }
    inline void echo_24() {
        int l = calc_fir(3, 0) + calc_fir(4, 0) + calc_fir(5, 0), r = calc_fir(3, 1) + calc_fir(4, 1) + calc_fir(5, 1);
        m.t_echo_in[0] += l; m.t_echo_in[1] += r;
    }
    inline void echo_25() {
        int l = m.t_echo_in[0] + calc_fir(6, 0), r = m.t_echo_in[1] + calc_fir(6, 1);
        l = (short)l; r = (short)r;
        l += (short)calc_fir(7, 0); r += (short)calc_fir(7, 1);
        l = clamp16(l); r = clamp16(r);
        m.t_echo_in[0] = l & ~1; m.t_echo_in[1] = r & ~1;
    }
    inline int echo_output(int ch) {
        int o = (short)((m.t_main_out[ch] * (signed char)m.regs[R_MVOLL + ch * 0x10]) >> 7) +
                (short)((m.t_echo_in[ch] * (signed char)m.regs[R_EVOLL + ch * 0x10]) >> 7);
        return clamp16(o);
    }
    inline void echo_26() {
        m.t_main_out[0] = echo_output(0);
        int l = m.t_echo_out[0] + (short)((m.t_echo_in[0] * (signed char)m.regs[R_EFB]) >> 7);
        int r = m.t_echo_out[1] + (short)((m.t_echo_in[1] * (signed char)m.regs[R_EFB]) >> 7);
        l = clamp16(l); r = clamp16(r);
        m.t_echo_out[0] = l & ~1; m.t_echo_out[1] = r & ~1;
    }
    inline void echo_27() {
        int l = m.t_main_out[0], r = echo_output(1);
        m.t_main_out[0] = 0; m.t_main_out[1] = 0;
        if (m.regs[R_FLG] & 0x40) { l = 0; r = 0; }
        if (out.size() / 2 < out_cap) { out.push_back((int16_t)l); out.push_back((int16_t)r); }
    }
    inline void echo_28() { m.t_echo_enabled = m.regs[R_FLG]; }
    inline void echo_write(int ch) {
        if (!(m.t_echo_enabled & 0x20)) {
            int a = m.t_echo_ptr + ch * 2, v = m.t_echo_out[ch];
            ram[a & 0xFFFF] = v & 0xFF; ram[(a + 1) & 0xFFFF] = (v >> 8) & 0xFF;
        }
        m.t_echo_out[ch] = 0;
    }
    inline void echo_29() {
        m.t_esa = m.regs[R_ESA];
        if (!m.echo_offset) m.echo_length = (m.regs[R_EDL] & 0x0F) * 0x800;
        m.echo_offset += 4;
        if (m.echo_offset >= m.echo_length) m.echo_offset = 0;
        echo_write(0);
        m.t_echo_enabled = m.regs[R_FLG];
    }
    inline void echo_30() { echo_write(1); }
    void clock(int phase) {
        Voice* V = m.voices;
        switch (phase) {
        case 0: V5(&V[0]); V2(&V[1]); break;
        case 1: V6(&V[0]); V3(&V[1]); break;
        case 2: V7_V4_V1(&V[0]); break;
        case 3: V8_V5_V2(&V[0]); break;
        case 4: V9_V6_V3(&V[0]); break;
        case 5: V7_V4_V1(&V[1]); break;
        case 6: V8_V5_V2(&V[1]); break;
        case 7: V9_V6_V3(&V[1]); break;
        case 8: V7_V4_V1(&V[2]); break;
        case 9: V8_V5_V2(&V[2]); break;
        case 10: V9_V6_V3(&V[2]); break;
        case 11: V7_V4_V1(&V[3]); break;
        case 12: V8_V5_V2(&V[3]); break;
        case 13: V9_V6_V3(&V[3]); break;
        case 14: V7_V4_V1(&V[4]); break;
        case 15: V8_V5_V2(&V[4]); break;
        case 16: V9_V6_V3(&V[4]); break;
        case 17: V1(&V[0]); V7(&V[5]); V4(&V[6]); break;
        case 18: V8_V5_V2(&V[5]); break;
        case 19: V9_V6_V3(&V[5]); break;
        case 20: V1(&V[1]); V7(&V[6]); V4(&V[7]); break;
        case 21: V8(&V[6]); V5(&V[7]); V2(&V[0]); break;
        case 22: V3a(&V[0]); V9(&V[6]); V6(&V[7]); echo_22(); break;
        case 23: V7(&V[7]); echo_23(); break;
        case 24: V8(&V[7]); echo_24(); break;
        case 25: V3b(&V[0]); V9(&V[7]); echo_25(); break;
        case 26: echo_26(); break;
        case 27: misc_27(); echo_27(); break;
        case 28: misc_28(); echo_28(); break;
        case 29: misc_29(); echo_29(); break;
        case 30: misc_30(); V3c(&V[0]); echo_30(); break;
        default: V4(&V[0]); V1(&V[2]); break;
        }
    }
    void run(i64 clocks) {
        int phase = m.phase;
        while (clocks > 0) { clock(phase); phase = (phase + 1) & 31; clocks--; }
        m.phase = phase;
    }
};

// ===================================================================== APU (S-SMP side)
static const int SPC_PER_FRAME = 17039;                 // 1.024 MHz / 60.0988 Hz
static const int CPU_PER_FRAME = LINES * LINE_CYC;      // CPU cycle units used by this core
static const int TIMER_PER[3] = {128, 128, 16};

struct APU : SPCBus {
    u8 ram[0x10000];
    u8 dsp[128];
    int dsp_addr = 0;
    int pin[4] = {0}, pout[4] = {0};
    bool ipl_on = true; int ipl_st = 0;       // 0=None 1='cc' 2='z' 3='x'
    int ipl_y = 0;
    int t_en[3] = {0}, t_tgt[3] = {0}, t_cnt[3] = {0}, t_int[3] = {0};
    i64 t_last[3] = {0};
    u64 wc = 0, treads = 0;
    // idle keys
    bool pk_valid = false; int pk[5] = {0}; u64 pk_wc = 0, pk_tr = 0;
    bool tk_valid = false; int tk[5] = {0}; u64 tk_wc = 0; int tk_i = 0;
    bool port_defer = false;
    SPC700 spc;
    SPCDSP dspc;
    i64 dsp_clock = 0;

    APU() : spc(this), dspc(&ram[0]) { reset(); }   // dspc keeps only the pointer; reset() clears ram

    void reset() {
        memset(ram, 0, sizeof(ram));
        memset(dsp, 0, sizeof(dsp)); dsp[0x6C] = 0xE0; dsp_addr = 0;
        for (int i = 0; i < 4; i++) pin[i] = pout[i] = 0;
        ipl_on = true; ipl_st = 0; ipl_y = 0;
        for (int i = 0; i < 3; i++) { t_en[i] = t_tgt[i] = t_cnt[i] = t_int[i] = 0; t_last[i] = 0; }
        wc = 0; treads = 0; pk_valid = tk_valid = false; port_defer = false;
        dspc.reset(); dspc.take(); dsp_clock = 0;
        spc.A = spc.X = spc.Y = 0; spc.SP = 0xEF; spc.PSW = 0x02; spc.PC = 0xFFC0;
        spc.cyc = 0; spc.stopped = false; spc.trap = 0xFFC0;
    }

    // ---- DSP wrapper (catch up to the SPC700 on every register access)
    void dsp_render_to(i64 spc_cyc) { i64 n = spc_cyc - dsp_clock; if (n > 0) { dspc.run(n); dsp_clock = spc_cyc; } }
    int dsp_read(int reg) { dsp_render_to(spc.cyc); return dspc.read(reg); }
    void dsp_write(int reg, int val) { dsp_render_to(spc.cyc); dspc.write(reg, val); dsp[reg & 0x7F] = val & 0xFF; }

    u8 read(int a) override {
        if (a >= 0xF0) {
            if (a <= 0xFF) return (u8)io_read(a);
            if (a >= 0xFFC0 && ipl_on) return a == 0xFFFE ? 0xC0 : (a == 0xFFFF ? 0xFF : 0);
        }
        return ram[a];
    }
    void write(int a, int v) override {
        if (a >= 0xF0 && a <= 0xFF) { ram[a] = v; io_write(a, v); }
        else if (ram[a] != v) { ram[a] = v; wc++; }
    }

    void tsync(int i) {
        if (!t_en[i]) return;
        int p = TIMER_PER[i]; i64 n = floordiv(spc.cyc - t_last[i], p);
        if (n > 0) {
            t_last[i] += n * p;
            int t = t_tgt[i] ? t_tgt[i] : 256;
            i64 tot = t_int[i] + n;
            t_cnt[i] = (int)((t_cnt[i] + tot / t) & 15);
            t_int[i] = (int)(tot % t);
        }
    }
    bool next_tick(i64& best) {
        bool any = false;
        for (int i = 0; i < 3; i++) if (t_en[i]) {
            int t = t_tgt[i] ? t_tgt[i] : 256;
            i64 n = t_last[i] + (i64)(t - t_int[i]) * TIMER_PER[i];
            if (!any || n < best) best = n;
            any = true;
        }
        return any;
    }
    int io_read(int a) {
        if (a >= 0xF4 && a <= 0xF7) {
            int key[5] = {spc.PC, spc.A, spc.X, spc.Y, spc.SP};
            if (pk_valid && !memcmp(key, pk, sizeof key) && pk_wc == wc && pk_tr == treads) {
                if (spc.cyc < spc.target) {
                    i64 nt = 0;
                    if (!next_tick(nt)) spc.cyc = spc.target;
                    else spc.cyc = std::max(spc.cyc, std::min(spc.target, nt));
                }
            } else { pk_valid = true; memcpy(pk, key, sizeof key); pk_wc = wc; pk_tr = treads; }
            return pin[a - 0xF4];
        }
        if (a >= 0xFD) {
            int i = a - 0xFD; tsync(i);
            int v = t_cnt[i]; t_cnt[i] = 0;
            if (v) { treads++; tk_valid = false; }
            else {
                int key[5] = {spc.PC, spc.A, spc.X, spc.Y, spc.SP};
                if (tk_valid && !memcmp(key, tk, sizeof key) && tk_wc == wc && tk_i == i && t_en[i]) {
                    int t = t_tgt[i] ? t_tgt[i] : 256;
                    i64 need = t_last[i] + (i64)(t - t_int[i]) * TIMER_PER[i];
                    if (need > spc.cyc) spc.cyc = std::min(spc.target, need);
                } else { tk_valid = true; memcpy(tk, key, sizeof key); tk_wc = wc; tk_i = i; }
            }
            return v;
        }
        if (a == 0xF2) return dsp_addr;
        if (a == 0xF3) return dsp_read(dsp_addr & 0x7F);
        if (a == 0xF8 || a == 0xF9) return ram[a];
        return 0;
    }
    void io_write(int a, int v) {
        if (a >= 0xF4 && a <= 0xF7) {
            if (pout[a - 0xF4] != v) { pout[a - 0xF4] = v; wc++; }
            return;
        }
        wc++;
        if (a == 0xF1) {
            for (int i = 0; i < 3; i++) {
                tsync(i);
                if (v & (1 << i)) {
                    if (!t_en[i]) { t_en[i] = 1; t_cnt[i] = 0; t_int[i] = 0; t_last[i] = spc.cyc; }
                } else t_en[i] = 0;
            }
            if (v & 0x10) pin[0] = pin[1] = 0;
            if (v & 0x20) pin[2] = pin[3] = 0;
            ipl_on = (v & 0x80) != 0;
            spc.trap = ipl_on ? 0xFFC0 : 0x10000;
        } else if (a == 0xF2) dsp_addr = v;
        else if (a == 0xF3) { int r = dsp_addr; if (r < 0x80) dsp_write(r, v); }
        else if (a >= 0xFA && a <= 0xFC) { int i = a - 0xFA; tsync(i); t_tgt[i] = v; }
    }

    // ---- HLE IPL boot ROM (documented upload protocol, no Nintendo code)
    void ipl_cmd() {
        ram[0] = pin[2]; ram[1] = pin[3];
        spc.A = pin[0]; spc.Y = pin[1]; pout[0] = pin[0];
        spc.cyc += 18;
        if (pin[1] == 0) {
            spc.X = 0; spc.PSW = 0x02; spc.SP = 0xEF;
            spc.PC = ram[0] | (ram[1] << 8); ipl_st = 0;
        } else { spc.X = pin[1]; ipl_st = 2; }
    }
    void ipl_step(i64 t) {
        while (spc.cyc < t && spc.PC >= 0xFFC0 && ipl_on) {
            int st = ipl_st;
            if (st == 0) {
                if (spc.PC == 0xFFC0) { memset(ram + 1, 0, 0xEF); spc.cyc += 1200; }
                spc.SP = 0xEF; spc.A = 0; spc.X = 0;
                pout[0] = 0xAA; pout[1] = 0xBB;
                ipl_st = 1; spc.PC = 0xFFCF; spc.cyc += 10;
            } else if (st == 1) {
                if (pin[0] == 0xCC) ipl_cmd(); else spc.cyc = t;
            } else if (st == 2) {
                if (pin[0] == 0) { ipl_y = 0; ipl_st = 3; } else spc.cyc = t;
            } else {
                int p0 = pin[0], y = ipl_y;
                if (p0 == y) {
                    write(((ram[0] | (ram[1] << 8)) + y) & 0xFFFF, pin[1]);
                    pout[0] = y; y = (y + 1) & 0xFF;
                    if (y == 0) ram[1] = (ram[1] + 1) & 0xFF;
                    ipl_y = y; spc.cyc += 22;
                } else if ((y - p0) & 0x80) ipl_cmd();
                else spc.cyc = t;
            }
        }
    }
    void run_to(i64 cpu_cyc) {
        port_defer = false;
        i64 t = floordiv(cpu_cyc * SPC_PER_FRAME, CPU_PER_FRAME);
        while (spc.cyc < t) {
            if (spc.PC >= spc.trap) ipl_step(t);
            else spc.run(t);
        }
    }
};

// ===================================================================== save-state serializer
struct Ser {
    bool loading = false; std::vector<u8> buf; size_t pos = 0;
    void raw(void* p, size_t n) {
        if (loading) {
            if (pos + n > buf.size()) throw std::runtime_error("state file truncated");
            memcpy(p, buf.data() + pos, n); pos += n;
        } else { const u8* b = (const u8*)p; buf.insert(buf.end(), b, b + n); }
    }
    template <class T> void pod(T& v) { raw(&v, sizeof(T)); }
    template <class T> void vec(std::vector<T>& v) {
        u64 n = v.size(); pod(n);
        if (loading) { if (n > (1u << 28)) throw std::runtime_error("bad state"); v.resize((size_t)n); }
        if (n) raw(v.data(), (size_t)n * sizeof(T));
    }
    void str(std::string& s) { u64 n = s.size(); pod(n); if (loading) s.resize((size_t)n); if (n) raw(&s[0], (size_t)n); }
};

// ===================================================================== PPU register file
enum {
    R_INIDISP, R_OBSEL, R_BGMODE, R_MOSAIC, R_BG1SC, R_BG2SC, R_BG3SC, R_BG4SC, R_BG12NBA, R_BG34NBA,
    R_BG1HOFS, R_BG1VOFS, R_BG2HOFS, R_BG2VOFS, R_BG3HOFS, R_BG3VOFS, R_BG4HOFS, R_BG4VOFS,
    R_M7SEL, R_M7A, R_M7B, R_M7C, R_M7D, R_M7X, R_M7Y, R_M7HOFS, R_M7VOFS, R_W12SEL, R_W34SEL, R_WOBJSEL,
    R_WH0, R_WH1, R_WH2, R_WH3, R_WBGLOG, R_WOBJLOG, R_TM, R_TS, R_TMW, R_TSW, R_CGWSEL, R_CGADSUB,
    R_COLR, R_COLG, R_COLB, R_SETINI, NR };
typedef std::array<int, NR> RegSnap;
typedef std::array<u16, 256> CgSnap;

static const int PAT_LEN[8] = {1, 2, 2, 4, 4, 4, 2, 4};
static const int PAT[8][4] = {{0}, {0, 1}, {0, 0}, {0, 0, 1, 1}, {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0}, {0, 0, 1, 1}};
static const int VINC[4] = {1, 32, 128, 128};
static const int OBJ_SIZES[8][2][2] = {{{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}}};

// ===================================================================== commercial cartridge info
static bool in_set(int v, std::initializer_list<int> s) { for (int x : s) if (x == v) return true; return false; }
static bool is_sa1(int c) { return in_set(c, {0x32, 0x33, 0x34, 0x35}); }
static bool is_dsp(int c) { return in_set(c, {0x03, 0x04, 0x05}); }
static bool is_sfx(int c) { return in_set(c, {0x13, 0x14, 0x15, 0x1A}); }
static bool is_obc1(int c) { return c == 0x25; }
static bool is_sdd1(int c) { return in_set(c, {0x43, 0x45}); }
static bool is_srtc(int c) { return c == 0x55; }
static bool is_cx4(int c) { return c == 0xF3; }
static bool is_st01x(int c) { return c == 0xF5; }
static bool is_spc7110(int c) { return c == 0xF9; }
static bool is_sgb(int c) { return in_set(c, {0xE3, 0xE5}); }

struct ChipInfo { int code; const char* name; const char* compat; };
static const ChipInfo CHIPSET_INFO[] = {
    {0x00, "ROM", "ok"}, {0x01, "ROM+RAM", "ok"}, {0x02, "ROM+RAM+Battery", "ok"},
    {0x03, "DSP-1", "ok"}, {0x04, "DSP-1+RAM", "ok"}, {0x05, "DSP-1+RAM+Battery", "ok"},
    {0x13, "SuperFX", "ok"}, {0x14, "SuperFX+RAM", "ok"}, {0x15, "SuperFX+RAM+Battery", "ok"},
    {0x1A, "SuperFX+RAM+Battery", "ok"}, {0x25, "OBC1", "ok"},
    {0x32, "SA-1", "ok"}, {0x33, "SA-1", "ok"}, {0x34, "SA-1+RAM", "ok"}, {0x35, "SA-1+RAM+Battery", "ok"},
    {0x43, "S-DD1", "ok"}, {0x45, "S-DD1+RAM+Battery", "ok"}, {0x55, "S-RTC", "ok"},
    {0xE3, "SGB", "ok"}, {0xE5, "SGB", "ok"}, {0xF3, "CX4", "ok"}, {0xF5, "ST010/ST011", "ok"},
    {0xF6, "ST018", "chip"}, {0xF9, "SPC7110", "ok"}};
static const ChipInfo* chip_info(int c) { for (auto& ci : CHIPSET_INFO) if (ci.code == c) return &ci; return nullptr; }
static std::string region_name(int r) {
    static const char* N[] = {"Japan", "USA", "Europe", "Sweden", "Finland", "Denmark", "France", "Netherlands",
        "Spain", "Germany", "Italy", "China", "Indonesia", "Korea", "Common", "Canada", "Brazil", "Australia", "Other"};
    if (r >= 0 && r <= 18) return N[r];
    return "Region " + std::to_string(r);
}
static const char* ROM_EXTS[] = {".sfc", ".smc", ".swc", ".fig", ".bin"};
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool ends_with(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
static bool is_rom_name(const std::string& n) { std::string l = lower(n); for (auto e : ROM_EXTS) if (ends_with(l, e)) return true; return false; }

static std::vector<u8> strip_copier_header(const std::vector<u8>& d) {
    if (d.size() % 1024 == 512) return std::vector<u8>(d.begin() + 512, d.end());
    return d;
}

static int header_score(const std::vector<u8>& data, size_t off, const char* kind) {
    if (data.size() < off + 0x40) return -99;
    const u8* h = data.data() + off;
    int sc = 0;
    int csum = h[0x1C] | (h[0x1D] << 8), comp = h[0x1E] | (h[0x1F] << 8);
    if ((csum ^ comp) == 0xFFFF) sc += 8;
    int mm = h[0x15], mn = mm & 0x0F;
    std::string k = kind;
    if (k == "lorom" && (mn == 0 || mn == 2 || mn == 3) && off == 0x7FC0) sc += 4;
    if (k == "hirom" && (mn == 1 || mn == 3) && off == 0xFFC0) sc += 4;
    if (k == "exhirom" && mn == 5 && off == 0x40FFC0) sc += 6;
    if (k == "lorom" && !(mm & 1)) sc += 1;
    if ((k == "hirom" || k == "exhirom") && (mm & 1)) sc += 1;
    int rst = h[0x3C] | (h[0x3D] << 8);
    if (rst >= 0x8000) sc += 2; else sc -= 4;
    int title_ok = 0; for (int i = 0; i < 21; i++) if (h[i] >= 32 && h[i] < 127) title_ok++;
    sc += title_ok >= 18 ? 2 : (title_ok >= 12 ? 1 : -2);
    int rom_sz = h[0x17];
    if (rom_sz >= 0x07 && rom_sz <= 0x0D) sc += 1;
    int chip = h[0x16];
    if (chip <= 0x05 || chip_info(chip)) sc += 1;
    return sc;
}

struct RomInfo {
    std::vector<u8> data; std::string title; bool hirom = false, exhirom = false;
    int map_mode = 0, chipset = 0, rom_size_code = 0, sram_size = 0, region = 0, header_score = 0;
    size_t header_off = 0;
    std::string chip_name, region_name, mapper, speed, compat;
};

static RomInfo analyze_rom(const std::vector<u8>& raw) {
    RomInfo I;
    std::vector<u8> data = strip_copier_header(raw);
    if (data.size() < 0x8000) data.resize(0x8000, 0);
    struct C { int sc; size_t off; bool hi, ex; };
    std::vector<C> cands = {
        {header_score(data, 0x7FC0, "lorom"), 0x7FC0, false, false},
        {header_score(data, 0xFFC0, "hirom"), 0xFFC0, true, false},
        {header_score(data, 0x40FFC0, "exhirom"), 0x40FFC0, true, true}};
    if (data.size() > 0x8000 + 512) {
        struct B { size_t base; const char* kind; bool hi, ex; } bs[3] = {{0x7FC0, "lorom", false, false}, {0xFFC0, "hirom", true, false}, {0x40FFC0, "exhirom", true, true}};
        for (auto& b : bs) { size_t off = b.base + 512; if (off + 0x40 <= data.size()) cands.push_back({header_score(data, off, b.kind) - 3, off, b.hi, b.ex}); }
    }
    C best = cands[0];
    for (auto& c : cands) if (c.sc > best.sc) best = c;
    const u8* h = data.data() + best.off;
    std::string t; for (int i = 0; i < 21; i++) t += (h[i] >= 32 && h[i] < 127) ? (char)h[i] : ' ';
    size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
    I.title = a == std::string::npos ? "UNTITLED" : t.substr(a, b - a + 1);
    I.map_mode = h[0x15]; I.chipset = h[0x16]; I.rom_size_code = h[0x17];
    int sram_code = h[0x18]; I.region = h[0x19];
    int sram_cap = is_sa1(I.chipset) ? 0x40000 : 0x20000;
    I.sram_size = (sram_code > 0 && sram_code <= 8) ? std::min(1024 << sram_code, sram_cap) : 0;
    if (is_sfx(I.chipset) && I.sram_size == 0 && best.off >= 0x10) {
        int exp = data[best.off - 0x10 + 0x0D];
        if (exp > 0 && exp <= 8) I.sram_size = std::min(1024 << exp, 0x20000);
    }
    const ChipInfo* ci = chip_info(I.chipset);
    if (ci) { I.chip_name = ci->name; I.compat = ci->compat; }
    else { char buf[32]; snprintf(buf, sizeof buf, "Chipset $%02X", I.chipset); I.chip_name = buf; I.compat = "unknown"; }
    I.mapper = best.ex ? "ExHiROM" : (best.hi ? "HiROM" : "LoROM");
    if ((I.map_mode & 0x0F) == 3 || is_sa1(I.chipset)) {
        I.mapper = "SA-1";
        if (I.chip_name.find("SA-1") == std::string::npos) I.chip_name = "SA-1";
        I.compat = "ok";
    }
    I.speed = (I.map_mode & 0x10) ? "FastROM" : "SlowROM";
    I.hirom = best.hi; I.exhirom = best.ex; I.header_score = best.sc; I.header_off = best.off;
    I.region_name = region_name(I.region);
    I.data = std::move(data);
    return I;
}

// ===================================================================== system declaration
struct SA1; struct DSP1; struct SuperFX; struct OBC1; struct SRTC; struct SDD1; struct CX4; struct ST01x; struct SPC7110; struct SGB;

struct ObjBuf { int idx[VIS][256]; u8 pri[VIS][256]; bool op[VIS][256]; bool hi[VIS][256]; };

struct SNES : Bus {
    // cart
    std::vector<u8> rom; size_t romlen = 0x8000;
    bool hirom = false, exhirom = false; std::string title;
    int sram_size = 0, map_mode = 0, chipset = 0;
    std::string chip_name = "ROM", compat = "ok", mapper = "LoROM", region_name, speed = "SlowROM";
    std::vector<u8> sram; std::string rom_path;
    // fast page map
    u8* rptr[2048]; u8* wptr[2048];
    // components
    CPU65816 cpu;
    SA1* sa1 = nullptr; DSP1* dsp1 = nullptr; SuperFX* sfx = nullptr; OBC1* obc1 = nullptr; SRTC* srtc = nullptr;
    SDD1* sdd1 = nullptr; CX4* cx4 = nullptr; ST01x* st01x = nullptr; SPC7110* spc7110 = nullptr; SGB* sgb = nullptr;
    APU apu;
    std::vector<u8> frame;   // 224*256*3
    int frame_w = 256, frame_h = 224;
    int joy1 = 0, joy2 = 0; bool skip_render = false;
    // memory
    std::vector<u8> wram; std::vector<u16> vram; std::vector<u16> cgram; std::vector<u8> oam;
    RegSnap R{}; bool rdirty = true, cgdirty = true, vram_dirty = true;
    RegSnap rsnap{}; CgSnap cgsnap{};
    std::vector<RegSnap> line_regs; std::vector<CgSnap> line_cg;
    int mdr = 0;
    int bgofs_latch = 0, m7_latch = 0, mpy = 0, oam_base = 0, oam_addr = 0, oam_latch = 0;
    int vmain = 0x80, vmadd = 0, vlatch = 0, cgaddr = 0, cg_latch = 0, wmadd = 0;
    int nmitimen = 0, wrio = 0xFF, wrmpya = 0xFF, wrdiv = 0xFFFF, rddiv = 0, rdmpy = 0, htime = 0x1FF, vtime = 0x1FF;
    int hdmaen = 0, memsel = 0;
    u8 dmar[0x80];
    bool hdma_do[8], hdma_done[8];
    bool nmi_flag = false, irq_flag = false, vblank = false;
    int vcount = 0; i64 line_start = 0, line_end = 0;
    int joyreg[8] = {0}, joy_sr[2] = {0, 0}, joy_strobe = 0;
    int hlatch = 0, vlatch_c = 0, hl_flip = 0, vl_flip = 0, field = 0;
    u64 frames = 0;
    std::unique_ptr<ObjBuf> objbuf;

    SNES();
    ~SNES();
    RomInfo load(const std::vector<u8>& data, const std::string& path = "");
    size_t rom_offset(int bank, int off);
    void save_sram();
    void power();
    void reset() { power(); }
    void free_chips();
    int chip_read(u32 a, bool& hit);
    bool chip_write(u32 a, int v);
    u8 read(u32 a) override {
        u32 p = a >> 13; u8* b = rptr[p];
        if (b) return b[a & 0x1FFF];
        return (u8)read_slow(a);
    }
    void write(u32 a, u8 v) override {
        wc++;
        u32 p = a >> 13; u8* b = wptr[p];
        if (b) b[a & 0x1FFF] = v; else write_slow(a, v);
    }
    void build_map();
    int read_slow(u32 a);
    void write_slow(u32 a, int v);
    std::vector<u8> read_block(int bank, int addr, int n);
    int io_read(int off);
    void io_write(int off, int v);
    int cpuio_read(int off);
    void cpuio_write(int off, int v);
    int ppu_read(int r);
    void ppu_write(int r, int v);
    int vremap();
    void do_dma(int mask);
    void hdma_init();
    void hdma_line();
    void sync_irq_line();
    void raise_irq() { irq_flag = true; sync_irq_line(); }
    void snap_line();
    void run_frame();
    std::vector<u8> save_state();
    void load_state(const std::vector<u8>& blob);
    void ser(Ser& s);
    // renderer
    void render();
    void render_obj();
    void render_line(int y, const RegSnap& L, const CgSnap& pal, u8* outrow);
};

// ===================================================================== Commercial enhancement chips
// Custom HLE / cores from public hardware docs. Not a Snes9x port.

// --------------------------------------------------------------------- DSP-1 / DSP-1B (HLE)
struct DSP1 {
    SNES* snes; bool hirom;
    int cmd = 0, in_count = 0, out_count = 0, in_idx = 0, out_idx = 0;
    int inputs[8] = {0}; std::vector<int> outputs;
    bool waiting_cmd = true;
    int mA[3][3], mB[3][3], mC[3][3];
    int les = 0, aas = 0, azs = 0;
    bool wlo = true; int wbuf = 0; bool rlo = true; int rword = 0;

    DSP1(SNES* s, bool hi) : snes(s), hirom(hi) { power(); }
    void power() {
        cmd = 0; in_count = out_count = 0; in_idx = out_idx = 0;
        for (auto& i : inputs) i = 0;
        outputs.assign(1024, 0);
        waiting_cmd = true;
        memset(mA, 0, sizeof mA); memset(mB, 0, sizeof mB); memset(mC, 0, sizeof mC);
        les = aas = azs = 0;
    }
    static int dsin(int a) { return ((int)(std::sin(a * (2 * CL_PI / 65536.0)) * 32768)) & 0xFFFF; }
    static int dcos(int a) { return ((int)(std::cos(a * (2 * CL_PI / 65536.0)) * 32768)) & 0xFFFF; }
    static void meta(int c, int& ni, int& no) {
        static const std::map<int, std::pair<int, int>> table = {
            {0x00, {2, 1}}, {0x10, {2, 2}}, {0x04, {2, 2}}, {0x08, {3, 2}}, {0x18, {4, 1}},
            {0x28, {3, 1}}, {0x0C, {3, 2}}, {0x1C, {6, 3}}, {0x02, {7, 4}}, {0x06, {3, 3}},
            {0x0A, {1, 4}}, {0x0E, {2, 2}}, {0x01, {4, 0}}, {0x11, {4, 0}}, {0x21, {4, 0}},
            {0x0D, {3, 3}}, {0x1D, {3, 3}}, {0x2D, {3, 3}}, {0x03, {3, 3}}, {0x13, {3, 3}},
            {0x23, {3, 3}}, {0x0B, {3, 1}}, {0x1B, {3, 1}}, {0x2B, {3, 1}}, {0x14, {6, 3}},
            {0x0F, {1, 1}}, {0x1F, {1, 1024}}, {0x2F, {1, 1}}, {0x20, {2, 1}}};
        static const std::map<int, int> aliases = {{0x05, 0x01}, {0x09, 0x0D}, {0x0D, 0x0D}, {0x12, 0x02}, {0x15, 0x11},
            {0x16, 0x06}, {0x19, 0x1D}, {0x22, 0x02}, {0x24, 0x04}, {0x25, 0x21}, {0x26, 0x06}, {0x29, 0x2D},
            {0x2C, 0x0C}, {0x2E, 0x0E}};
        c &= 0x3F;
        auto it = table.find(c);
        if (it == table.end()) { auto al = aliases.find(c); it = table.find(al == aliases.end() ? c : al->second); }
        if (it == table.end()) { ni = no = 0; return; }
        ni = it->second.first; no = it->second.second;
    }
    void write_data(int v) {
        v &= 0xFF;
        if (wlo) { wbuf = v; wlo = false; return; }
        int word = wbuf | (v << 8);
        wlo = true;
        write_word(word);
    }
    void write_word(int w) {
        if (waiting_cmd) {
            cmd = w & 0x3F; in_idx = 0; out_idx = 0;
            meta(cmd, in_count, out_count);
            waiting_cmd = in_count == 0;
            if (in_count == 0) execute();
            return;
        }
        if (in_idx < 8) inputs[in_idx] = w & 0xFFFF;
        in_idx++;
        if (in_idx >= in_count) execute();
    }
    int read_data() {
        if (rlo) {
            if (out_idx < out_count) {
                rword = out_idx < (int)outputs.size() ? outputs[out_idx] & 0xFFFF : 0; out_idx++;   // the Python version raised IndexError here
                if (out_idx >= out_count) waiting_cmd = true;
            } else rword = 0;
            rlo = false; return rword & 0xFF;
        }
        rlo = true; return (rword >> 8) & 0xFF;
    }
    int read_status() { return 0x80; }
    void set_out(std::initializer_list<int> vals) { outputs.assign(vals.begin(), vals.end()); out_count = (int)outputs.size(); out_idx = 0; waiting_cmd = false; }
    void attitude(int mat[3][3]) {
        int az = inputs[0], ay = inputs[1], ax = inputs[2];
        double axf = ax * (2 * CL_PI / 65536.0), ayf = ay * (2 * CL_PI / 65536.0), azf = az * (2 * CL_PI / 65536.0);
        double cx = cos(axf), sx = sin(axf), cy = cos(ayf), sy = sin(ayf), cz = cos(azf), sz = sin(azf);
        double r[3][3] = {{cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz},
                          {cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz},
                          {-sy, sx * cy, cx * cy}};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) mat[i][j] = u16v((i64)(r[i][j] * 32768));
        set_out({});
    }
    void execute() {
        int c = cmd & 0x3F; int* I = inputs;
        if (c == 0x00 || c == 0x20) { set_out({u16v((s16v(I[0]) * s16v(I[1])) >> 15)}); }
        else if (c == 0x10) {
            int m = s16v(I[0]), e = s16v(I[1]);
            if (m == 0) set_out({0x7FFF, e});
            else {
                i64 inv = (i64)((1.0 / (m / 32768.0)) * 32768);
                while (std::llabs(inv) >= 32768) { inv = floordiv(inv, 2); e++; }
                while (std::llabs(inv) < 16384 && inv != 0) { inv *= 2; e--; }
                set_out({u16v(inv), u16v(e)});
            }
        } else if (c == 0x04 || c == 0x24) {
            int a = I[0], r = s16v(I[1]); int s = s16v(dsin(a)), co = s16v(dcos(a));
            set_out({u16v((r * s) >> 15), u16v((r * co) >> 15)});
        } else if (c == 0x08) {
            i64 s = (i64)s16v(I[0]) * s16v(I[0]) + (i64)s16v(I[1]) * s16v(I[1]) + (i64)s16v(I[2]) * s16v(I[2]);
            set_out({(int)(s & 0xFFFF), (int)((s >> 16) & 0xFFFF)});
        } else if (c == 0x18) {
            i64 s = (i64)s16v(I[0]) * s16v(I[0]) + (i64)s16v(I[1]) * s16v(I[1]) + (i64)s16v(I[2]) * s16v(I[2]) - (i64)s16v(I[3]) * s16v(I[3]);
            set_out({u16v(s >> 15)});
        } else if (c == 0x28) {
            i64 s = (i64)s16v(I[0]) * s16v(I[0]) + (i64)s16v(I[1]) * s16v(I[1]) + (i64)s16v(I[2]) * s16v(I[2]);
            set_out({u16v((i64)std::sqrt((double)std::max<i64>(s, 0)))});
        } else if (c == 0x0C || c == 0x2C) {
            int a = I[0], x = s16v(I[1]), y = s16v(I[2]); int s = s16v(dsin(a)), co = s16v(dcos(a));
            set_out({u16v((x * co - y * s) >> 15), u16v((x * s + y * co) >> 15)});
        } else if (c == 0x1C) {
            double axf = I[0] * (2 * CL_PI / 65536.0), ayf = I[1] * (2 * CL_PI / 65536.0), azf = I[2] * (2 * CL_PI / 65536.0);
            double x = s16v(I[3]), y = s16v(I[4]), z = s16v(I[5]), t1, t2;
            t1 = y * cos(axf) - z * sin(axf); t2 = y * sin(axf) + z * cos(axf); y = t1; z = t2;
            t1 = x * cos(ayf) + z * sin(ayf); t2 = -x * sin(ayf) + z * cos(ayf); x = t1; z = t2;
            t1 = x * cos(azf) - y * sin(azf); t2 = x * sin(azf) + y * cos(azf); x = t1; y = t2;
            set_out({u16v((i64)x), u16v((i64)y), u16v((i64)z)});
        } else if (c == 0x01 || c == 0x05) attitude(mA);
        else if (c == 0x11 || c == 0x15) attitude(mB);
        else if (c == 0x21 || c == 0x25) attitude(mC);
        else if (c == 0x02 || c == 0x12 || c == 0x22) { les = I[4]; aas = I[5]; azs = I[6]; set_out({I[0], I[1], I[2], I[3]}); }
        else if (c == 0x06 || c == 0x16 || c == 0x26) {
            int x = s16v(I[0]), y = s16v(I[1]), z = s16v(I[2]); if (z == 0) z = 1;
            set_out({u16v(floordiv((i64)x * 256, z)), u16v(floordiv((i64)y * 256, z)), u16v(z)});
        } else if (c == 0x0A) set_out({les, aas, azs, 0});
        else if (c == 0x0E || c == 0x2E) set_out({I[0], I[1]});
        else if (c == 0x0D || c == 0x09 || c == 0x1D || c == 0x19 || c == 0x2D || c == 0x29) {
            int (*mat)[3] = (c == 0x0D || c == 0x09) ? mA : ((c == 0x1D || c == 0x19) ? mB : mC);
            int x = s16v(I[0]), y = s16v(I[1]), z = s16v(I[2]);
            i64 ox = ((i64)s16v(mat[0][0]) * x + (i64)s16v(mat[0][1]) * y + (i64)s16v(mat[0][2]) * z) >> 15;
            i64 oy = ((i64)s16v(mat[1][0]) * x + (i64)s16v(mat[1][1]) * y + (i64)s16v(mat[1][2]) * z) >> 15;
            i64 oz = ((i64)s16v(mat[2][0]) * x + (i64)s16v(mat[2][1]) * y + (i64)s16v(mat[2][2]) * z) >> 15;
            set_out({u16v(ox), u16v(oy), u16v(oz)});
        } else if (c == 0x03 || c == 0x13 || c == 0x23) {
            int (*mat)[3] = c == 0x03 ? mA : (c == 0x13 ? mB : mC);
            int x = s16v(I[0]), y = s16v(I[1]), z = s16v(I[2]);
            i64 ox = ((i64)s16v(mat[0][0]) * x + (i64)s16v(mat[1][0]) * y + (i64)s16v(mat[2][0]) * z) >> 15;
            i64 oy = ((i64)s16v(mat[0][1]) * x + (i64)s16v(mat[1][1]) * y + (i64)s16v(mat[2][1]) * z) >> 15;
            i64 oz = ((i64)s16v(mat[0][2]) * x + (i64)s16v(mat[1][2]) * y + (i64)s16v(mat[2][2]) * z) >> 15;
            set_out({u16v(ox), u16v(oy), u16v(oz)});
        } else if (c == 0x0B || c == 0x1B || c == 0x2B) {
            int (*mat)[3] = c == 0x0B ? mA : (c == 0x1B ? mB : mC);
            int x = s16v(I[0]), y = s16v(I[1]), z = s16v(I[2]);
            i64 s = ((i64)s16v(mat[2][0]) * x + (i64)s16v(mat[2][1]) * y + (i64)s16v(mat[2][2]) * z) >> 15;
            set_out({u16v(s)});
        } else if (c == 0x14) set_out({I[0], I[1], I[2]});
        else if (c == 0x0F) set_out({0});
        else if (c == 0x2F) set_out({0x0100});
        else if (c == 0x1F) { outputs.assign(1024, 0); out_count = 1024; out_idx = 0; waiting_cmd = false; }
        else set_out({0});
    }
    bool handles(u32 a) {
        int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
        if (hirom) return b < 0x10 && off >= 0x6000 && off < 0x8000;
        return b >= 0x30 && b < 0x40 && off >= 0x8000;
    }
    int read(u32 a) {
        int off = a & 0xFFFF;
        if (hirom) return (off >= 0x7000 && off < 0x8000) ? read_status() : read_data();
        return off >= 0xC000 ? read_status() : read_data();
    }
    void write(u32 a, int v) {
        int off = a & 0xFFFF;
        if (hirom && off >= 0x7000 && off < 0x8000) return;
        if (!hirom && off >= 0xC000) return;
        write_data(v);
    }
    void ser(Ser& s) {
        s.pod(cmd); s.pod(in_count); s.pod(out_count); s.pod(in_idx); s.pod(out_idx); s.pod(waiting_cmd);
        s.pod(inputs); s.vec(outputs); s.pod(mA); s.pod(mB); s.pod(mC); s.pod(les); s.pod(aas); s.pod(azs);
        s.pod(wlo); s.pod(wbuf); s.pod(rlo); s.pod(rword);
    }
};

// --------------------------------------------------------------------- OBC1
struct OBC1 {
    SNES* snes; u8 ram[0x2000]; int address = 0, baseptr = 0, shift = 0;
    explicit OBC1(SNES* s) : snes(s) { memset(ram, 0, sizeof ram); }
    void power() { address = baseptr = shift = 0; }
    bool handles(u32 a) { int bank = a >> 16, off = a & 0xFFFF; return (bank & 0x7F) < 0x40 && off >= 0x6000 && off < 0x8000; }
    int read(u32 a) {
        int off = a & 0x1FFF;
        if (off >= 0x1FF0) {
            int i = off & 3;
            int ptr = (baseptr + (address << 2) + i) & 0x1FFF;
            if (i == 3) return ((ram[(baseptr + 0x100 + (address >> 2)) & 0x1FFF] >> ((address & 3) << 1)) & 3);
            return ram[ptr];
        }
        return ram[off];
    }
    void write(u32 a, int v) {
        int off = a & 0x1FFF; v &= 0xFF;
        if (off == 0x1FF0) address = (address & 0x100) | v;
        else if (off == 0x1FF1) address = (address & 0x0FF) | ((v & 1) << 8);
        else if (off == 0x1FF2) baseptr = (v & 0x3F) << 4;
        else if (off == 0x1FF3) {}
        else ram[off] = v;
    }
    void ser(Ser& s) { s.pod(ram); s.pod(address); s.pod(baseptr); s.pod(shift); }
};

// --------------------------------------------------------------------- S-RTC
struct SRTC {
    SNES* snes; int mode = 0, index = 0; int data[13] = {0};
    explicit SRTC(SNES* s) : snes(s) {}
    void power() { mode = 0; index = 0; latch_now(); }
    void latch_now() {
        time_t t = time(nullptr); struct tm n = *localtime(&t);
        int year = n.tm_year + 1900, wd = (n.tm_wday + 6) % 7;   // Python weekday(): Monday=0
        int d[13] = {n.tm_sec % 10, n.tm_sec / 10, n.tm_min % 10, n.tm_min / 10, n.tm_hour % 10, n.tm_hour / 10,
                     n.tm_mday % 10, n.tm_mday / 10, (n.tm_mon + 1) % 10, (n.tm_mon + 1) / 10, year % 10, (year / 10) % 10, wd};
        memcpy(data, d, sizeof d);
    }
    bool handles(u32 a) { int bank = a >> 16, off = a & 0xFFFF; return (bank & 0x7F) < 0x40 && (off == 0x2800 || off == 0x2801); }
    int read(u32 a) {
        if ((a & 0xFFFF) == 0x2800) {
            if (index >= 13) return 0x0F;
            int v = data[index < 0 ? 13 + index : index] & 0x0F;
            index++; return v;
        }
        return 0x0F;
    }
    void write(u32 a, int v) {
        if ((a & 0xFFFF) != 0x2801) return;
        v &= 0x0F;
        if (v == 0x0D) { mode = 0; index = -1; return; }
        if (v == 0x0E) { mode = 0; index = 0; latch_now(); return; }
        if (index < 0) { index = 0; return; }
        if (index < 13) { data[index] = v; index++; }
    }
    void ser(Ser& s) { s.pod(mode); s.pod(index); s.pod(data); }
};

// --------------------------------------------------------------------- S-DD1 (Andreas Naive ABS Golomb HLE)
static const int SDD1_EVOL[33][3] = {
    {0, 25, 25}, {0, 2, 1}, {0, 3, 1}, {0, 4, 2}, {0, 5, 3},
    {1, 6, 4}, {1, 7, 5}, {1, 8, 6}, {1, 9, 7},
    {2, 10, 8}, {2, 11, 9}, {2, 12, 10}, {2, 13, 11},
    {3, 14, 12}, {3, 15, 13}, {3, 16, 14}, {3, 17, 15},
    {4, 18, 16}, {4, 19, 17}, {5, 20, 18}, {5, 21, 19},
    {6, 22, 20}, {6, 23, 21}, {7, 24, 22}, {7, 24, 23},
    {0, 26, 1}, {1, 27, 2}, {2, 28, 4}, {3, 29, 8},
    {4, 30, 12}, {5, 31, 16}, {6, 32, 18}, {7, 24, 22}};

static std::vector<u8> sdd1_decompress(const std::vector<u8>& data, int nbytes) {
    std::vector<u8> out(std::max(nbytes, 0), 0);
    if (data.empty() || nbytes <= 0) return out;
    size_t bit_i = 0;
    auto bit = [&]() -> int {
        size_t i = bit_i; bit_i++;
        if ((i >> 3) >= data.size()) return 0;
        return (data[i >> 3] >> (7 - (i & 7))) & 1;
    };
    int h0 = bit(), h1 = bit(), h2 = bit(), h3 = bit();
    int plane_mode = (h0 << 1) | h1, ctx_mode = (h2 << 1) | h3;
    static const std::vector<int> templates[4] = {{1, 7, 8, 9}, {1, 8, 9}, {1, 7, 8}, {1, 2, 8, 9}};
    const std::vector<int>& tmpl = templates[ctx_mode];
    int nctx_bits = (int)tmpl.size();
    int nctx = (1 << nctx_bits) * 2;
    std::vector<int> status(nctx, 0), mps(nctx, 0);
    int bg_cnt[8] = {0}; bool bg_lps[8] = {false};
    auto get_run = [&](int code, int& cnt, bool& lps) {
        if (bit() == 0) { cnt = 1 << code; lps = false; return; }
        int k = 0; for (int i = 0; i < code; i++) k = (k << 1) | bit();
        cnt = k; lps = true;
    };
    auto gen_bit = [&](int code, bool& end) -> int {
        if (bg_cnt[code] == 0 && !bg_lps[code]) get_run(code, bg_cnt[code], bg_lps[code]);
        end = false; int b;
        if (bg_cnt[code] > 0) {
            bg_cnt[code]--; b = 0;
            if (bg_cnt[code] == 0 && !bg_lps[code]) end = true;
        } else { b = 1; bg_lps[code] = false; end = true; }
        return b;
    };
    auto pem_bit = [&](int ctx) -> int {
        int st = status[ctx];
        int code = SDD1_EVOL[st][0], nm = SDD1_EVOL[st][1], nl = SDD1_EVOL[st][2];
        bool end; int b = gen_bit(code, end);
        int o = b ^ mps[ctx];
        if (end) {
            if (b) { if (st <= 1) mps[ctx] ^= 1; status[ctx] = nl; }
            else status[ctx] = nm;
        }
        return o;
    };
    int hist[8] = {0};
    auto want_bit = [&](int p) -> int {
        int h = hist[p], ctx = 0;
        for (int i = 0; i < nctx_bits; i++) ctx |= ((h >> (tmpl[i] - 1)) & 1) << i;
        ctx |= (p & 1) << nctx_bits;
        int b = pem_bit(ctx & (nctx - 1));
        hist[p] = ((h << 1) | b) & 0x1FF;
        return b;
    };
    auto take = [&](int p) -> u8 { int v = 0; for (int i = 0; i < 8; i++) v = (v << 1) | want_bit(p); return (u8)v; };
    int i = 0;
    if (plane_mode == 0) {
        while (i < nbytes) { out[i++] = take(0); if (i >= nbytes) break; out[i++] = take(1); }
    } else if (plane_mode == 2) {
        while (i < nbytes) {
            for (int k = 0; k < 8; k++) { if (i >= nbytes) break; out[i++] = take(0); if (i >= nbytes) break; out[i++] = take(1); }
            for (int k = 0; k < 8; k++) { if (i >= nbytes) break; out[i++] = take(2); if (i >= nbytes) break; out[i++] = take(3); }
        }
    } else if (plane_mode == 1) {
        while (i < nbytes) {
            for (int pair = 0; pair < 4; pair++) {
                int p0 = pair * 2, p1 = pair * 2 + 1;
                for (int k = 0; k < 8; k++) { if (i >= nbytes) break; out[i++] = take(p0); if (i >= nbytes) break; out[i++] = take(p1); }
            }
        }
    } else {
        while (i < nbytes) {
            int chunk = std::min(8, nbytes - i);
            int bits[8][8];
            for (int b = 0; b < 8; b++) for (int n = 0; n < chunk; n++) bits[b][n] = want_bit(b);
            for (int n = 0; n < chunk; n++) { int v = 0; for (int b = 0; b < 8; b++) v |= bits[b][n] << b; out[i + n] = (u8)v; }
            i += chunk;
        }
    }
    return out;
}

struct SDD1 {
    SNES* snes; int dma_enable[8] = {0}; int bank[4] = {0};
    explicit SDD1(SNES* s) : snes(s) {}
    void power() { for (auto& d : dma_enable) d = 0; for (auto& b : bank) b = 0; }
    bool handles_mmio(u32 a) { int bk = a >> 16, off = a & 0xFFFF; return (bk & 0x7F) < 0x40 && off >= 0x4800 && off <= 0x4807; }
    int read_mmio(u32) { return 0; }
    void write_mmio(u32 a, int v) {
        int off = a & 0xFFFF; v &= 0xFF;
        if (off == 0x4800) { for (int i = 0; i < 8; i++) dma_enable[i] = (v >> i) & 1; }
        else if (off >= 0x4801 && off <= 0x4804) bank[off - 0x4801] = v & 0x0F;
    }
    size_t map_rom(int bk, int addr);
    std::vector<u8> dma_read(int bk, int addr, int size);
    void ser(Ser& s) { s.pod(dma_enable); s.pod(bank); }
};

// --------------------------------------------------------------------- CX4 (Capcom) HLE MMIO + IRAM
struct CX4 {
    SNES* snes; u8 ram[0xC00]; int dma = 0, page = 0; bool busy = false;
    explicit CX4(SNES* s) : snes(s) { memset(ram, 0, sizeof ram); }
    void power() { memset(ram, 0, sizeof ram); dma = page = 0; busy = false; }
    bool handles(u32 a) { int bank = a >> 16, off = a & 0xFFFF; return (bank & 0x7F) < 0x40 && off >= 0x6000 && off < 0x8000; }
    void w16(int addr, int v) { ram[addr & 0xBFF] = v & 0xFF; ram[(addr + 1) & 0xBFF] = (v >> 8) & 0xFF; }
    int r16(int addr) { return ram[addr & 0xBFF] | (ram[(addr + 1) & 0xBFF] << 8); }
    int read(u32 a) {
        int off = a & 0x1FFF;
        if (off == 0x1F50) return busy ? 0x00 : 0x40;
        if (off < 0xC00) return ram[off];
        return 0;
    }
    void write(u32 a, int v) {
        int off = a & 0x1FFF; v &= 0xFF;
        if (off < 0xC00) ram[off] = v;
        if (off == 0x1F4F) run(v);
        else if (off == 0x1F47) page = v;
    }
    void run(int cmd) {
        if (cmd == 0x01) {
            int a = s16v(r16(0x1F40)), b = s16v(r16(0x1F42)); i64 r = (i64)a * b;
            w16(0x1F40, (int)(r & 0xFFFF)); w16(0x1F42, (int)((r >> 16) & 0xFFFF));
        } else if (cmd == 0x0D) { int a = s16v(r16(0x1F40)); w16(0x1F40, (a * a) & 0xFFFF); }
        else if (cmd == 0x13) { int a = r16(0x1F40); w16(0x1F40, (int)std::sqrt((double)a) & 0xFFFF); }
    }
    void ser(Ser& s) { s.pod(ram); s.pod(dma); s.pod(page); s.pod(busy); }
};

// --------------------------------------------------------------------- SuperFX / GSU
struct SuperFX {
    SNES* snes;
    int r[16] = {0}; int sfr = 0, pbr = 0, rombr = 0, rambr = 0, cbr = 0, scbr = 0, scmr = 0, colr = 0, por = 0, vcr = 0x04, cfgr = 0, clsr = 0;
    u8 cache[512]; u8 cache_valid[32];
    bool running = false; int src = 0, dst = 0; bool alt1 = false, alt2 = false, b_flag = false;
    explicit SuperFX(SNES* s) : snes(s) { memset(cache, 0, sizeof cache); memset(cache_valid, 0, sizeof cache_valid); }
    void power() {
        for (auto& x : r) x = 0;
        sfr = pbr = rombr = rambr = cbr = scbr = scmr = colr = por = cfgr = clsr = 0;
        memset(cache, 0, sizeof cache); memset(cache_valid, 0, sizeof cache_valid);
        running = false; src = dst = 0; alt1 = alt2 = false; b_flag = false;
    }
    bool handles(u32 a) { int bank = a >> 16, off = a & 0xFFFF; return (bank & 0x7F) < 0x40 && off >= 0x3000 && off < 0x3300; }
    void flag_z(int v) { sfr = ((v & 0xFFFF) == 0) ? (sfr | 0x02) : (sfr & ~0x02); }
    void flag_s(int v) { sfr = (v & 0x8000) ? (sfr | 0x08) : (sfr & ~0x08); }
    void flag_cy(bool c) { sfr = c ? (sfr | 0x04) : (sfr & ~0x04); }
    void flag_ov(bool o) { sfr = o ? (sfr | 0x10) : (sfr & ~0x10); }
    int zs(i64 v) { int x = (int)(v & 0xFFFF); flag_z(x); flag_s(x); return x; }
    int read_reg(u32 a) {
        int off = a & 0xFFFF;
        if (off >= 0x3000 && off <= 0x301F) { int i = (off - 0x3000) >> 1; return (r[i] >> ((off & 1) * 8)) & 0xFF; }
        if (off == 0x3030) return sfr & 0xFF;
        if (off == 0x3031) return (sfr >> 8) & 0xFF;
        if (off == 0x3034) return pbr & 0x7F;
        if (off == 0x3036) return rombr;
        if (off == 0x303B) return vcr;
        if (off == 0x303C) return rambr & 1;
        if (off == 0x303E) return cbr & 0xFF;
        if (off == 0x303F) return (cbr >> 8) & 0xFF;
        return 0;
    }
    void write_reg(u32 a, int v) {
        int off = a & 0xFFFF; v &= 0xFF;
        if (off >= 0x3000 && off <= 0x301F) {
            int i = (off - 0x3000) >> 1;
            if (off & 1) r[i] = (r[i] & 0x00FF) | (v << 8); else r[i] = (r[i] & 0xFF00) | v;
            if (i == 15 && (off & 1) == 1) start();
            return;
        }
        if (off == 0x3030) {
            if (v & 0x20) start();
            else { running = false; sfr = (sfr & 0xFF00) | (v & 0xDF); sfr &= ~0x20; return; }
            sfr = (sfr & 0xFF00) | (v & 0xFF);
        } else if (off == 0x3031) sfr = (sfr & 0x00FF) | ((v & 0x7F) << 8);
        else if (off == 0x3034) pbr = v & 0x7F;
        else if (off == 0x3037) cfgr = v;
        else if (off == 0x3038) scbr = v;
        else if (off == 0x3039) clsr = v;
        else if (off == 0x303A) scmr = v;
    }
    void start() { running = true; sfr = (sfr | 0x20) & ~0x8000; }
    int rom_read(int bank, int addr);
    int ram_read(int addr);
    void ram_write(int addr, int v);
    int fetch() {
        int pc = r[15];
        int caddr = (pc - cbr) & 0xFFFF;
        int op;
        if (caddr < 512) {
            int block = caddr >> 4;
            if (!cache_valid[block]) {
                int base = (cbr + (block << 4)) & 0xFFFF;
                for (int i = 0; i < 16; i++) cache[(block << 4) + i] = (u8)rom_read(pbr, (base + i) & 0xFFFF);
                cache_valid[block] = 1;
            }
            op = cache[caddr];
        } else op = rom_read(pbr, pc);
        r[15] = (pc + 1) & 0xFFFF;
        return op;
    }
    void plot();
    void rpix() { r[dst] = zs(0); }
    void step(int max_ops) { if (!running) return; for (int i = 0; i < max_ops; i++) { if (!running) break; exec_one(); } }
    void run_to(int budget) { step(budget); }
    void branch(bool take) { int off = fetch(); if (off & 0x80) off -= 256; if (take) r[15] = (r[15] + off) & 0xFFFF; }
    void exec_one() {
        int op = fetch();
        bool a1 = alt1, a2 = alt2;
        alt1 = alt2 = false;
        int S = src, D = dst;
        bool z = sfr & 0x02, cy = sfr & 0x04, sflg = sfr & 0x08, ov = sfr & 0x10;
        if (op == 0x00) { running = false; sfr = (sfr & ~0x20) | 0x8000; return; }
        if (op == 0x01) return;
        if (op == 0x02) { cbr = r[15] & 0xFFF0; memset(cache_valid, 0, sizeof cache_valid); return; }
        if (op == 0x03) { int v = r[S]; flag_cy(v & 1); r[D] = zs(v >> 1); return; }
        if (op == 0x04) { int v = r[S]; int cin = cy ? 1 : 0; flag_cy((v >> 15) & 1); r[D] = zs(((v << 1) | cin) & 0xFFFF); return; }
        if (op == 0x05) { branch(true); return; }
        if (op == 0x06) { branch(!(sflg ^ ov)); return; }
        if (op == 0x07) { branch(sflg ^ ov); return; }
        if (op == 0x08) { branch(!z); return; }
        if (op == 0x09) { branch(z); return; }
        if (op == 0x0A) { branch(!sflg); return; }
        if (op == 0x0B) { branch(sflg); return; }
        if (op == 0x0C) { branch(!cy); return; }
        if (op == 0x0D) { branch(cy); return; }
        if (op == 0x0E) { branch(!ov); return; }
        if (op == 0x0F) { branch(ov); return; }
        if (op >= 0x10 && op <= 0x1F) {
            int n = op & 0x0F;
            if (b_flag) { r[n] = r[S]; b_flag = false; sfr &= ~0x1000; }
            else dst = n;
            return;
        }
        if (op >= 0x20 && op <= 0x2F) { int n = op & 0x0F; src = dst = n; b_flag = true; sfr |= 0x1000; return; }
        if (op >= 0x30 && op <= 0x3B) {
            int addr = r[op & 0x0F];
            if (a1) ram_write(addr, r[S] & 0xFF);
            else { ram_write(addr, r[S] & 0xFF); ram_write((addr + 1) & 0xFFFF, (r[S] >> 8) & 0xFF); }
            return;
        }
        if (op == 0x3C) { r[12] = (r[12] - 1) & 0xFFFF; flag_z(r[12]); if (r[12] != 0) r[15] = r[13]; return; }
        if (op == 0x3D) { alt1 = true; return; }
        if (op == 0x3E) { alt2 = true; return; }
        if (op == 0x3F) { alt1 = alt2 = true; return; }
        if (op >= 0x40 && op <= 0x4B) {
            int addr = r[op & 0x0F];
            if (a1) r[D] = zs(ram_read(addr));
            else { int lo = ram_read(addr); int hi = ram_read((addr + 1) & 0xFFFF); r[D] = zs(lo | (hi << 8)); }
            return;
        }
        if (op == 0x4C) { if (a1) rpix(); else plot(); return; }
        if (op == 0x4D) { int v = r[S]; r[D] = zs(((v >> 8) | (v << 8)) & 0xFFFF); return; }
        if (op == 0x4E) { if (a1) por = r[S] & 0xFF; else colr = r[S] & 0xFF; return; }
        if (op == 0x4F) { r[D] = zs((~r[S]) & 0xFFFF); return; }
        if (op >= 0x50 && op <= 0x5F) {
            int n = op & 0x0F; int a = r[S]; int b = a2 ? n : r[n];
            int cin = a1 ? (cy ? 1 : 0) : 0;
            int rr = a + b + cin;
            flag_cy(rr > 0xFFFF);
            flag_ov(((a ^ rr) & (b ^ rr) & 0x8000) != 0);
            r[D] = zs(rr); return;
        }
        if (op >= 0x60 && op <= 0x6F) {
            int n = op & 0x0F; int a = r[S]; int b = a2 ? n : r[n];
            int rr = a - b;
            flag_cy(a >= b);
            flag_ov(((a ^ b) & (a ^ rr) & 0x8000) != 0);
            int x = rr & 0xFFFF; zs(x);
            if (!a1) r[D] = x;
            return;
        }
        if (op >= 0x70 && op <= 0x7F) {
            int n = op & 0x0F;
            if (op == 0x70 && !a1 && !a2) { r[D] = zs((r[7] & 0xFF00) | ((r[8] >> 8) & 0xFF)); return; }
            int b = a2 ? n : r[n];
            int rr = a1 ? (r[S] & (~b)) : (r[S] & b);
            r[D] = zs(rr); return;
        }
        if (op >= 0x80 && op <= 0x8F) {
            int n = op & 0x0F; int b = a2 ? n : r[n];
            i64 rr = a1 ? (i64)((r[S] & 0xFF) * (b & 0xFF)) : (((i64)s16v(r[S]) * s16v(b)) & 0xFFFF);
            r[D] = zs(rr & 0xFFFF); return;
        }
        if (op == 0x90) { ram_write(r[0], r[S] & 0xFF); ram_write((r[0] + 1) & 0xFFFF, (r[S] >> 8) & 0xFF); return; }
        if (op >= 0x91 && op <= 0x94) { r[11] = (r[15] + (op - 0x90)) & 0xFFFF; return; }
        if (op == 0x95) { int v = r[S] & 0xFF; r[D] = zs(v | ((v & 0x80) ? 0xFF00 : 0)); return; }
        if (op == 0x96) {
            int v = s16v(r[S]); int rr;
            if (a1) rr = v != -1 ? (v >> 1) : 0; else rr = v >> 1;
            flag_cy(v & 1); r[D] = zs(rr & 0xFFFF); return;
        }
        if (op == 0x97) { int v = r[S]; int cin = cy ? 1 : 0; flag_cy(v & 1); r[D] = zs(((v >> 1) | (cin << 15)) & 0xFFFF); return; }
        if (op >= 0x98 && op <= 0x9D) {
            int n = op & 0x0F;
            if (a1) { pbr = r[n] & 0x7F; cbr = r[15] & 0xFFF0; memset(cache_valid, 0, sizeof cache_valid); }
            else r[15] = r[n];
            return;
        }
        if (op == 0x9E) { r[D] = zs(r[S] & 0xFF); return; }
        if (op == 0x9F) {
            i64 a = s16v(r[S]), b = s16v(r[6]); i64 prod = a * b;
            if (a1) { r[4] = (int)(prod & 0xFFFF); r[D] = zs((prod >> 16) & 0xFFFF); }
            else { flag_cy((prod >> 15) & 1); r[D] = zs((prod >> 16) & 0xFFFF); }
            return;
        }
        if (op >= 0xA0 && op <= 0xAF) { int n = op & 0x0F; int imm = fetch(); r[n] = imm | ((imm & 0x80) ? 0xFF00 : 0); return; }
        if (op >= 0xB0 && op <= 0xBF) {
            int n = op & 0x0F;
            if (b_flag) { r[D] = zs(r[n]); b_flag = false; sfr &= ~0x1000; }
            else src = n;
            return;
        }
        if (op == 0xC0) { r[D] = zs((r[S] >> 8) & 0xFF); return; }
        if (op >= 0xC1 && op <= 0xCF) {
            int n = op & 0x0F; int b = a2 ? n : r[n];
            r[D] = zs(a1 ? (r[S] ^ b) : (r[S] | b)); return;
        }
        if (op >= 0xD0 && op <= 0xDE) { int n = op & 0x0F; r[n] = zs((r[n] + 1) & 0xFFFF); return; }
        if (op == 0xDF) {
            if (a1 && a2) rambr = r[S] & 1;
            else if (a2) rambr = r[S] & 1;
            else if (a1) rombr = r[S] & 0x7F;
            else colr = rom_read(rombr, r[14]) & 0xFF;
            return;
        }
        if (op >= 0xE0 && op <= 0xEE) { int n = op & 0x0F; r[n] = zs((r[n] - 1) & 0xFFFF); return; }
        if (op == 0xEF) {
            int b = rom_read(rombr, r[14]) & 0xFF;
            if (a1 && a2) r[D] = zs(b | ((b & 0x80) ? 0xFF00 : 0));
            else if (a2) r[D] = zs((r[S] & 0xFF00) | b);
            else if (a1) r[D] = zs((r[S] & 0x00FF) | (b << 8));
            else r[D] = zs(b);
            return;
        }
        // $F0-$FF IWT / LM / SM
        int n = op & 0x0F;
        int lo = fetch(); int hi = fetch(); int imm = lo | (hi << 8);
        if (a1 && !a2) { int l = ram_read(imm); int h = ram_read((imm + 1) & 0xFFFF); r[n] = l | (h << 8); }
        else if (a2 || (a1 && a2)) { ram_write(imm, r[n] & 0xFF); ram_write((imm + 1) & 0xFFFF, (r[n] >> 8) & 0xFF); }
        else { r[n] = imm; if (n == 15) start(); }
    }
    void ser(Ser& s) {
        s.pod(r); s.pod(sfr); s.pod(pbr); s.pod(rombr); s.pod(rambr); s.pod(cbr); s.pod(scbr); s.pod(scmr); s.pod(colr);
        s.pod(por); s.pod(cfgr); s.pod(clsr); s.pod(running); s.pod(src); s.pod(dst); s.pod(b_flag); s.pod(alt1); s.pod(alt2);
        s.pod(cache); s.pod(cache_valid);
    }
};

// --------------------------------------------------------------------- ST010 / ST011 (Seta) HLE
struct ST01x {
    SNES* snes; bool st011; u8 ram[0x1000]; int dr = 0, sr = 0x80, cmd = 0;
    ST01x(SNES* s, bool st11) : snes(s), st011(st11) { memset(ram, 0, sizeof ram); }
    void power() { memset(ram, 0, sizeof ram); dr = 0; sr = 0x80; cmd = 0; }
    bool handles(u32 a) {
        int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
        if (b == 0x60 && off < 0x1000) return true;
        if (b >= 0x68 && b <= 0x6F && off < 0x1000) return true;
        return false;
    }
    void w16(int addr, int v) { addr &= 0xFFE; ram[addr] = v & 0xFF; ram[addr + 1] = (v >> 8) & 0xFF; }
    int r16(int addr) { addr &= 0xFFE; return ram[addr] | (ram[addr + 1] << 8); }
    int rs16(int addr) { return s16v(r16(addr)); }
    int read(u32 a) {
        int off = a & 0xFFFF, bank = (a >> 16) & 0x7F;
        if (bank == 0x60 && off < 0x1000) return (off & 1) ? sr : (dr & 0xFF);
        return ram[off & 0xFFF];
    }
    void write(u32 a, int v) {
        int off = a & 0xFFFF, bank = (a >> 16) & 0x7F; v &= 0xFF;
        if (bank == 0x60 && off < 0x1000) {
            if (off & 1) return;
            dr = v;
            if (v & 0x80) { cmd = v & 0x7F; exec(); }
            return;
        }
        ram[off & 0xFFF] = v;
    }
    void exec() {
        sr = 0x80; int c = cmd;
        if (st011) return;
        if (c == 0x01) { int x = rs16(0), y = rs16(2); w16(4, ((int)(std::atan2((double)y, (double)x) / (2 * CL_PI) * 65536)) & 0xFFFF); }
        else if (c == 0x02) { i64 x = rs16(0), y = rs16(2); w16(4, (int)std::sqrt((double)(x * x + y * y)) & 0xFFFF); }
        else if (c == 0x04) { int a = rs16(0), b = rs16(2); w16(4, u16v(((i64)a * b) >> 15)); }
        else if (c == 0x05) { int a = rs16(0), b = rs16(2); w16(4, u16v(((i64)a * b) >> 8)); }
        else if (c == 0x06) {
            int x = rs16(0), y = rs16(2), a = r16(4);
            double ang = a * (2 * CL_PI / 65536.0), ca = cos(ang), sa = sin(ang);
            w16(6, u16v((i64)(x * ca - y * sa))); w16(8, u16v((i64)(x * sa + y * ca)));
        } else if (c == 0x07) { int n = r16(0); if (!n) n = 1; w16(2, u16v(0x8000 / n)); }
        else if (c == 0x08) {
            int cur = rs16(0), tgt = rs16(2), step = rs16(4); if (!step) step = 1;
            int d = tgt - cur; if (d > step) d = step; if (d < -step) d = -step;
            w16(6, u16v(cur + d));
        }
    }
    void ser(Ser& s) { s.pod(ram); s.pod(dr); s.pod(sr); s.pod(cmd); }
};

// --------------------------------------------------------------------- SPC7110 (Epson)
struct SPC7110 {
    SNES* snes; u8 regs[0x50]; int rtc[16] = {0}; int rtc_mode = 0, rtc_idx = 0;
    std::vector<u8> decomp; int decomp_off = 0; bool busy = false, dx_enabled = false;
    size_t prog_size = 0; std::vector<u8> data;
    explicit SPC7110(SNES* s);
    void power() {
        memset(regs, 0, sizeof regs); regs[0x31] = 0; regs[0x32] = 1; regs[0x33] = 2;
        rtc_mode = 0; rtc_idx = 0; latch_rtc();
        decomp.assign(0x10000, 0); decomp_off = 0; busy = false; dx_enabled = false;
    }
    void latch_rtc() {
        time_t t = time(nullptr); struct tm n = *localtime(&t);
        int year = n.tm_year + 1900, wd = (n.tm_wday + 6) % 7;
        int d[16] = {n.tm_sec % 10, n.tm_sec / 10, n.tm_min % 10, n.tm_min / 10, n.tm_hour % 10, n.tm_hour / 10,
                     n.tm_mday % 10, n.tm_mday / 10, (n.tm_mon + 1) % 10, (n.tm_mon + 1) / 10, year % 10, (year / 10) % 10,
                     wd, 0x01, 0x0F, 0x06};
        memcpy(rtc, d, sizeof d);
    }
    bool handles(u32 a) {
        int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
        if (b < 0x40 && off >= 0x4800 && off <= 0x4842) return true;
        return bank == 0x50;
    }
    int data_ptr() { return (regs[0x11] | (regs[0x12] << 8) | (regs[0x13] << 16)) & 0xFFFFFF; }
    void set_data_ptr(i64 p) { p &= 0xFFFFFF; regs[0x11] = p & 0xFF; regs[0x12] = (p >> 8) & 0xFF; regs[0x13] = (p >> 16) & 0xFF; }
    int rd_data(i64 p) { if (data.empty()) return 0; return data[(size_t)floormod(p, (i64)data.size())]; }
    void do_muldiv() {
        u8* r = regs;
        u64 a = r[0x20] | (r[0x21] << 8), b = r[0x24] | (r[0x25] << 8);
        u64 prod = (a * b) & 0xFFFFFFFF;
        for (int i = 0; i < 4; i++) r[0x28 + i] = (prod >> (8 * i)) & 0xFF;
        u64 dividend = (u64)r[0x20] | ((u64)r[0x21] << 8) | ((u64)r[0x22] << 16) | ((u64)r[0x23] << 24);
        u64 divisor = r[0x26] | (r[0x27] << 8), q, rem;
        if (divisor == 0) { q = 0; rem = dividend & 0xFFFF; }
        else { q = (dividend / divisor) & 0xFFFFFFFF; rem = (dividend % divisor) & 0xFFFF; }
        for (int i = 0; i < 4; i++) r[0x28 + i] = (q >> (8 * i)) & 0xFF;
        r[0x2C] = rem & 0xFF; r[0x2D] = (rem >> 8) & 0xFF;
        r[0x2F] = 0;
    }
    void begin_decomp();
    int read(u32 a) {
        int bank = a >> 16, off = a & 0xFFFF;
        if (bank == 0x50) return decomp[off];
        u8* r = regs; int reg = off - 0x4800;
        if (reg == 0x00) {
            int v = decomp[decomp_off & 0xFFFF];
            decomp_off = (decomp_off + 1) & 0xFFFF;
            int ln = r[0x09] | (r[0x0A] << 8);
            if (ln) { ln--; r[0x09] = ln & 0xFF; r[0x0A] = (ln >> 8) & 0xFF; }
            return v;
        }
        if (reg == 0x10) {
            int p = data_ptr(); int v = rd_data(p);
            int mode = r[0x18]; int inc = 1;
            if (mode & 1) { inc = r[0x16] | (r[0x17] << 8); if ((mode & 4) && (inc & 0x8000)) inc -= 0x10000; }
            if (!(mode & 0x10)) set_data_ptr((i64)p + inc);
            else { int adj = r[0x14] | (r[0x15] << 8); adj = (adj + inc) & 0xFFFF; r[0x14] = adj & 0xFF; r[0x15] = adj >> 8; }
            return v;
        }
        if (reg == 0x1A) {
            int p = data_ptr(); int adj = r[0x14] | (r[0x15] << 8);
            if ((r[0x18] & 8) && (adj & 0x8000)) adj -= 0x10000;
            return rd_data(((i64)p + adj) & 0xFFFFFF);
        }
        if (reg == 0x0C) { int v = r[0x0C]; r[0x0C] = 0; return v; }
        if (reg == 0x2F) return r[0x2F];
        if (reg == 0x41) { if (rtc_idx < 16) return rtc[rtc_idx++] & 0x0F; return 0x0F; }
        if (reg == 0x42) return 0x80;
        if (reg >= 0 && reg < 0x50) return r[reg];
        return 0;
    }
    void write(u32 a, int v) {
        int off = a & 0xFFFF; v &= 0xFF;
        if ((a >> 16) == 0x50) { decomp[off] = v; return; }
        int reg = off - 0x4800; u8* r = regs;
        if (reg == 0x06) { r[reg] = v; busy = true; begin_decomp(); return; }
        if (reg == 0x25 || reg == 0x27) { r[reg] = v; r[0x2F] = 0x80; do_muldiv(); return; }
        if (reg == 0x2E) { for (int i = 0x20; i < 0x2E; i++) r[i] = 0; return; }
        if (reg == 0x13 && v != 0) dx_enabled = true;
        if (reg == 0x40) { r[reg] = v; if (v & 1) { latch_rtc(); rtc_mode = 0; rtc_idx = 0; } return; }
        if (reg == 0x41) {
            if (rtc_mode == 0) { rtc_mode = v; rtc_idx = 0; }
            else if (rtc_idx < 16) { rtc[rtc_idx++] = v & 0x0F; }
            return;
        }
        if (reg >= 0 && reg < 0x50) r[reg] = v;
    }
    bool map_bank(int bank, size_t& out) {
        int b = bank & 0xFF, sel;
        if (b >= 0xD0 && b <= 0xDF) sel = regs[0x31] & 3;
        else if (b >= 0xE0 && b <= 0xEF) sel = regs[0x32] & 3;
        else if (b >= 0xF0 && b <= 0xFF) sel = regs[0x33] & 3;
        else return false;
        out = (size_t)sel * 0x100000 + ((b & 0x0F) << 16);
        return true;
    }
    void ser(Ser& s) { s.pod(regs); s.pod(rtc); s.pod(rtc_mode); s.pod(rtc_idx); s.vec(decomp); s.pod(decomp_off); s.pod(busy); s.pod(dx_enabled); }
};

// --------------------------------------------------------------------- SGB / ICD2 bridge
struct SGB {
    SNES* snes; int row = 0, joy = 0, r6000 = 0, r6002 = 0, r6003 = 0x01, r6004 = 0; u8 packet[16]; int pk_i = 0; u8 vram[0x2000];
    explicit SGB(SNES* s) : snes(s) { memset(packet, 0, 16); memset(vram, 0, sizeof vram); }
    void power() { row = 0; joy = 0; r6000 = 0; r6002 = 0; r6003 = 0x01; r6004 = 0; memset(packet, 0, 16); pk_i = 0; memset(vram, 0, sizeof vram); }
    bool handles(u32 a) { int bank = a >> 16, off = a & 0xFFFF; return (bank & 0x7F) < 0x40 && off >= 0x6000 && off <= 0x6007; }
    int read(u32 a) {
        switch (a & 0xFFFF) {
        case 0x6000: return 0x01; case 0x6002: return r6002; case 0x6003: return r6003; case 0x6004: return r6004;
        case 0x6005: return 0; case 0x6006: return 0x01; case 0x6007: return 0;
        }
        return 0;
    }
    void write(u32 a, int v) {
        int off = a & 0xFFFF; v &= 0xFF;
        if (off == 0x6001) {
            if (pk_i < 16) packet[pk_i++] = v;
            if (pk_i >= 16) { do_packet(); pk_i = 0; }
        } else if (off == 0x6002) r6002 = v;
        else if (off == 0x6003) r6003 = v;
        else if (off == 0x6004) { r6004 = v; row = 0; }
    }
    void do_packet() {
        int cmd = packet[0] >> 3;
        if (in_set(cmd, {0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x11, 0x14, 0x15, 0x1A})) r6000 = 0x01;
    }
    void ser(Ser& s) { s.pod(row); s.pod(joy); s.pod(r6000); s.pod(r6002); s.pod(r6003); s.pod(r6004); s.pod(packet); s.pod(pk_i); s.pod(vram); }
};

// ===================================================================== SA-1 (Nintendo Super Accelerator)
// Custom implementation from public SA-1 hardware docs (SnesLab / fullsnes).
struct SA1 : Bus {
    SNES* snes;
    int mdr = 0;
    u8 iram[0x800];
    std::vector<u8>* bwram = nullptr;
    CPU65816 cpu;
    int dma_line = 0; bool cc1_active = false;
    bool sa1_irq, sa1_rdyb, sa1_nmi, sa1_resb; int smeg;
    bool cpu_irqen, chdma_irqen, cpu_irqcl, chdma_irqcl;
    int crv, cnv, civ; bool cpu_irq, cpu_ivsw, cpu_nvsw; int cmeg;
    bool sa1_irqen, timer_irqen, dma_irqen, sa1_nmien, sa1_irqcl, timer_irqcl, dma_irqcl, sa1_nmicl;
    int snv, siv; bool hvselb, ven, hen; int hcnt, vcnt, hcounter, vcounter;
    int cbmode, dbmode, ebmode, fbmode, cb, db, eb, fb, sbm; bool sw46; int cbm; bool swen, cwen; int bwp, siwp, ciwp;
    bool dmaen, dprio, cden, cdsel; int dd, sd; bool chdend; int dmasize, dmacb; int dsa, dda, dtc, bbf; u8 brf[16];
    int acm, md, ma, mb; i64 mr; bool overflow, hl; int vb, va, vbit, vdp;
    bool cpu_irqfl, chdma_irqfl, sa1_irqfl, timer_irqfl, dma_irqfl, sa1_nmifl;

    explicit SA1(SNES* s) : snes(s), cpu(this) { memset(iram, 0, sizeof iram); power_mmio(); }
    void attach_bwram(std::vector<u8>* b) { bwram = b; }
    void power_mmio() {
        sa1_irq = sa1_rdyb = sa1_nmi = false; sa1_resb = true; smeg = 0;
        cpu_irqen = chdma_irqen = false; cpu_irqcl = chdma_irqcl = true;
        crv = cnv = civ = 0; cpu_irq = cpu_ivsw = cpu_nvsw = false; cmeg = 0;
        sa1_irqen = timer_irqen = dma_irqen = sa1_nmien = false;
        sa1_irqcl = timer_irqcl = dma_irqcl = sa1_nmicl = true;
        snv = siv = 0; hvselb = ven = hen = false; hcnt = vcnt = 0; hcounter = vcounter = 0;
        cbmode = dbmode = ebmode = fbmode = 0; cb = 0; db = 1; eb = 2; fb = 3; sbm = 0;
        sw46 = false; cbm = 0; swen = cwen = false; bwp = 0x0F; siwp = ciwp = 0;
        dmaen = dprio = cden = cdsel = false; dd = sd = 0; chdend = false; dmasize = dmacb = 0;
        dsa = dda = 0; dtc = 0; bbf = 0; memset(brf, 0, 16);
        acm = md = 0; ma = mb = 0; mr = 0; overflow = false; hl = false; vb = 16; va = 0; vbit = 0; vdp = 0;
        cpu_irqfl = chdma_irqfl = false; sa1_irqfl = timer_irqfl = dma_irqfl = sa1_nmifl = false;
        dma_line = 0; cc1_active = false;
    }
    void power() {
        memset(iram, 0, sizeof iram);
        power_mmio();
        CPU65816& c = cpu;
        c.cyc = 0; c.A = c.X = c.Y = 0; c.ik_valid = false;
        c.E = 1; c.P = 0x34; c.D = 0; c.DB = 0; c.PB = 0; c.S = 0x01FF;
        c.PC = 0; c.waiting = c.stopped = false; c.nmi_pending = c.irq_line = false;
    }
    int rom_byte(size_t off);
    // returns kind: 0=None 1=offset 2=vector byte
    int rom_linear(u32 addr, u32& out) {
        if ((addr & 0xFFFFE0) == 0x00FFE0 && ((addr & 0xFF0000) == 0 || (addr & 0xFF0000) == 0x800000)) {
            int lo = addr & 0xFFFF;
            if (lo == 0xFFEA && cpu_nvsw) { out = snv & 0xFF; return 2; }
            if (lo == 0xFFEB && cpu_nvsw) { out = (snv >> 8) & 0xFF; return 2; }
            if (lo == 0xFFEE && cpu_ivsw) { out = siv & 0xFF; return 2; }
            if (lo == 0xFFEF && cpu_ivsw) { out = (siv >> 8) & 0xFF; return 2; }
        }
        u32 a;
        if ((addr & 0x408000) == 0x008000) a = ((addr & 0x800000) >> 2) | ((addr & 0x3F0000) >> 1) | (addr & 0x7FFF);
        else if ((addr & 0xC00000) == 0xC00000) a = addr;
        else return 0;
        bool lo = a < 0x400000;
        a &= 0x3FFFFF;
        int mode, img;
        if (a < 0x100000) { mode = cbmode; img = cb; }
        else if (a < 0x200000) { mode = dbmode; img = db; }
        else if (a < 0x300000) { mode = ebmode; img = eb; }
        else { mode = fbmode; img = fb; }
        if (lo && !mode) { out = a; return 1; }
        out = ((img & 7) << 20) | (a & 0x0FFFFF); return 1;
    }
    int read_rom(u32 addr) {
        u32 o; int k = rom_linear(addr, o);
        if (k == 2) return o;
        if (k == 0) return mdr;
        return rom_byte(o);
    }
    int bw_mask() { size_t n = bwram ? bwram->size() : 0; return n ? (int)n - 1 : 0; }
    int bw_read(i64 off) { size_t n = bwram ? bwram->size() : 0; if (!n) return mdr; return (*bwram)[(size_t)floormod(off, (i64)n)]; }
    void bw_write(i64 off, int v) {
        size_t n = bwram ? bwram->size() : 0; if (!n) return;
        if (!swen && !cwen && (off & 0x3FFFF) < (0x100 << bwp)) return;
        (*bwram)[(size_t)floormod(off, (i64)n)] = v & 0xFF;
    }
    int bw_read_bitmap(i64 addr) {
        if (!bbf) { int sh = addr & 1; i64 a = addr >> 1; return (bw_read(a) >> (sh == 0 ? 0 : 4)) & 15; }
        int sh = addr & 3; i64 a = addr >> 2; return (bw_read(a) >> (sh * 2)) & 3;
    }
    void bw_write_bitmap(i64 addr, int data) {
        data &= 0xFF;
        if (!bbf) {
            int sh = addr & 1; i64 a = addr >> 1; int cur = bw_read(a);
            if (sh == 0) bw_write(a, (cur & 0xF0) | (data & 0x0F)); else bw_write(a, (cur & 0x0F) | ((data & 0x0F) << 4));
        } else {
            int sh = addr & 3; i64 a = addr >> 2; int cur = bw_read(a);
            int mask = ~(3 << (sh * 2)) & 0xFF;
            bw_write(a, (cur & mask) | ((data & 3) << (sh * 2)));
        }
    }
    int iram_read(int a) { return iram[a & 0x7FF]; }
    void iram_write(int a, int v, bool from_sa1);
    u8 read(u32 a) override {
        a &= 0xFFFFFF;
        int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
        if (b < 0x40 && off >= 0x2200 && off < 0x2400) { mdr = read_io_sa1(off); return (u8)mdr; }
        if ((a & 0x408000) == 0x008000 || (a & 0xC00000) == 0xC00000) {
            if (b == 0 && off >= 0xFFE0) {
                if (off == 0xFFEA || off == 0xFFEB || off == 0xFFFA || off == 0xFFFB) return (cnv >> ((off & 1) ? 8 : 0)) & 0xFF;
                if (off == 0xFFEE || off == 0xFFEF || off == 0xFFFE || off == 0xFFFF) return (civ >> ((off & 1) ? 8 : 0)) & 0xFF;
            }
            mdr = read_rom(a); return (u8)mdr;
        }
        if (b < 0x40 && off >= 0x6000 && off < 0x8000) {
            if (sw46) mdr = bw_read_bitmap((i64)cbm * 0x2000 + (off & 0x1FFF));
            else mdr = bw_read((i64)(cbm & 0x1F) * 0x2000 + (off & 0x1FFF));
            return (u8)mdr;
        }
        if (bank >= 0x40 && bank <= 0x5F) { mdr = bw_read(((i64)(bank - 0x40) << 16) | off); return (u8)mdr; }
        if (bank >= 0x60 && bank <= 0x6F) { mdr = bw_read_bitmap(((i64)(bank - 0x60) << 16) | off); return (u8)mdr; }
        if (b < 0x40 && (off < 0x0800 || (off >= 0x3000 && off < 0x3800))) { mdr = iram_read(off); return (u8)mdr; }
        return (u8)mdr;
    }
    void write(u32 a, u8 vv) override {
        a &= 0xFFFFFF; int v = vv;
        wc++; mdr = v;
        int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
        if (b < 0x40 && off >= 0x2200 && off < 0x2400) { write_io_sa1(off, v); return; }
        if (b < 0x40 && off >= 0x6000 && off < 0x8000) {
            if (sw46) bw_write_bitmap((i64)cbm * 0x2000 + (off & 0x1FFF), v);
            else bw_write((i64)(cbm & 0x1F) * 0x2000 + (off & 0x1FFF), v);
            return;
        }
        if (bank >= 0x40 && bank <= 0x5F) { bw_write(((i64)(bank - 0x40) << 16) | off, v); return; }
        if (bank >= 0x60 && bank <= 0x6F) { bw_write_bitmap(((i64)(bank - 0x60) << 16) | off, v); return; }
        if (b < 0x40 && (off < 0x0800 || (off >= 0x3000 && off < 0x3800))) iram_write(off, v, true);
    }
    void sync_to_scpu();
    int snes_read(u32 a);
    bool snes_write(u32 a, int v);
    bool snes_irq_active() {
        return (cpu_irqfl && cpu_irqen && !cpu_irqcl) || (chdma_irqfl && chdma_irqen && !chdma_irqcl);
    }
    void update_sa1_irq_line() {
        bool irq = false;
        if (sa1_irqen && sa1_irqfl && !sa1_irqcl) irq = true;
        if (timer_irqen && timer_irqfl && !timer_irqcl) irq = true;
        if (dma_irqen && dma_irqfl && !dma_irqcl) irq = true;
        cpu.irq_line = irq;
        if (sa1_nmien && sa1_nmifl && !sa1_nmicl) cpu.nmi_pending = true;
    }
    void release_reset_if_needed(bool was_reset) {
        if (was_reset && !sa1_resb) {
            CPU65816& c = cpu;
            c.E = 1; c.P = 0x34; c.D = 0; c.DB = 0; c.PB = 0; c.S = 0x01FF; c.X &= 0xFF; c.Y &= 0xFF;
            c.PC = crv & 0xFFFF; c.waiting = c.stopped = false; c.nmi_pending = c.irq_line = false; c.ik_valid = false;
        }
    }
    int read_io_snes(int off);
    int read_io_sa1(int off) {
        if (off == 0x2301) return (sa1_irqfl ? 0x80 : 0) | (timer_irqfl ? 0x40 : 0) | (dma_irqfl ? 0x20 : 0) | (sa1_nmifl ? 0x10 : 0) | (smeg & 0x0F);
        if (off == 0x2302) return hcounter & 0xFF;
        if (off == 0x2303) return (hcounter >> 8) & 0xFF;
        if (off == 0x2304) return vcounter & 0xFF;
        if (off == 0x2305) return (vcounter >> 8) & 0xFF;
        if (off >= 0x2306 && off <= 0x230A) return (int)((mr >> ((off - 0x2306) * 8)) & 0xFF);
        if (off == 0x230B) return overflow ? 0x80 : 0;
        if (off == 0x230C) return vdp & 0xFF;
        if (off == 0x230D) { int v = (vdp >> 8) & 0xFF; vlb_advance(); return v; }
        return mdr;
    }
    void write_io_snes(int off, int v);
    void write_io_sa1(int off, int v) {
        switch (off) {
        case 0x2209:
            cpu_irq = v & 0x80; cpu_ivsw = v & 0x40; cpu_nvsw = v & 0x10; cmeg = v & 0x0F;
            if (cpu_irq) { cpu_irqfl = true; if (cpu_irqen) cpu_irqcl = false; }
            break;
        case 0x220A:
            sa1_irqen = v & 0x80; timer_irqen = v & 0x40; dma_irqen = v & 0x20; sa1_nmien = v & 0x10;
            if (sa1_irqen && sa1_irqfl) sa1_irqcl = false;
            if (timer_irqen && timer_irqfl) timer_irqcl = false;
            if (dma_irqen && dma_irqfl) dma_irqcl = false;
            if (sa1_nmien && sa1_nmifl) sa1_nmicl = false;
            update_sa1_irq_line(); break;
        case 0x220B:
            if (v & 0x80) { sa1_irqfl = false; sa1_irqcl = true; }
            if (v & 0x40) { timer_irqfl = false; timer_irqcl = true; }
            if (v & 0x20) { dma_irqfl = false; dma_irqcl = true; }
            if (v & 0x10) { sa1_nmifl = false; sa1_nmicl = true; }
            update_sa1_irq_line(); break;
        case 0x220C: snv = (snv & 0xFF00) | v; break;
        case 0x220D: snv = (snv & 0x00FF) | (v << 8); break;
        case 0x220E: siv = (siv & 0xFF00) | v; break;
        case 0x220F: siv = (siv & 0x00FF) | (v << 8); break;
        case 0x2210: hvselb = v & 0x80; ven = v & 0x02; hen = v & 0x01; break;
        case 0x2211: hcounter = vcounter = 0; break;
        case 0x2212: hcnt = (hcnt & 0xFF00) | v; break;
        case 0x2213: hcnt = (hcnt & 0x00FF) | ((v & 1) << 8); break;
        case 0x2214: vcnt = (vcnt & 0xFF00) | v; break;
        case 0x2215: vcnt = (vcnt & 0x00FF) | ((v & 1) << 8); break;
        case 0x2225: sw46 = v & 0x80; cbm = v & 0x7F; break;
        case 0x2227: cwen = v & 0x80; break;
        case 0x222A: ciwp = v; break;
        case 0x2230:
            dmaen = v & 0x80; dprio = v & 0x40; cden = v & 0x20; cdsel = v & 0x10;
            dd = (v & 0x04) ? 1 : 0; sd = v & 3; break;
        case 0x2231: write_cdma(v); break;
        case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237: case 0x2238: case 0x2239:
            write_io_shared(off, v); break;
        case 0x223F: bbf = (v & 0x80) ? 1 : 0; break;
        case 0x2250: md = v & 3; if (v & 2) mr = 0; break;
        case 0x2251: ma = (ma & 0xFF00) | v; break;
        case 0x2252: ma = (ma & 0x00FF) | (v << 8); break;
        case 0x2253: mb = (mb & 0xFF00) | v; break;
        case 0x2254: mb = (mb & 0x00FF) | (v << 8); do_math(); break;
        case 0x2258: hl = v & 0x80; vb = (v & 0x0F) == 0 ? 16 : (v & 0x0F); if (!hl) vlb_latch(); break;
        case 0x2259: va = (va & 0xFFFF00) | v; break;
        case 0x225A: va = (va & 0xFF00FF) | (v << 8); break;
        case 0x225B: va = (va & 0x00FFFF) | (v << 16); vbit = 0; vlb_latch(); break;
        default:
            if (off >= 0x2240 && off <= 0x224F) {
                brf[off - 0x2240] = v;
                if ((off == 0x2247 || off == 0x224F) && dmaen && cden && !cdsel) dma_cc2();
            }
        }
    }
    void write_io_shared(int off, int v) {
        if (off == 0x2232) dsa = (dsa & 0xFFFF00) | v;
        else if (off == 0x2233) dsa = (dsa & 0xFF00FF) | (v << 8);
        else if (off == 0x2234) dsa = (dsa & 0x00FFFF) | (v << 16);
        else if (off == 0x2235) dda = (dda & 0xFFFF00) | v;
        else if (off == 0x2236) {
            dda = (dda & 0xFF00FF) | (v << 8);
            if (dmaen && !cden && dd == 0) dma_normal();
            else if (dmaen && cden && cdsel) dma_cc1();
        } else if (off == 0x2237) {
            dda = (dda & 0x00FFFF) | (v << 16);
            if (dmaen && !cden && dd == 1) dma_normal();
        } else if (off == 0x2238) dtc = (dtc & 0xFF00) | v;
        else if (off == 0x2239) dtc = (dtc & 0x00FF) | (v << 8);
    }
    void write_cdma(int v) {
        chdend = v & 0x80; dmasize = std::min((v >> 2) & 7, 5); dmacb = std::min(v & 3, 2);
        if (chdend) cc1_active = false;
    }
    void do_math() {
        int sma = ma < 0x8000 ? ma : ma - 0x10000; int mode = md & 3;
        if (mode == 0) { int smb = mb < 0x8000 ? mb : mb - 0x10000; mr = ((i64)sma * smb) & 0xFFFFFFFFLL; overflow = false; }
        else if (mode == 1) {
            int umb = mb & 0xFFFF;
            if (umb == 0) mr = 0;
            else { i64 q = (i64)((double)sma / umb); i64 r = std::abs(sma) % umb; mr = (q & 0xFFFF) | ((r & 0xFFFF) << 16); }
            overflow = false;
        } else {
            int smb = mb < 0x8000 ? mb : mb - 0x10000;
            mr = mr + (i64)sma * smb;
            const i64 M40 = (1LL << 40) - 1;
            overflow = mr != (mr & M40);
            mr &= M40;
        }
    }
    int vlb_rom(int addr) { addr &= ~1; int lo = read_rom(addr); int hi = read_rom(addr + 1); return lo | (hi << 8); }
    void vlb_latch() { int raw = vlb_rom(va); int val = (raw >> vbit) & ((1 << std::min(vb, 16)) - 1); vdp = val & 0xFFFF; }
    void vlb_advance() {
        if (!hl) return;
        vbit += vb;
        while (vbit >= 16) { vbit -= 16; va = (va + 2) & 0xFFFFFF; }
        vlb_latch();
    }
    int dma_read_src(int addr) {
        if (sd == 0) return read_rom(addr);
        if (sd == 1) return bw_read(addr);
        if (sd == 2) return iram_read(addr);
        return 0xFF;
    }
    void dma_write_dst(int addr, int v) { if (dd == 0) iram_write(addr, v, true); else bw_write(addr, v); }
    void dma_normal() {
        int n = dtc & 0xFFFF; if (n == 0) n = 0x10000;
        int s = dsa, d = dda;
        for (int i = 0; i < n; i++) { int v = dma_read_src(s); dma_write_dst(d, v); s = (s + 1) & 0xFFFFFF; d = (d + 1) & 0xFFFFFF; }
        dsa = s; dda = d; dtc = 0;
        dma_irqfl = true; if (dma_irqen) dma_irqcl = false;
        update_sa1_irq_line();
    }
    void dma_cc1() { cc1_active = true; chdma_irqfl = true; if (chdma_irqen) chdma_irqcl = false; }
    int dma_cc1_read(i64 addr) {
        int charmask = (1 << (6 - dmacb)) - 1;
        if ((addr & charmask) == 0) {
            int bpp = 2 << (2 - dmacb);
            int bpl = (8 << dmasize) >> dmacb;
            i64 bwmask = bw_mask() ? bw_mask() : 0xFFFFFFFFLL;
            i64 tile = ((addr - dsa) & bwmask) >> (6 - dmacb);
            i64 ty = tile >> dmasize, tx = tile & ((1 << dmasize) - 1);
            i64 bwaddr = dsa + ty * 8 * bpl + tx * bpp;
            for (int y = 0; y < 8; y++) {
                u64 data = 0;
                for (int byte = 0; byte < bpp; byte++) data |= (u64)bw_read((bwaddr + byte) & bwmask) << (byte * 8);
                bwaddr += bpl;
                int out[8] = {0};
                for (int x = 0; x < 8; x++) {
                    out[0] |= (data & 1) << (7 - x); data >>= 1;
                    out[1] |= (data & 1) << (7 - x); data >>= 1;
                    if (dmacb == 2) continue;
                    out[2] |= (data & 1) << (7 - x); data >>= 1;
                    out[3] |= (data & 1) << (7 - x); data >>= 1;
                    if (dmacb == 1) continue;
                    out[4] |= (data & 1) << (7 - x); data >>= 1;
                    out[5] |= (data & 1) << (7 - x); data >>= 1;
                    out[6] |= (data & 1) << (7 - x); data >>= 1;
                    out[7] |= (data & 1) << (7 - x); data >>= 1;
                }
                for (int byte = 0; byte < bpp; byte++) {
                    int p = dda + (y << 1) + ((byte & 6) << 3) + (byte & 1);
                    iram[p & 0x7FF] = out[byte];
                }
            }
        }
        return iram[(dda + (addr & charmask)) & 0x7FF];
    }
    void dma_cc2() {
        const u8* b = brf + ((dma_line & 1) << 3);
        int bpp = 2 << (2 - dmacb);
        int addr = dda & 0x7FF;
        addr &= ~((1 << (7 - dmacb)) - 1);
        addr += (dma_line & 8) * bpp;
        addr += (dma_line & 7) * 2;
        for (int byte = 0; byte < bpp; byte++) {
            int output = 0;
            for (int bit = 0; bit < 8; bit++) output |= ((b[bit] >> byte) & 1) << (7 - bit);
            iram[(addr + ((byte & 6) << 3) + (byte & 1)) & 0x7FF] = output;
        }
        dma_line = (dma_line + 1) & 15;
    }
    void run_to(i64 snes_cyc) {
        if (sa1_resb || sa1_rdyb) { cpu.cyc = snes_cyc * 4; return; }
        update_sa1_irq_line();
        cpu.run(snes_cyc * 4);
    }
    void ser(Ser& s) {
        s.pod(mdr); s.pod(iram); s.pod(dma_line); s.pod(cc1_active);
        s.pod(sa1_irq); s.pod(sa1_rdyb); s.pod(sa1_nmi); s.pod(sa1_resb); s.pod(smeg);
        s.pod(cpu_irqen); s.pod(chdma_irqen); s.pod(cpu_irqcl); s.pod(chdma_irqcl);
        s.pod(crv); s.pod(cnv); s.pod(civ); s.pod(cpu_irq); s.pod(cpu_ivsw); s.pod(cpu_nvsw); s.pod(cmeg);
        s.pod(sa1_irqen); s.pod(timer_irqen); s.pod(dma_irqen); s.pod(sa1_nmien);
        s.pod(sa1_irqcl); s.pod(timer_irqcl); s.pod(dma_irqcl); s.pod(sa1_nmicl);
        s.pod(snv); s.pod(siv); s.pod(hvselb); s.pod(ven); s.pod(hen); s.pod(hcnt); s.pod(vcnt); s.pod(hcounter); s.pod(vcounter);
        s.pod(cbmode); s.pod(dbmode); s.pod(ebmode); s.pod(fbmode); s.pod(cb); s.pod(db); s.pod(eb); s.pod(fb); s.pod(sbm);
        s.pod(sw46); s.pod(cbm); s.pod(swen); s.pod(cwen); s.pod(bwp); s.pod(siwp); s.pod(ciwp);
        s.pod(dmaen); s.pod(dprio); s.pod(cden); s.pod(cdsel); s.pod(dd); s.pod(sd); s.pod(chdend); s.pod(dmasize); s.pod(dmacb);
        s.pod(dsa); s.pod(dda); s.pod(dtc); s.pod(bbf); s.pod(brf);
        s.pod(acm); s.pod(md); s.pod(ma); s.pod(mb); s.pod(mr); s.pod(overflow); s.pod(hl); s.pod(vb); s.pod(va); s.pod(vbit); s.pod(vdp);
        s.pod(cpu_irqfl); s.pod(chdma_irqfl); s.pod(sa1_irqfl); s.pod(timer_irqfl); s.pod(dma_irqfl); s.pod(sa1_nmifl); s.pod(wc);
    }
};

// ===================================================================== chip methods needing SNES
size_t SDD1::map_rom(int bk, int addr) {
    int b = bk & 0xFF;
    if (b >= 0xC0 && b <= 0xFF) {
        int bi = (b - 0xC0) >> 4, top = bank[bi] & 0x0F;
        return (((size_t)top << 20) | ((b & 0x0F) << 16) | (addr & 0xFFFF)) % snes->romlen;
    }
    return snes->rom_offset(bk, addr);
}
std::vector<u8> SDD1::dma_read(int bk, int addr, int size) {
    size_t off0 = map_rom(bk, addr);
    size_t grab = std::min(snes->rom.size() - off0, (size_t)std::max(size * 2 + 16, size + 64));
    std::vector<u8> src(snes->rom.begin() + off0, snes->rom.begin() + off0 + grab);
    return sdd1_decompress(src, size);
}

int SuperFX::rom_read(int bank, int addr) {
    int b = bank & 0x7F; size_t off;
    if (b < 0x40 && addr < 0x8000) return 0xFF;
    if (b >= 0x40) off = ((size_t)(b & 0x3F) << 16) | (addr & 0xFFFF);
    else off = ((size_t)(b & 0x3F) << 15) | (addr & 0x7FFF);
    return snes->rom[off % snes->romlen];
}
int SuperFX::ram_read(int addr) {
    auto& ram = snes->sram; if (ram.empty()) return 0xFF;
    size_t a = ((size_t)(rambr & 1) << 16) | (addr & 0xFFFF);
    return ram[a % ram.size()];
}
void SuperFX::ram_write(int addr, int v) {
    auto& ram = snes->sram; if (ram.empty()) return;
    size_t a = ((size_t)(rambr & 1) << 16) | (addr & 0xFFFF);
    ram[a % ram.size()] = v & 0xFF;
}
void SuperFX::plot() {
    int x = r[1] & 0xFF, y = r[2] & 0xFF, color = colr, md = scmr & 3;
    static const int BPPS[4] = {2, 4, 4, 8}, HTS[4] = {128, 160, 192, 256};
    int bpp = BPPS[md];
    int ht = ((scmr >> 2) & 1) | (((scmr >> 5) & 1) << 1);
    if (y >= HTS[ht]) { r[1] = (r[1] + 1) & 0xFFFF; return; }
    size_t base = (size_t)scbr << 10;
    int tx = x >> 3, ty = y >> 3, px = x & 7, py = y & 7, tile = ty * 32 + tx, bit = 7 - px;
    auto& ram = snes->sram; if (ram.empty()) return;
    for (int plane = 0; plane < bpp; plane++) {
        size_t a = (base + tile * (8 * bpp) + (plane & ~1) * 8 + py * 2 + (plane & 1)) % ram.size();
        if (color & (1 << plane)) ram[a] |= (1 << bit); else ram[a] &= ~(1 << bit);
    }
    r[1] = (r[1] + 1) & 0xFFFF;
}

SPC7110::SPC7110(SNES* s) : snes(s) {
    memset(regs, 0, sizeof regs); decomp.assign(0x10000, 0);
    auto& rom = s->rom;
    prog_size = std::min(rom.size(), (size_t)0x100000);
    if (rom.size() > prog_size) data.assign(rom.begin() + prog_size, rom.end()); else data = rom;
}
void SPC7110::begin_decomp() {
    u8* r = regs;
    i64 table = r[0x01] | (r[0x02] << 8) | (r[0x03] << 16);
    int index = r[0x04];
    i64 p = (table + index * 4) & 0xFFFFFF;
    i64 ptr = data.empty() ? 0 : (((i64)rd_data(p) << 24) | (rd_data(p + 1) << 16) | (rd_data(p + 2) << 8) | rd_data(p + 3));
    int mode = r[0x0B];
    int length = r[0x09] | (r[0x0A] << 8); if (length == 0) length = 0x10000;
    int off = r[0x05] | (r[0x06] << 8);
    i64 src_off = ptr & 0xFFFFFF;
    std::vector<u8> src;
    i64 grab = 0;
    if (!data.empty()) grab = std::min((i64)data.size() - floormod(src_off, (i64)data.size()), (i64)length * 2 + 32);
    if (!data.empty() && grab > 0) { src.resize((size_t)grab); for (i64 i = 0; i < grab; i++) src[i] = data[(size_t)((src_off + i) % data.size())]; }
    else src.assign(16, 0);
    int out_len = std::min(length, 0x10000);
    std::vector<u8> raw;
    if (mode == 0) { raw.resize(out_len); for (int i = 0; i < out_len; i++) raw[i] = rd_data(src_off + i); }
    else raw = sdd1_decompress(src, out_len);
    decomp.assign(0x10000, 0);
    int place = off % 0x10000;
    for (size_t i = 0; i < raw.size(); i++) decomp[(place + i) & 0xFFFF] = raw[i];
    decomp_off = place;
    r[0x09] = length & 0xFF; r[0x0A] = (length >> 8) & 0xFF;
    r[0x0C] = 0x80; busy = false;
}

int SA1::rom_byte(size_t off) { if (!snes->romlen) return 0xFF; return snes->rom[off % snes->romlen]; }
void SA1::iram_write(int a, int v, bool from_sa1) {
    int page = (a & 0x700) >> 8;
    int enable = from_sa1 ? ciwp : siwp;
    if (!(enable & (1 << page))) return;
    a &= 0x7FF; v &= 0xFF;
    if (iram[a] == v) return;
    iram[a] = v;
    wc++; snes->wc++;
    cpu.ik_valid = false; snes->cpu.ik_valid = false;
}
void SA1::sync_to_scpu() { run_to(snes->cpu.cyc); }
int SA1::snes_read(u32 a) {
    int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F;
    if (b < 0x40 && off >= 0x2200 && off < 0x2400) return read_io_snes(off);
    if (b < 0x40 && off >= 0x3000 && off < 0x3800) { if (off < 0x3010) sync_to_scpu(); return iram_read(off); }
    if (b < 0x40 && off >= 0x6000 && off < 0x8000) {
        i64 addr = (i64)sbm * 0x2000 + (off & 0x1FFF);
        return cc1_active ? dma_cc1_read(addr) : bw_read(addr);
    }
    if (bank >= 0x40 && bank <= 0x4F) {
        i64 addr = ((i64)(bank - 0x40) << 16) | off;
        return cc1_active ? dma_cc1_read(addr) : bw_read(addr);
    }
    if ((a & 0x408000) == 0x008000 || (a & 0xC00000) == 0xC00000 || b >= 0x40) return read_rom(a);
    return snes->mdr;
}
bool SA1::snes_write(u32 a, int v) {
    int bank = a >> 16, off = a & 0xFFFF, b = bank & 0x7F; v &= 0xFF;
    if (b < 0x40 && off >= 0x2200 && off < 0x2400) { write_io_snes(off, v); return true; }
    if (b < 0x40 && off >= 0x3000 && off < 0x3800) { if (off < 0x3010) sync_to_scpu(); iram_write(off, v, false); return true; }
    if (b < 0x40 && off >= 0x6000 && off < 0x8000) { bw_write((i64)sbm * 0x2000 + (off & 0x1FFF), v); return true; }
    if (bank >= 0x40 && bank <= 0x4F) { bw_write(((i64)(bank - 0x40) << 16) | off, v); return true; }
    return false;
}
int SA1::read_io_snes(int off) {
    if (off == 0x2300) return (cpu_irqfl ? 0x80 : 0) | (cpu_ivsw ? 0x40 : 0) | (chdma_irqfl ? 0x20 : 0) | (cpu_nvsw ? 0x10 : 0) | (cmeg & 0x0F);
    return snes->mdr;
}
void SA1::write_io_snes(int off, int v) {
    switch (off) {
    case 0x2200: {
        bool was = sa1_resb;
        sa1_irq = v & 0x80; sa1_rdyb = v & 0x40; sa1_resb = v & 0x20; sa1_nmi = v & 0x10; smeg = v & 0x0F;
        if (sa1_irq) { sa1_irqfl = true; if (sa1_irqen) sa1_irqcl = false; }
        if (sa1_nmi) { sa1_nmifl = true; if (sa1_nmien) sa1_nmicl = false; }
        release_reset_if_needed(was);
        update_sa1_irq_line(); break;
    }
    case 0x2201:
        cpu_irqen = v & 0x80; chdma_irqen = v & 0x20;
        if (cpu_irqen && cpu_irqfl) cpu_irqcl = false;
        if (chdma_irqen && chdma_irqfl) chdma_irqcl = false;
        break;
    case 0x2202:
        if (v & 0x80) { cpu_irqfl = false; cpu_irqcl = true; }
        if (v & 0x20) { chdma_irqfl = false; chdma_irqcl = true; }
        break;
    case 0x2203: crv = (crv & 0xFF00) | v; break;
    case 0x2204: crv = (crv & 0x00FF) | (v << 8); break;
    case 0x2205: cnv = (cnv & 0xFF00) | v; break;
    case 0x2206: cnv = (cnv & 0x00FF) | (v << 8); break;
    case 0x2207: civ = (civ & 0xFF00) | v; break;
    case 0x2208: civ = (civ & 0x00FF) | (v << 8); break;
    case 0x2220: cbmode = (v & 0x80) ? 1 : 0; cb = v & 7; snes->build_map(); break;
    case 0x2221: dbmode = (v & 0x80) ? 1 : 0; db = v & 7; snes->build_map(); break;
    case 0x2222: ebmode = (v & 0x80) ? 1 : 0; eb = v & 7; snes->build_map(); break;
    case 0x2223: fbmode = (v & 0x80) ? 1 : 0; fb = v & 7; snes->build_map(); break;
    case 0x2224: sbm = v & 0x1F; break;
    case 0x2226: swen = v & 0x80; break;
    case 0x2228: bwp = v & 0x0F; break;
    case 0x2229: siwp = v; break;
    case 0x2231: write_cdma(v); break;
    case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237: write_io_shared(off, v); break;
    }
}

// ===================================================================== SNES system
SNES::SNES() : cpu(this) {
    rom.assign(0x8000, 0); romlen = 0x8000;
    frame.assign(224 * 256 * 3, 0);
    objbuf.reset(new ObjBuf());
    power();
}
SNES::~SNES() { free_chips(); }
void SNES::free_chips() {
    delete sa1; delete dsp1; delete sfx; delete obc1; delete srtc; delete sdd1; delete cx4; delete st01x; delete spc7110; delete sgb;
    sa1 = nullptr; dsp1 = nullptr; sfx = nullptr; obc1 = nullptr; srtc = nullptr; sdd1 = nullptr; cx4 = nullptr;
    st01x = nullptr; spc7110 = nullptr; sgb = nullptr;
}

static std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void write_file(const std::string& path, const std::vector<u8>& d) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
static std::string strip_ext(const std::string& p) {
    size_t dot = p.find_last_of('.'), sl = p.find_last_of("/\\");
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return p;
    return p.substr(0, dot);
}

RomInfo SNES::load(const std::vector<u8>& data, const std::string& path) {
    RomInfo info = analyze_rom(data);
    rom = info.data; romlen = rom.size(); rom_path = path;
    hirom = info.hirom; exhirom = info.exhirom; title = info.title; sram_size = info.sram_size;
    map_mode = info.map_mode; chipset = info.chipset; chip_name = info.chip_name; compat = info.compat;
    mapper = info.mapper; region_name = info.region_name; speed = info.speed;
    if (is_sa1(chipset) && sram_size == 0) sram_size = 0x40000;
    if (is_sfx(chipset) && sram_size == 0) sram_size = 0x10000;
    sram.assign(sram_size, 0xFF);
    if (!path.empty() && sram_size) {
        std::string srm = strip_ext(path) + ".srm";
        if (fs::exists(srm)) {
            try { auto d = read_file(srm); size_t n = std::min(d.size(), (size_t)sram_size); memcpy(sram.data(), d.data(), n); } catch (...) {}
        }
    }
    free_chips();
    int cs = chipset;
    if (is_sa1(cs)) { sa1 = new SA1(this); sa1->attach_bwram(&sram); }
    if (is_dsp(cs)) dsp1 = new DSP1(this, hirom);
    if (is_sfx(cs)) sfx = new SuperFX(this);
    if (is_obc1(cs)) obc1 = new OBC1(this);
    if (is_srtc(cs)) srtc = new SRTC(this);
    if (is_sdd1(cs)) sdd1 = new SDD1(this);
    if (is_cx4(cs)) cx4 = new CX4(this);
    if (is_st01x(cs)) st01x = new ST01x(this, false);
    if (is_spc7110(cs)) spc7110 = new SPC7110(this);
    if (is_sgb(cs)) sgb = new SGB(this);
    power();
    return info;
}

size_t SNES::rom_offset(int bank, int off) {
    if (sa1) {
        u32 a = ((bank & 0xFF) << 16) | (off & 0xFFFF), o;
        int k = sa1->rom_linear(a, o);
        if (k == 2) return 0;
        if (k == 0) { int b = bank & 0x7F; return (((size_t)b << 15) | (off & 0x7FFF)) % romlen; }
        return o % romlen;
    }
    if (spc7110) {
        size_t mapped;
        if (spc7110->map_bank(bank, mapped)) return (spc7110->prog_size + mapped + (off & 0xFFFF)) % romlen;
    }
    if (sdd1 && (bank & 0xFF) >= 0xC0) return sdd1->map_rom(bank, off);
    int b = bank & 0x7F;
    if (exhirom && romlen > 0x400000) {
        if (b >= 0x40 && b < 0x7E) return (0x400000 + ((((size_t)(b - 0x40) & 0x3F) << 16) | off)) % romlen;
        return ((((size_t)b & 0x3F) << 16) | off) % romlen;
    }
    if (hirom) return ((((size_t)b & 0x3F) << 16) | off) % romlen;
    return (((size_t)b << 15) | (off & 0x7FFF)) % romlen;
}

void SNES::save_sram() {
    if (!rom_path.empty() && sram_size) {
        try { write_file(strip_ext(rom_path) + ".srm", sram); } catch (...) {}
    }
}

void SNES::power() {
    wram.assign(0x20000, 0);
    if (sa1) { sa1->attach_bwram(&sram); sa1->power(); }
    if (dsp1) dsp1->power();
    if (sfx) sfx->power();
    if (obc1) obc1->power();
    if (srtc) srtc->power();
    if (sdd1) sdd1->power();
    if (cx4) cx4->power();
    if (st01x) st01x->power();
    if (spc7110) spc7110->power();
    if (sgb) sgb->power();
    build_map();
    vram.assign(0x8000, 0); cgram.assign(256, 0); oam.assign(544, 0);
    R.fill(0); R[R_INIDISP] = 0x80;
    rdirty = cgdirty = vram_dirty = true;
    rsnap = R; for (int i = 0; i < 256; i++) cgsnap[i] = cgram[i];
    line_regs.clear(); line_cg.clear();
    mdr = 0; wc = 0;
    bgofs_latch = 0; m7_latch = 0; mpy = 0;
    oam_base = 0; oam_addr = 0; oam_latch = 0;
    vmain = 0x80; vmadd = 0; vlatch = 0;
    cgaddr = 0; cg_latch = 0; wmadd = 0;
    apu.reset();
    nmitimen = 0; wrio = 0xFF; wrmpya = 0xFF; wrdiv = 0xFFFF;
    rddiv = 0; rdmpy = 0; htime = 0x1FF; vtime = 0x1FF;
    hdmaen = 0; memsel = 0;
    memset(dmar, 0xFF, sizeof dmar);
    for (int i = 0; i < 8; i++) { hdma_do[i] = false; hdma_done[i] = true; }
    nmi_flag = irq_flag = vblank = false;
    vcount = 0; line_start = 0; line_end = 0;
    for (auto& j : joyreg) j = 0;
    joy_sr[0] = joy_sr[1] = 0; joy_strobe = 0;
    hlatch = vlatch_c = hl_flip = vl_flip = field = 0;
    frames = 0;
    cpu.cyc = 0; cpu.A = cpu.X = cpu.Y = 0; cpu.ik_valid = false;
    cpu.reset();
}

int SNES::chip_read(u32 a, bool& hit) {
    hit = true;
    if (sfx && sfx->handles(a)) return sfx->read_reg(a);
    if (dsp1 && dsp1->handles(a)) return dsp1->read(a);
    if (obc1 && obc1->handles(a)) return obc1->read(a);
    if (cx4 && cx4->handles(a)) return cx4->read(a);
    if (srtc && srtc->handles(a)) return srtc->read(a);
    if (sdd1 && sdd1->handles_mmio(a)) return sdd1->read_mmio(a);
    if (st01x && st01x->handles(a)) return st01x->read(a);
    if (spc7110 && spc7110->handles(a)) return spc7110->read(a);
    if (sgb && sgb->handles(a)) return sgb->read(a);
    hit = false; return 0;
}
bool SNES::chip_write(u32 a, int v) {
    if (sfx && sfx->handles(a)) { sfx->write_reg(a, v); return true; }
    if (dsp1 && dsp1->handles(a)) { dsp1->write(a, v); return true; }
    if (obc1 && obc1->handles(a)) { obc1->write(a, v); return true; }
    if (cx4 && cx4->handles(a)) { cx4->write(a, v); return true; }
    if (srtc && srtc->handles(a)) { srtc->write(a, v); return true; }
    if (sdd1 && sdd1->handles_mmio(a)) { sdd1->write_mmio(a, v); return true; }
    if (st01x && st01x->handles(a)) { st01x->write(a, v); return true; }
    if (spc7110 && spc7110->handles(a)) { spc7110->write(a, v); return true; }
    if (sgb && sgb->handles(a)) { sgb->write(a, v); return true; }
    return false;
}

void SNES::build_map() {
    size_t rl = romlen; bool hasa1 = sa1 != nullptr, dsp = dsp1 != nullptr;
    for (int p = 0; p < 2048; p++) {
        int bank = p >> 3, off = (p & 7) << 13, b = bank & 0x7F;
        u8* rb = nullptr; u8* wb = nullptr; size_t ro = 0, wo = 0; bool isrom = false;
        if (bank == 0x7E || bank == 0x7F) { rb = wb = wram.data(); ro = wo = ((bank - 0x7E) << 16) | off; }
        else if (b < 0x40 && off < 0x2000) { rb = wb = wram.data(); ro = wo = off; }
        else if (hasa1 && b < 0x40 && off < 0x8000) {}
        else if (hasa1 && bank >= 0x40 && bank <= 0x4F) {}
        else if (hasa1 && (off >= 0x8000 || bank >= 0xC0)) {}
        else if (dsp && !hirom && b >= 0x30 && b < 0x40 && off >= 0x8000) {}
        else if (b < 0x40 && off < 0x8000) {}
        else if (hirom) { ro = rom_offset(bank, off); rb = rom.data(); isrom = true; }
        else if (off >= 0x8000 || !(b >= 0x70 && b < 0x7E)) { ro = rom_offset(bank, off); rb = rom.data(); isrom = true; }
        if (isrom && ro + 0x2000 > rl) rb = nullptr;
        rptr[p] = rb ? rb + ro : nullptr;
        wptr[p] = wb ? wb + wo : nullptr;
    }
}

int SNES::read_slow(u32 a) {
    int bank = a >> 16, off = a & 0xFFFF;
    if (bank == 0x7E) return wram[off];
    if (bank == 0x7F) return wram[0x10000 | off];
    if (sa1) {
        int b = bank & 0x7F;
        if (b < 0x40 && off < 0x2000) return wram[off];
        if (b < 0x40 && off < 0x2200) return io_read(off);
        if ((b < 0x40 && ((off >= 0x2200 && off < 0x2400) || (off >= 0x3000 && off < 0x3800) || (off >= 0x6000 && off < 0x8000))) ||
            (bank >= 0x40 && bank <= 0x4F))
            return sa1->snes_read(a);
        if (b < 0x40 && off < 0x6000) return io_read(off);
        if ((a & 0x408000) == 0x008000 || (a & 0xC00000) == 0xC00000 || b >= 0x40) return sa1->read_rom(a);
        return mdr;
    }
    bool hit; int c = chip_read(a, hit);
    if (hit) return c;
    int b = bank & 0x7F;
    if (b < 0x40) {
        if (off < 0x2000) return wram[off];
        if (off >= 0x8000) return rom[rom_offset(bank, off)];
        if (off < 0x6000) return io_read(off);
        if (hirom && sram_size && b >= 0x20) return sram[((b & 0x1F) * 0x2000 + off - 0x6000) % sram_size];
        return mdr;
    }
    if (hirom) return rom[rom_offset(bank, off)];
    if (off < 0x8000 && b >= 0x70 && b < 0x7E && sram_size) return sram[((b - 0x70) * 0x8000 + off) % sram_size];
    return rom[rom_offset(bank, off)];
}

void SNES::write_slow(u32 a, int v) {
    int bank = a >> 16, off = a & 0xFFFF;
    if (bank == 0x7E) { wram[off] = v; return; }
    if (bank == 0x7F) { wram[0x10000 | off] = v; return; }
    if (sa1) {
        int b = bank & 0x7F;
        if (b < 0x40 && off < 0x2000) { wram[off] = v; return; }
        if (b < 0x40 && off < 0x2200) { io_write(off, v); return; }
        if (sa1->snes_write(a, v)) return;
        if (b < 0x40 && off < 0x6000) io_write(off, v);
        return;
    }
    if (chip_write(a, v)) return;
    int b = bank & 0x7F;
    if (b < 0x40) {
        if (off < 0x2000) { wram[off] = v; return; }
        if (off < 0x6000) { io_write(off, v); return; }
        if (off < 0x8000 && hirom && sram_size && b >= 0x20) sram[((b & 0x1F) * 0x2000 + off - 0x6000) % sram_size] = v;
        return;
    }
    if (!hirom && off < 0x8000 && b >= 0x70 && b < 0x7E && sram_size) sram[((b - 0x70) * 0x8000 + off) % sram_size] = v;
}

std::vector<u8> SNES::read_block(int bank, int addr, int n) {
    if (addr + n <= 0x10000) {
        if (bank == 0x7E) return std::vector<u8>(wram.begin() + addr, wram.begin() + addr + n);
        if (bank == 0x7F) return std::vector<u8>(wram.begin() + 0x10000 + addr, wram.begin() + 0x10000 + addr + n);
        int b = bank & 0x7F;
        if ((hirom && (b >= 0x40 || addr >= 0x8000)) || (!hirom && addr >= 0x8000)) {
            size_t o = rom_offset(bank, addr);
            if (o + n <= romlen) return std::vector<u8>(rom.begin() + o, rom.begin() + o + n);
        }
        if (b < 0x40 && addr + n <= 0x2000) return std::vector<u8>(wram.begin() + addr, wram.begin() + addr + n);
    }
    std::vector<u8> r(n);
    for (int i = 0; i < n; i++) r[i] = read((bank << 16) | ((addr + i) & 0xFFFF));
    return r;
}

// ------------------------------------------------------------- I/O
int SNES::io_read(int off) {
    int v;
    if (off >= 0x2100 && off < 0x2200) v = ppu_read(off & 0xFF);
    else if (off == 0x4016) {
        v = (joy_sr[0] >> 15) & 1; joy_sr[0] = ((joy_sr[0] << 1) | 1) & 0xFFFF;
        v |= mdr & 0xFC;
    } else if (off == 0x4017) {
        v = ((joy_sr[1] >> 15) & 1) | 0x1C; joy_sr[1] = ((joy_sr[1] << 1) | 1) & 0xFFFF;
    } else if (off >= 0x4200 && off < 0x4220) v = cpuio_read(off);
    else if (off >= 0x4300 && off < 0x4380) v = dmar[off - 0x4300];
    else v = mdr;
    mdr = v;
    return v;
}
void SNES::io_write(int off, int v) {
    mdr = v;
    if (off >= 0x2100 && off < 0x2200) ppu_write(off & 0xFF, v);
    else if (off == 0x4016) {
        if (joy_strobe && !(v & 1)) { joy_sr[0] = joy1; joy_sr[1] = joy2; }
        joy_strobe = v & 1;
    } else if (off >= 0x4200 && off < 0x4220) cpuio_write(off, v);
    else if (off >= 0x4300 && off < 0x4380) dmar[off - 0x4300] = v;
}
int SNES::cpuio_read(int off) {
    if (off == 0x4210) { int v = (nmi_flag ? 0x80 : 0) | 0x42; nmi_flag = false; return v; }
    if (off == 0x4211) { int v = (irq_flag ? 0x80 : 0) | 0x42; irq_flag = false; sync_irq_line(); return v; }
    if (off == 0x4212) { bool hb = (cpu.cyc - line_start) >= HBLANK_CYC; return (vblank ? 0x80 : 0) | (hb ? 0x40 : 0) | 0x02; }
    if (off == 0x4213) return wrio;
    if (off == 0x4214) return rddiv & 0xFF;
    if (off == 0x4215) return rddiv >> 8;
    if (off == 0x4216) return rdmpy & 0xFF;
    if (off == 0x4217) return rdmpy >> 8;
    if (off >= 0x4218) return joyreg[off - 0x4218];
    return mdr;
}
void SNES::cpuio_write(int off, int v) {
    switch (off) {
    case 0x4200: {
        int old = nmitimen; nmitimen = v;
        if ((v & 0x80) && !(old & 0x80) && nmi_flag) cpu.nmi_pending = true;
        if (!(v & 0x30)) { irq_flag = false; sync_irq_line(); }
        break;
    }
    case 0x4201: wrio = v; break;
    case 0x4202: wrmpya = v; break;
    case 0x4203: rdmpy = wrmpya * v; rddiv = v; break;
    case 0x4204: wrdiv = (wrdiv & 0xFF00) | v; break;
    case 0x4205: wrdiv = (wrdiv & 0xFF) | (v << 8); break;
    case 0x4206: if (v == 0) { rddiv = 0xFFFF; rdmpy = wrdiv; } else { rddiv = wrdiv / v; rdmpy = wrdiv % v; } break;
    case 0x4207: htime = (htime & 0x100) | v; break;
    case 0x4208: htime = (htime & 0xFF) | ((v & 1) << 8); break;
    case 0x4209: vtime = (vtime & 0x100) | v; break;
    case 0x420A: vtime = (vtime & 0xFF) | ((v & 1) << 8); break;
    case 0x420B: do_dma(v); break;
    case 0x420C: hdmaen = v; break;
    case 0x420D: memsel = v; break;
    }
}
int SNES::vremap() {
    int a = vmadd, r = (vmain >> 2) & 3;
    if (r == 1) a = (a & 0xFF00) | ((a & 0x1F) << 3) | ((a >> 5) & 7);
    else if (r == 2) a = (a & 0xFE00) | ((a & 0x3F) << 3) | ((a >> 6) & 7);
    else if (r == 3) a = (a & 0xFC00) | ((a & 0x7F) << 3) | ((a >> 7) & 7);
    return a & 0x7FFF;
}
void SNES::ppu_write(int o, int v) {
    static const int SETR[][2] = {{0x00, R_INIDISP}, {0x01, R_OBSEL}, {0x05, R_BGMODE}, {0x06, R_MOSAIC},
        {0x07, R_BG1SC}, {0x08, R_BG2SC}, {0x09, R_BG3SC}, {0x0A, R_BG4SC}, {0x0B, R_BG12NBA}, {0x0C, R_BG34NBA},
        {0x1A, R_M7SEL}, {0x23, R_W12SEL}, {0x24, R_W34SEL}, {0x25, R_WOBJSEL}, {0x26, R_WH0}, {0x27, R_WH1},
        {0x28, R_WH2}, {0x29, R_WH3}, {0x2A, R_WBGLOG}, {0x2B, R_WOBJLOG}, {0x2C, R_TM}, {0x2D, R_TS},
        {0x2E, R_TMW}, {0x2F, R_TSW}, {0x30, R_CGWSEL}, {0x31, R_CGADSUB}, {0x33, R_SETINI}};
    static int setr_map[256]; static bool init = false;
    if (!init) { for (auto& x : setr_map) x = -1; for (auto& e : SETR) setr_map[e[0]] = e[1]; init = true; }
    if (setr_map[o] >= 0) { R[setr_map[o]] = v; rdirty = true; return; }
    if (o >= 0x0D && o <= 0x14) {
        int k = o - 0x0D, idx = R_BG1HOFS + k;
        R[idx] = ((v << 8) | bgofs_latch) & 0x3FF;
        bgofs_latch = v;
        if (k == 0) { R[R_M7HOFS] = ((v << 8) | m7_latch) & 0x1FFF; m7_latch = v; }
        else if (k == 1) { R[R_M7VOFS] = ((v << 8) | m7_latch) & 0x1FFF; m7_latch = v; }
        rdirty = true; return;
    }
    if (o >= 0x1B && o <= 0x20) {
        static const int idxs[6] = {R_M7A, R_M7B, R_M7C, R_M7D, R_M7X, R_M7Y};
        int idx = idxs[o - 0x1B];
        R[idx] = ((v << 8) | m7_latch) & 0xFFFF; m7_latch = v; rdirty = true;
        if (o <= 0x1C) {
            int a = R[R_M7A]; if (a & 0x8000) a -= 0x10000;
            int b = R[R_M7B] >> 8; if (b & 0x80) b -= 0x100;
            mpy = (a * b) & 0xFFFFFF;
        }
        return;
    }
    if (o >= 0x40 && o < 0x80) {
        int p = o & 3; v &= 0xFF;
        if (apu.pin[p] != v) { apu.pin[p] = v; apu.pk_valid = false; apu.wc++; }
        if (p == 0 || p == 2) apu.port_defer = true;
        else apu.run_to(cpu.cyc);
        return;
    }
    switch (o) {
    case 0x02: oam_base = (oam_base & 0x200) | (v << 1); oam_addr = oam_base; break;
    case 0x03: oam_base = ((v & 1) << 9) | (oam_base & 0x1FE); oam_addr = oam_base; break;
    case 0x04: {
        int a = oam_addr;
        if (a < 0x200) { if (!(a & 1)) oam_latch = v; else { oam[a - 1] = oam_latch; oam[a] = v; } }
        else oam[0x200 + (a & 0x1F)] = v;
        oam_addr = (a + 1) & 0x3FF; break;
    }
    case 0x15: vmain = v; break;
    case 0x16: vmadd = (vmadd & 0xFF00) | v; vlatch = vram[vremap()]; break;
    case 0x17: vmadd = (v << 8) | (vmadd & 0xFF); vlatch = vram[vremap()]; break;
    case 0x18: { int a = vremap(); vram[a] = (vram[a] & 0xFF00) | v; vram_dirty = true;
                 if (!(vmain & 0x80)) vmadd = (vmadd + VINC[vmain & 3]) & 0xFFFF;
                 break; }
    case 0x19: { int a = vremap(); vram[a] = (vram[a] & 0xFF) | (v << 8); vram_dirty = true;
                 if (vmain & 0x80) vmadd = (vmadd + VINC[vmain & 3]) & 0xFFFF;
                 break; }
    case 0x21: cgaddr = v << 1; break;
    case 0x22: { int a = cgaddr; if (!(a & 1)) cg_latch = v; else { cgram[a >> 1] = ((v & 0x7F) << 8) | cg_latch; cgdirty = true; }
                 cgaddr = (a + 1) & 0x1FF; break; }
    case 0x32: { int c = v & 31; if (v & 0x20) R[R_COLR] = c; if (v & 0x40) R[R_COLG] = c; if (v & 0x80) R[R_COLB] = c; rdirty = true; break; }
    case 0x80: wram[wmadd] = v; wmadd = (wmadd + 1) & 0x1FFFF; break;
    case 0x81: wmadd = ((wmadd & ~0xFF) | (v & 0xFF)) & 0x1FFFF; break;
    case 0x82: wmadd = ((wmadd & ~(0xFF << 8)) | ((v & 0xFF) << 8)) & 0x1FFFF; break;
    case 0x83: wmadd = ((wmadd & ~(0xFF << 16)) | ((v & 1) << 16)) & 0x1FFFF; break;
    default: break;
    }
}
int SNES::ppu_read(int o) {
    if (o >= 0x40 && o < 0x80) { apu.run_to(cpu.cyc); return apu.pout[o & 3]; }
    switch (o) {
    case 0x34: return mpy & 0xFF;
    case 0x35: return (mpy >> 8) & 0xFF;
    case 0x36: return (mpy >> 16) & 0xFF;
    case 0x37: hlatch = (int)std::min<i64>(339, floordiv((cpu.cyc - line_start) * 340, LINE_CYC)); vlatch_c = vcount; return mdr;
    case 0x38: { int a = oam_addr; int v = a < 0x200 ? oam[a] : oam[0x200 + (a & 0x1F)]; oam_addr = (a + 1) & 0x3FF; return v; }
    case 0x39: { int v = vlatch & 0xFF; if (!(vmain & 0x80)) { vlatch = vram[vremap()]; vmadd = (vmadd + VINC[vmain & 3]) & 0xFFFF; } return v; }
    case 0x3A: { int v = vlatch >> 8; if (vmain & 0x80) { vlatch = vram[vremap()]; vmadd = (vmadd + VINC[vmain & 3]) & 0xFFFF; } return v; }
    case 0x3B: { int a = cgaddr, w = cgram[a >> 1]; cgaddr = (a + 1) & 0x1FF; return (a & 1) ? (w >> 8) & 0x7F : w & 0xFF; }
    case 0x3C: hl_flip ^= 1; return !hl_flip ? (hlatch >> 8) & 1 : hlatch & 0xFF;
    case 0x3D: vl_flip ^= 1; return !vl_flip ? (vlatch_c >> 8) & 1 : vlatch_c & 0xFF;
    case 0x3E: return 0x01;
    case 0x3F: hl_flip = vl_flip = 0; return 0x03 | (field << 7);
    case 0x80: { int v = wram[wmadd]; wmadd = (wmadd + 1) & 0x1FFFF; return v; }
    }
    return mdr;
}

// ------------------------------------------------------------- DMA
void SNES::do_dma(int mask) {
    u8* d = dmar; i64 total = 0;
    for (int ch = 0; ch < 8; ch++) {
        if (!((mask >> ch) & 1)) continue;
        int b = ch * 16;
        int ctrl = d[b], bb = d[b + 1], aaddr = d[b + 2] | (d[b + 3] << 8), abank = d[b + 4];
        int size = d[b + 5] | (d[b + 6] << 8); if (!size) size = 0x10000;
        int mode = ctrl & 7, step = (ctrl & 8) ? 0 : ((ctrl & 0x10) ? -1 : 1);
        total += size;
        std::vector<u8> sdd1_buf; bool use_sdd1 = false; size_t sdd1_i = 0;
        if (sdd1 && sdd1->dma_enable[ch] && !(ctrl & 0x80) && size > 0) { sdd1_buf = sdd1->dma_read(abank, aaddr, size); use_sdd1 = true; }
        if (!(ctrl & 0x80) && mode == 1 && bb == 0x18 && step == 1 && (vmain & 0x8F) == 0x80 && size >= 2) {
            int n = size >> 1;
            std::vector<u8> data;
            if (use_sdd1) { data.assign(sdd1_buf.begin(), sdd1_buf.begin() + std::min((size_t)n * 2, sdd1_buf.size())); sdd1_i = n * 2; }
            else data = read_block(abank, aaddr, n * 2);
            int a = vmadd & 0x7FFF;
            for (int k = 0; k < n; k++) vram[(a + k) & 0x7FFF] = data[2 * k] | (data[2 * k + 1] << 8);
            vmadd = (vmadd + n) & 0xFFFF; vram_dirty = true;
            aaddr = (aaddr + n * 2) & 0xFFFF; size -= n * 2;
        }
        const int* pat = PAT[mode]; int plen = PAT_LEN[mode], i = 0;
        if (ctrl & 0x80) {
            while (size > 0) {
                int v = ppu_read((bb + pat[i % plen]) & 0xFF);
                write((abank << 16) | aaddr, (u8)v);
                aaddr = (aaddr + step) & 0xFFFF; i++; size--;
            }
        } else {
            while (size > 0) {
                int v;
                if (use_sdd1 && sdd1_i < sdd1_buf.size()) v = sdd1_buf[sdd1_i++];
                else v = read((abank << 16) | aaddr);
                ppu_write((bb + pat[i % plen]) & 0xFF, v);
                aaddr = (aaddr + step) & 0xFFFF; i++; size--;
            }
        }
        d[b + 2] = aaddr & 0xFF; d[b + 3] = aaddr >> 8; d[b + 5] = 0; d[b + 6] = 0;
    }
    cpu.cyc += total + 2;
}
void SNES::hdma_init() {
    u8* d = dmar; int en = hdmaen;
    for (int c = 0; c < 8; c++) {
        if (!((en >> c) & 1)) { hdma_done[c] = true; continue; }
        int b = c * 16, bank = d[b + 4], addr = d[b + 2] | (d[b + 3] << 8);
        int ntrl = read((bank << 16) | addr); addr = (addr + 1) & 0xFFFF;
        if (d[b] & 0x40) {
            int lo = read((bank << 16) | addr); int hi = read((bank << 16) | ((addr + 1) & 0xFFFF));
            int ia = lo | (hi << 8);
            addr = (addr + 2) & 0xFFFF; d[b + 5] = ia & 0xFF; d[b + 6] = ia >> 8;
        }
        d[b + 8] = addr & 0xFF; d[b + 9] = addr >> 8; d[b + 10] = ntrl;
        hdma_done[c] = ntrl == 0; hdma_do[c] = true;
    }
}
void SNES::hdma_line() {
    u8* d = dmar; int en = hdmaen; i64 n = 0;
    for (int c = 0; c < 8; c++) {
        if (!((en >> c) & 1) || hdma_done[c]) continue;
        int b = c * 16, ctrl = d[b], bank = d[b + 4], addr = d[b + 8] | (d[b + 9] << 8);
        int ind = ctrl & 0x40;
        if (hdma_do[c]) {
            int bb = d[b + 1], sb, src;
            if (ind) { sb = d[b + 7]; src = d[b + 5] | (d[b + 6] << 8); } else { sb = bank; src = addr; }
            int m = ctrl & 7;
            for (int k = 0; k < PAT_LEN[m]; k++) {
                int v = read((sb << 16) | src);
                ppu_write((bb + PAT[m][k]) & 0xFF, v);
                src = (src + 1) & 0xFFFF; n++;
            }
            if (ind) { d[b + 5] = src & 0xFF; d[b + 6] = src >> 8; } else addr = src;
        }
        int ntrl = (d[b + 10] - 1) & 0xFF;
        hdma_do[c] = (ntrl & 0x80) != 0;
        if (!(ntrl & 0x7F)) {
            ntrl = read((bank << 16) | addr); addr = (addr + 1) & 0xFFFF;
            if (ind) {
                int lo = read((bank << 16) | addr); int hi = read((bank << 16) | ((addr + 1) & 0xFFFF));
                int ia = lo | (hi << 8);
                addr = (addr + 2) & 0xFFFF; d[b + 5] = ia & 0xFF; d[b + 6] = ia >> 8;
            }
            if (ntrl == 0) hdma_done[c] = true;
            hdma_do[c] = true;
        }
        d[b + 10] = ntrl; d[b + 8] = addr & 0xFF; d[b + 9] = addr >> 8;
    }
    cpu.cyc += n;
}

// ------------------------------------------------------------- frame
void SNES::sync_irq_line() {
    bool s = sa1 ? sa1->snes_irq_active() : false;
    cpu.irq_line = irq_flag || s;
}
void SNES::snap_line() {
    if (rdirty) { rsnap = R; rdirty = false; }
    if (cgdirty) { for (int i = 0; i < 256; i++) cgsnap[i] = cgram[i]; cgdirty = false; }
    line_regs.push_back(rsnap); line_cg.push_back(cgsnap);
}
void SNES::run_frame() {
    for (int v = 0; v < LINES; v++) {
        vcount = v;
        if (v == 0) {
            vblank = false; nmi_flag = false;
            line_regs.clear(); line_cg.clear();
            field ^= 1;
            if (hdmaen) hdma_init(); else for (auto& x : hdma_done) x = true;
        } else if (v <= VIS) snap_line();
        else if (v == VIS + 1) {
            vblank = true;
            if (!skip_render) render();
            nmi_flag = true;
            if (nmitimen & 0x80) cpu.nmi_pending = true;
            if (nmitimen & 1) {
                int j1 = joy1, j2 = joy2;
                int jr[8] = {j1 & 0xFF, j1 >> 8, j2 & 0xFF, j2 >> 8, 0, 0, 0, 0};
                memcpy(joyreg, jr, sizeof jr);
            }
        }
        i64 base = line_end;
        line_start = base;
        i64 end = base + LINE_CYC;
        int nt = nmitimen;
        if (nt & 0x30) {
            if ((nt & 0x30) == 0x20) { if (v == vtime) raise_irq(); }
            else if (!(nt & 0x20) || v == vtime) {
                cpu.run(base + floordiv((i64)std::min(htime, 339) * LINE_CYC, 340));
                if (sa1) { sa1->run_to(cpu.cyc); sync_irq_line(); }
                if (sfx) sfx->run_to(512);
                raise_irq();
            }
        }
        cpu.run(base + HBLANK_CYC);
        if (sa1) { sa1->run_to(cpu.cyc); sync_irq_line(); }
        if (sfx) sfx->run_to(512);
        if (v < VIS && hdmaen) hdma_line();
        cpu.run(end);
        if (sa1) { sa1->run_to(end); sync_irq_line(); }
        if (sfx) sfx->run_to(1024);
        apu.run_to(end);
        line_end = end;
    }
    apu.dsp_render_to(apu.spc.cyc);
    frames++;
}

// ------------------------------------------------------------- save states
static const char STATE_MAGIC[8] = {'V', 'S', 'N', 'E', 'S', 'C', 0, 1};
static u32 crc32_buf(const u8* p, size_t n, u32 crc = 0) {
    static u32 table[256]; static bool init = false;
    if (!init) { for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
static void ser_cpu(Ser& s, CPU65816& c) {
    s.pod(c.A); s.pod(c.X); s.pod(c.Y); s.pod(c.S); s.pod(c.D); s.pod(c.DB); s.pod(c.PB); s.pod(c.PC); s.pod(c.P); s.pod(c.E);
    s.pod(c.cyc); s.pod(c.target); s.pod(c.waiting); s.pod(c.stopped); s.pod(c.nmi_pending); s.pod(c.irq_line);
    if (s.loading) c.ik_valid = false;
}
static void ser_apu(Ser& s, APU& a) {
    s.pod(a.ram); s.pod(a.dsp); s.pod(a.dsp_addr); s.pod(a.pin); s.pod(a.pout); s.pod(a.ipl_on); s.pod(a.ipl_st); s.pod(a.ipl_y);
    s.pod(a.t_en); s.pod(a.t_tgt); s.pod(a.t_cnt); s.pod(a.t_int); s.pod(a.t_last); s.pod(a.wc); s.pod(a.treads);
    SPC700& c = a.spc;
    s.pod(c.A); s.pod(c.X); s.pod(c.Y); s.pod(c.SP); s.pod(c.PC); s.pod(c.PSW); s.pod(c.cyc); s.pod(c.stopped);
    s.pod(a.dspc.m); s.pod(a.dsp_clock);
    if (s.loading) { c.trap = a.ipl_on ? 0xFFC0 : 0x10000; a.pk_valid = a.tk_valid = false; a.dspc.out.clear(); }
}
void SNES::ser(Ser& s) {
    s.vec(wram); s.vec(vram); s.vec(cgram); s.vec(oam); s.vec(sram);
    s.pod(R); s.pod(mdr); s.pod(wc);
    s.pod(bgofs_latch); s.pod(m7_latch); s.pod(mpy); s.pod(oam_base); s.pod(oam_addr); s.pod(oam_latch);
    s.pod(vmain); s.pod(vmadd); s.pod(vlatch); s.pod(cgaddr); s.pod(cg_latch); s.pod(wmadd);
    s.pod(nmitimen); s.pod(wrio); s.pod(wrmpya); s.pod(wrdiv); s.pod(rddiv); s.pod(rdmpy); s.pod(htime); s.pod(vtime);
    s.pod(hdmaen); s.pod(memsel); s.pod(dmar); s.pod(hdma_do); s.pod(hdma_done);
    s.pod(nmi_flag); s.pod(irq_flag); s.pod(vblank); s.pod(vcount); s.pod(line_start); s.pod(line_end);
    s.pod(joyreg); s.pod(joy_sr); s.pod(joy_strobe); s.pod(hlatch); s.pod(vlatch_c); s.pod(hl_flip); s.pod(vl_flip); s.pod(field);
    s.pod(frames);
    ser_cpu(s, cpu);
    ser_apu(s, apu);
    if (sa1) { sa1->ser(s); ser_cpu(s, sa1->cpu); }
    if (dsp1) dsp1->ser(s);
    if (sfx) sfx->ser(s);
    if (obc1) obc1->ser(s);
    if (srtc) srtc->ser(s);
    if (sdd1) sdd1->ser(s);
    if (cx4) cx4->ser(s);
    if (st01x) st01x->ser(s);
    if (spc7110) spc7110->ser(s);
    if (sgb) sgb->ser(s);
}
std::vector<u8> SNES::save_state() {
    Ser s; s.loading = false;
    s.raw((void*)STATE_MAGIC, 8);
    u32 crc = crc32_buf(rom.data(), rom.size()); s.pod(crc);
    ser(s);
    return s.buf;
}
void SNES::load_state(const std::vector<u8>& blob) {
    if (blob.size() < 12 || memcmp(blob.data(), STATE_MAGIC, 8)) throw std::runtime_error("not a vsnes state file");
    Ser s; s.loading = true; s.buf = blob; s.pos = 8;
    u32 crc; s.pod(crc);
    if (crc != crc32_buf(rom.data(), rom.size())) throw std::runtime_error("state belongs to a different ROM");
    // validate on a scratch copy first is overkill; sizes are checked as we go
    ser(s);
    if (sa1) sa1->attach_bwram(&sram);
    build_map();
    rdirty = cgdirty = vram_dirty = true;
    cpu.ik_valid = false;
    sync_irq_line();
}

// ===================================================================== renderer
// Scalar re-implementation of the vectorised numpy PPU renderer: identical maths, evaluated
// per pixel from the per-scanline register / CGRAM snapshots.
struct OrdE { int ly, p; };   // p = -1 -> any priority
static const OrdE ORD0[] = {{4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {2, 1}, {3, 1}, {4, 0}, {2, 0}, {3, 0}};
static const OrdE ORD1[] = {{4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {2, 1}, {4, 0}, {2, 0}};
static const OrdE ORD1P[] = {{2, 1}, {4, 3}, {0, 1}, {1, 1}, {4, 2}, {0, 0}, {1, 0}, {4, 1}, {4, 0}, {2, 0}};
static const OrdE ORD2[] = {{4, 3}, {0, 1}, {4, 2}, {1, 1}, {4, 1}, {0, 0}, {4, 0}, {1, 0}};
static const OrdE ORD7[] = {{4, 3}, {4, 2}, {4, 1}, {0, -1}, {4, 0}};
static const int BPP_N[7] = {4, 3, 2, 2, 2, 2, 1};
static const int BPP_T[7][4] = {{2, 2, 2, 2}, {4, 4, 2}, {4, 4}, {8, 4}, {8, 2}, {4, 2}, {4}};

static inline int tile_px(const u16* VR, int bpp, int g, int fy, int fx) {
    int wbase = g * (4 * bpp) + fy, bit = 7 - fx, v = 0;
    for (int k = 0; k < bpp / 2; k++) {
        int w = VR[(wbase + k * 8) & 0x7FFF];
        v |= ((w >> bit) & 1) << (2 * k);
        v |= ((w >> (8 + bit)) & 1) << (2 * k + 1);
    }
    return v;
}

static inline void tile_row(const u16* VR, int bpp, int g, int fy, int out[8]) {
    int wbase = g * (4 * bpp) + fy;
    for (int x = 0; x < 8; x++) out[x] = 0;
    for (int k = 0; k < bpp / 2; k++) {
        int w = VR[(wbase + k * 8) & 0x7FFF];
        for (int x = 0; x < 8; x++) {
            int bit = 7 - x;
            out[x] |= (((w >> bit) & 1) << (2 * k)) | (((w >> (8 + bit)) & 1) << (2 * k + 1));
        }
    }
}

void SNES::render_obj() {
    ObjBuf& O = *objbuf;
    memset(O.op, 0, sizeof O.op);
    int obsel = R[R_OBSEL];
    const int (*sizes)[2] = OBJ_SIZES[(obsel >> 5) & 7];
    int base = (obsel & 7) << 13, gap = (((obsel >> 3) & 3) + 1) << 12;
    const u16* VR = vram.data();
    for (int i = 127; i >= 0; i--) {
        int o = i * 4;
        int hb = oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
        int x = oam[o] | ((hb & 1) << 8), y = oam[o + 1], t = oam[o + 2], at = oam[o + 3];
        int w = sizes[(hb >> 1) & 1][0], h = sizes[(hb >> 1) & 1][1];
        if (x >= 256) x -= 512;
        if (x <= -w || (y >= VIS && y + h <= 256)) continue;
        int pal = (at >> 1) & 7, pri = (at >> 4) & 3;
        for (int rr = 0; rr < h; rr++) {
            int row = (y + rr) & 0xFF;
            if (row >= VIS) continue;
            int sr = (at & 0x80) ? h - 1 - rr : rr;
            int cy = sr >> 3, fy = sr & 7;
            for (int cc = 0; cc < w; cc++) {
                int col = x + cc;
                if (col < 0 || col >= 256) continue;
                int sc = (at & 0x40) ? w - 1 - cc : cc;
                int cx = sc >> 3, fx = sc & 7;
                int tn = ((((t >> 4) + cy) & 0xF) << 4) | (((t & 0xF) + cx) & 0xF);
                int wa = (base + (gap * ((at & 1) ? 1 : 0)) + tn * 16) & 0x7FFF;
                int v = tile_px(VR, 4, wa >> 4, fy, fx);
                if (!v) continue;
                O.idx[row][col] = 128 + (pal << 4) + v;
                O.pri[row][col] = pri; O.op[row][col] = true; O.hi[row][col] = pal >= 4;
            }
        }
    }
}

void SNES::render() {
    vram_dirty = false;
    if (line_regs.empty()) snap_line();
    std::vector<RegSnap> regs = line_regs; std::vector<CgSnap> cgs = line_cg;
    while ((int)regs.size() < VIS) { regs.push_back(regs.back()); cgs.push_back(cgs.back()); }
    render_obj();
    frame_w = 256; frame_h = 224;
    if (frame.size() != 224 * 256 * 3) frame.assign(224 * 256 * 3, 0);
    for (int y = 0; y < VIS; y++) render_line(y, regs[y], cgs[y], &frame[(size_t)y * 256 * 3]);
}

void SNES::render_line(int y, const RegSnap& L, const CgSnap& pal, u8* outrow) {
    const u16* VR = vram.data();
    ObjBuf& O = *objbuf;
    int bgmode = L[R_BGMODE], mode = bgmode & 7;
    int anyen = L[R_TM] | L[R_TS];
    static int lc[4][256], lpr[4][256]; static bool lop[4][256]; bool have[4] = {false, false, false, false};
    if (mode == 7) {
        if (anyen & 1) {
            have[0] = true;
            auto sx16 = [](i64 v) -> i64 { return ((v + 0x8000) & 0xFFFF) - 0x8000; };
            auto sx13 = [](i64 v) -> i64 { return ((v + 0x1000) & 0x1FFF) - 0x1000; };
            i64 A = sx16(L[R_M7A]), B = sx16(L[R_M7B]), C = sx16(L[R_M7C]), D = sx16(L[R_M7D]);
            i64 cx = sx13(L[R_M7X]), cy = sx13(L[R_M7Y]), ho = sx13(L[R_M7HOFS]), vo = sx13(L[R_M7VOFS]);
            int sel = L[R_M7SEL];
            i64 yy = y + 1; i64 sy = (sel & 2) ? 255 - yy : yy;
            auto clip = [](i64 v) -> i64 { return (v & 0x2000) ? (v | ~(i64)0x3FF) : (v & 0x3FF); };
            i64 hx = clip(ho - cx), vy = clip(vo - cy);
            for (int x = 0; x < 256; x++) {
                i64 sx = (sel & 1) ? 255 - x : x;
                i64 X = ((A * hx) & ~(i64)63) + ((B * vy) & ~(i64)63) + ((B * sy) & ~(i64)63) + cx * 256 + A * sx;
                i64 Y = ((C * hx) & ~(i64)63) + ((D * vy) & ~(i64)63) + ((D * sy) & ~(i64)63) + cy * 256 + C * sx;
                i64 tx = X >> 8, ty = Y >> 8;
                bool oob = ((tx | ty) & ~(i64)1023) != 0;
                tx &= 1023; ty &= 1023;
                int tile = VR[((ty >> 3) << 7) | (tx >> 3)] & 0xFF;
                int fine = (int)(((ty & 7) << 3) | (tx & 7));
                int c = VR[(tile << 6) | fine] >> 8;
                if ((sel & 0x80) && oob) c = (sel & 0x40) ? (VR[fine] >> 8) : 0;
                lc[0][x] = c; lpr[0][x] = 0; lop[0][x] = c != 0;
            }
        }
    } else {
        for (int i = 0; i < BPP_N[mode]; i++) {
            if (!(anyen & (1 << i))) continue;
            have[i] = true;
            int bpp = BPP_T[mode][i];
            int sc = L[R_BG1SC + i], nba = L[R_BG12NBA + (i >> 1)];
            int chr_base = ((nba >> (4 * (i & 1))) & 0xF) << 12;
            int map_base = (sc & 0xFC) << 8;
            int big = (bgmode >> (4 + i)) & 1;
            int hofs = L[R_BG1HOFS + 2 * i], vofs = L[R_BG1VOFS + 2 * i];
            int py = (vofs + y + 1) & 0x3FF;
            int tm = big ? 15 : 7, sh = big ? 4 : 3;
            int wide = sc & 1, tall = (sc >> 1) & 1;
            int trow = (py >> sh) & (tall ? 63 : 31);
            int rowbase = map_base + (((trow >> 5) * (wide ? 2 : 1)) << 10) + ((trow & 31) << 5);
            int wpt = 4 * bpp, gmask = (32768 / wpt) - 1, cbase = chr_base / wpt;
            int ckey = -1, crow[8];
            for (int x = 0; x < 256; x++) {
                int px = (((mode == 5 || mode == 6) ? 2 * x : x) + hofs) & 0x3FF;
                int tcol = (px >> sh) & (wide ? 63 : 31);
                int ent = VR[(rowbase + ((tcol >> 5) << 10) + (tcol & 31)) & 0x7FFF];
                int fx = (px & tm) ^ (((ent >> 14) & 1) * tm);
                int fy = (py & tm) ^ (((ent >> 15) & 1) * tm);
                int tile = ent & 0x3FF;
                if (big) { tile = (tile + (fx >> 3) + ((fy >> 3) << 4)) & 0x3FF; fx &= 7; fy &= 7; }
                int g = (cbase + tile) & gmask;
                int key = (g << 3) | fy;
                if (key != ckey) { ckey = key; tile_row(VR, bpp, g, fy, crow); }
                int c = crow[fx];
                int cidx;
                if (bpp == 8) cidx = c;
                else if (mode == 0) cidx = c + (((ent >> 10) & 7) << 2) + (i << 5);
                else if (bpp == 2) cidx = c + (((ent >> 10) & 7) << 2);
                else cidx = c + (((ent >> 10) & 7) << 4);
                lc[i][x] = cidx; lpr[i][x] = (ent >> 13) & 1; lop[i][x] = c != 0;
            }
        }
    }
    const OrdE* order; int on;
    if (mode == 7) { order = ORD7; on = 5; }
    else if (mode == 0) { order = ORD0; on = 12; }
    else if (mode == 1 && (bgmode & 8)) { order = ORD1P; on = 10; }
    else if (mode == 1) { order = ORD1; on = 10; }
    else { order = ORD2; on = 8; }

    // windows (layers 0-3 BG, 4 OBJ, 5 colour)
    static bool win[6][256];
    int cgw = L[R_CGWSEL], cga = L[R_CGADSUB];
    bool nomath = !(cga & 0x3F) && !(cgw & 0xC0);
    int wneed = ((L[R_TMW] | L[R_TSW]) & 0x1F) | (nomath ? 0 : 0x20);
    for (int layer = 0; layer < 6; layer++) {
        if (!((wneed >> layer) & 1)) continue;
        int sel, log;
        if (layer < 4) { sel = (L[R_W12SEL + (layer >> 1)] >> (4 * (layer & 1))) & 15; log = (L[R_WBGLOG] >> (2 * layer)) & 3; }
        else if (layer == 4) { sel = L[R_WOBJSEL] & 15; log = L[R_WOBJLOG] & 3; }
        else { sel = (L[R_WOBJSEL] >> 4) & 15; log = (L[R_WOBJLOG] >> 2) & 3; }
        bool e1 = sel & 2, e2 = sel & 8;
        for (int x = 0; x < 256; x++) {
            bool w1 = ((x >= L[R_WH0]) && (x <= L[R_WH1])) ^ ((sel & 1) != 0);
            bool w2 = ((x >= L[R_WH2]) && (x <= L[R_WH3])) ^ ((sel & 4) != 0);
            bool comb = log == 0 ? (w1 || w2) : log == 1 ? (w1 && w2) : log == 2 ? (w1 != w2) : (w1 == w2);
            win[layer][x] = (e1 && e2) ? comb : (e1 ? w1 : (e2 ? w2 : false));
        }
    }
    // per-line filtered priority lists (only layers enabled on this screen and present)
    OrdE mlist[12], slist[12]; int mn = 0, sn = 0;
    for (int k = 0; k < on; k++) {
        int Ly = order[k].ly;
        if (Ly != 4 && !have[Ly]) continue;
        if ((L[R_TM] >> Ly) & 1) mlist[mn++] = order[k];
        if ((L[R_TS] >> Ly) & 1) slist[sn++] = order[k];
    }
    const bool* opy = O.op[y]; const u8* opr = O.pri[y]; const int* oix = O.idx[y];
    auto compose = [&](int x, const OrdE* lst, int n, int wen, int& idx, int& src) {
        for (int k = 0; k < n; k++) {
            int Ly = lst[k].ly, p = lst[k].p;
            bool m; int c;
            if (Ly == 4) { m = opy[x] && opr[x] == p; c = oix[x]; }
            else { m = p < 0 ? lop[Ly][x] : (lop[Ly][x] && lpr[Ly][x] == p); c = lc[Ly][x]; }
            if (m && ((wen >> Ly) & 1) && win[Ly][x]) m = false;
            if (m) { idx = c; src = Ly; return; }
        }
        idx = 0; src = 5;
    };
    int ini = L[R_INIDISP];
    int br = (ini & 0x80) ? 0 : (ini & 15) + 1;
    u8 lut[32]; for (int v = 0; v < 32; v++) { int o = (v * br) >> 4; lut[v] = (u8)((o << 3) | (o >> 2)); }
    int m_black = (cgw >> 6) & 3, m_prev = (cgw >> 4) & 3;
    bool use_sub = cgw & 2;
    int fixed[3] = {L[R_COLR], L[R_COLG], L[R_COLB]};
    for (int x = 0; x < 256; x++) {
        int midx, msrc;
        compose(x, mlist, mn, L[R_TMW], midx, msrc);
        int w = pal[midx];
        int mc[3] = {w & 31, (w >> 5) & 31, (w >> 10) & 31};
        int res[3];
        if (nomath) { res[0] = mc[0]; res[1] = mc[1]; res[2] = mc[2]; }
        else {
            bool cw = win[5][x];
            auto region = [&](int m2) { return m2 == 0 ? false : m2 == 1 ? !cw : m2 == 2 ? cw : true; };
            bool black = region(m_black), prevent = region(m_prev);
            int srcbit = msrc == 4 ? (O.hi[y][x] ? 0x10 : 0) : (1 << msrc);
            bool domath = ((cga & srcbit) != 0) && !prevent;
            if (black) mc[0] = mc[1] = mc[2] = 0;
            int opnd[3]; bool half;
            if (use_sub) {
                int sidx, ssrc; compose(x, slist, sn, L[R_TSW], sidx, ssrc);
                bool sback = ssrc == 5;
                if (!sback) { int sw = pal[sidx]; opnd[0] = sw & 31; opnd[1] = (sw >> 5) & 31; opnd[2] = (sw >> 10) & 31; }
                else { opnd[0] = fixed[0]; opnd[1] = fixed[1]; opnd[2] = fixed[2]; }
                half = (cga & 0x40) && !black && !sback;
            } else {
                opnd[0] = fixed[0]; opnd[1] = fixed[1]; opnd[2] = fixed[2];
                half = (cga & 0x40) && !black;
            }
            for (int ch = 0; ch < 3; ch++) {
                if (!domath) { res[ch] = mc[ch]; continue; }
                int r = (cga & 0x80) ? mc[ch] - opnd[ch] : mc[ch] + opnd[ch];
                if (half) r >>= 1;
                res[ch] = r < 0 ? 0 : (r > 31 ? 31 : r);
            }
        }
        outrow[x * 3] = lut[res[0]]; outrow[x * 3 + 1] = lut[res[1]]; outrow[x * 3 + 2] = lut[res[2]];
    }
}

// ===================================================================== built-in demo cartridge
struct Glyph { char ch; const char* rows; };
static const Glyph FONT[] = {
    {'A', "0E 11 11 1F 11 11 11"}, {'B', "1E 11 11 1E 11 11 1E"}, {'C', "0E 11 10 10 10 11 0E"},
    {'D', "1E 11 11 11 11 11 1E"}, {'E', "1F 10 10 1E 10 10 1F"}, {'F', "1F 10 10 1E 10 10 10"},
    {'G', "0E 11 10 17 11 11 0F"}, {'H', "11 11 11 1F 11 11 11"}, {'I', "0E 04 04 04 04 04 0E"},
    {'J', "07 02 02 02 02 12 0C"}, {'K', "11 12 14 18 14 12 11"}, {'L', "10 10 10 10 10 10 1F"},
    {'M', "11 1B 15 15 11 11 11"}, {'N', "11 11 19 15 13 11 11"}, {'O', "0E 11 11 11 11 11 0E"},
    {'P', "1E 11 11 1E 10 10 10"}, {'Q', "0E 11 11 11 15 12 0D"}, {'R', "1E 11 11 1E 14 12 11"},
    {'S', "0F 10 10 0E 01 01 1E"}, {'T', "1F 04 04 04 04 04 04"}, {'U', "11 11 11 11 11 11 0E"},
    {'V', "11 11 11 11 11 0A 04"}, {'W', "11 11 11 15 15 15 0A"}, {'X', "11 11 0A 04 0A 11 11"},
    {'Y', "11 11 11 0A 04 04 04"}, {'Z', "1F 01 02 04 08 10 1F"}, {'0', "0E 11 13 15 19 11 0E"},
    {'1', "04 0C 04 04 04 04 0E"}, {'2', "0E 11 01 02 04 08 1F"}, {'3', "1F 02 04 02 01 11 0E"},
    {'4', "02 06 0A 12 1F 02 02"}, {'5', "1F 10 1E 01 01 11 0E"}, {'6', "06 08 10 1E 11 11 0E"},
    {'7', "1F 01 02 04 08 08 08"}, {'8', "0E 11 11 0E 11 11 0E"}, {'9', "0E 11 11 0F 01 02 0C"},
    {'.', "00 00 00 00 00 0C 0C"}, {'-', "00 00 00 1F 00 00 00"}, {'(', "02 04 08 08 08 04 02"},
    {')', "08 04 02 02 02 04 08"}, {'/', "00 01 02 04 08 10 00"}, {':', "00 0C 0C 00 0C 0C 00"},
    {'!', "04 04 04 04 04 00 04"}, {'>', "00 08 0C 0E 0C 08 00"}, {'<', "00 02 06 0E 06 02 00"}};
static bool g_legacy_demo_text = false;   // --vsnes-demo: original Python demo text (for A/B verification)
static const Glyph DEMO_PLUS = {'+', "00 04 04 1F 04 04 00"};
static std::vector<Glyph> demo_font() { std::vector<Glyph> f(std::begin(FONT), std::end(FONT)); if (!g_legacy_demo_text) f.push_back(DEMO_PLUS); return f; }
static std::string demo_chars() { std::string s = " "; for (auto& g : demo_font()) s += g.ch; return s; }
static void glyph_rows(const char* rows, int out[7]) {
    std::istringstream is(rows); std::string h; int i = 0;
    while (is >> h && i < 7) out[i++] = (int)strtol(h.c_str(), nullptr, 16);
}

static const char* CAT[16] = {
    "  #          #  ", " #+#        #+# ", " #++#      #++# ", " #+++######+++# ",
    " #++++++++++++# ", "#++++++++++++++#", "#++oo++++++oo++#", "#++oo++++++oo++#",
    "#++++++##++++++#", "#+++#++++++#+++#", "#++++######++++#", " #++++++++++++# ",
    " #++++++++++++# ", "  #++++++++++#  ", "   ##########   ", "                "};

struct DemoLine { int row; std::string text; int pal; };
static std::vector<DemoLine> demo_text() {
    if (g_legacy_demo_text)
        return {{3, "VSNES 0.1", 1}, {5, "VIRTUAL SNES EMULATOR", 1}, {7, "CLASSIC DESKTOP FRONTEND", 0},
                {10, "65816  SPC700  PPU MODES 0-7", 0}, {11, "DMA HDMA WINDOWS COLOR MATH", 0},
                {12, "S-DSP AUDIO  60 FPS PACING", 0}, {15, "D-PAD MOVES THE CAT", 0},
                {17, "FILE > OPEN ROM TO START", 0}, {24, "VSNES 0.1", 0}, {25, "FILE  CPU  OPTIONS  HELP", 0}};
    return {{3, "KONDO VSNES 0.1.1A", 1}, {5, "VIRTUAL SNES EMULATOR", 1}, {7, "NATIVE C++ CORE", 0},
            {10, "65816  SPC700  PPU MODES 0-7", 0}, {11, "DMA HDMA WINDOWS COLOR MATH", 0},
            {12, "S-DSP AUDIO  60 FPS PACING", 0}, {15, "D-PAD MOVES THE CAT", 0},
            {17, "FILE > LOAD GAME TO START", 0}, {24, "KONDO VSNES 0.1.1A", 0}, {25, "BY MR KONDO", 0}};
}

struct Asm {
    std::vector<u8> c; int org; std::map<std::string, int> lab;
    struct Fix { size_t pos; std::string name; int kind; int add; };   // kind 0 = rel8, 1 = abs16
    std::vector<Fix> fix;
    explicit Asm(int o) : org(o) {}
    int pc() { return org + (int)c.size(); }
    void L(const std::string& n) { lab[n] = pc(); }
    void B(std::initializer_list<int> a) { for (int x : a) c.push_back((u8)x); }
    void W(int v) { c.push_back(v & 0xFF); c.push_back((v >> 8) & 0xFF); }
    void A16(const std::string& n, int add = 0) { fix.push_back({c.size(), n, 1, add}); W(0); }
    void R8(const std::string& n) { fix.push_back({c.size(), n, 0, 0}); c.push_back(0); }
    void resolve() {
        for (auto& f : fix) {
            auto it = lab.find(f.name);
            if (it == lab.end()) throw std::runtime_error("demo asm: missing label " + f.name);
            int t = it->second;
            if (f.kind == 0) {
                int d = t - (org + (int)f.pos + 1);
                if (d < -128 || d >= 128) throw std::runtime_error("demo asm: branch out of range " + f.name);
                c[f.pos] = d & 0xFF;
            } else { t += f.add; c[f.pos] = t & 0xFF; c[f.pos + 1] = (t >> 8) & 0xFF; }
        }
    }
};

static std::vector<u8> build_spc_demo() {
    Asm a(0x0200);
    auto dsp = [&](int reg, int val) { a.B({0x8F, reg, 0xF2, 0x8F, val, 0xF3}); };
    auto rel = [&](int op, const std::string& n) { a.c.push_back(op); a.R8(n); };
    auto absx = [&](int op, const std::string& n, int add) { a.c.push_back(op); a.A16(n, add); };
    dsp(0x6C, 0x20);
    dsp(0x5D, 0x04);
    int init[][2] = {{0x0C, 0x60}, {0x1C, 0x60}, {0x2C, 0}, {0x3C, 0}, {0x2D, 0}, {0x3D, 0}, {0x4D, 0}, {0x5C, 0},
                     {0x00, 0x60}, {0x01, 0x48}, {0x04, 0x00}, {0x05, 0xBF}, {0x06, 0x52}};
    for (auto& rv : init) dsp(rv[0], rv[1]);
    a.B({0x8F, 0x80, 0xFA});
    a.B({0x8F, 0x81, 0xF1});
    a.L("loop");
    a.B({0xE4, 0xF4, 0xBC, 0xC4, 0xF5});
    a.B({0xE4, 0xFD}); rel(0xF0, "loop");
    a.B({0xAB, 0x11, 0xFA, 0x11, 0xF6});
    a.B({0xE4, 0x11, 0x28, 0x0F}); rel(0xD0, "loop");
    a.B({0xE4, 0x10, 0xBC, 0x28, 0x07, 0xC4, 0x10, 0x1C, 0x5D});
    absx(0xF5, "notes", 0); a.B({0x8F, 0x02, 0xF2, 0xC4, 0xF3});
    absx(0xF5, "notes", 1); a.B({0x8F, 0x03, 0xF2, 0xC4, 0xF3});
    dsp(0x4C, 0x01);
    rel(0x2F, "loop");
    a.L("notes");
    for (double f : {523.25, 659.26, 783.99, 1046.5, 783.99, 659.26, 523.25, 392.0}) {
        int p = pyround(f * 16 * 4096 / 32000); a.B({p & 0xFF, p >> 8});
    }
    a.resolve();
    std::vector<u8> img(0x204, 0);
    if (a.c.size() > 0x100) throw std::runtime_error("spc demo too large");
    std::copy(a.c.begin(), a.c.end(), img.begin());
    int nib[16];
    for (int k = 0; k < 16; k++) nib[k] = std::max(-8, std::min(7, pyround(5 * sin(2 * CL_PI * k / 16) + 2 * sin(4 * CL_PI * k / 16))));
    img[0x100] = 0xC3;
    for (int k = 0; k < 8; k++) img[0x101 + k] = ((nib[2 * k] & 15) << 4) | (nib[2 * k + 1] & 15);
    img[0x200] = 0x00; img[0x201] = 0x03; img[0x202] = 0x00; img[0x203] = 0x03;
    return img;
}

static std::vector<u8> build_demo_rom() {
    std::vector<u8> SPC_DEMO = build_spc_demo();
    std::string CHARS = demo_chars();
    Asm a(0x8000);
    auto lda = [&](int v) { a.B({0xA9, v}); };
    auto sta = [&](int ad) { a.B({0x8D}); a.W(ad); };
    auto stz = [&](int ad) { a.B({0x9C}); a.W(ad); };
    auto ldx = [&](int v) { a.B({0xA2}); a.W(v); };
    auto stx = [&](int ad) { a.B({0x8E}); a.W(ad); };
    auto dma = [&](int ctrl, int bbad, const std::string& src, int size) {
        lda(ctrl); sta(0x4300); lda(bbad); sta(0x4301);
        a.B({0xA2}); a.A16(src); stx(0x4302); stz(0x4304); ldx(size); stx(0x4305);
        lda(1); sta(0x420B);
    };
    // data
    std::vector<u8> font(16, 0);
    for (auto& g : demo_font()) {
        int rows[7]; glyph_rows(g.rows, rows);
        font.push_back(0); font.push_back(0);
        for (int r = 0; r < 7; r++) { font.push_back((u8)(rows[r] << 2)); font.push_back(0); }
    }
    std::vector<int> tmap(1024, 0);
    for (auto& dl : demo_text()) {
        int col = (32 - (int)dl.text.size()) / 2;
        for (size_t k = 0; k < dl.text.size(); k++) {
            size_t f = CHARS.find(dl.text[k]);
            tmap[dl.row * 32 + col + k] = (f == std::string::npos ? 0 : (int)f) | (dl.pal << 10);
        }
    }
    std::vector<u8> tmap_b; for (int w : tmap) { tmap_b.push_back(w & 0xFF); tmap_b.push_back(w >> 8); }
    std::vector<u8> spr(18 * 32, 0);
    for (int yy = 0; yy < 16; yy++) for (int xx = 0; xx < 16; xx++) {
        char ch = CAT[yy][xx]; int c = ch == '#' ? 1 : ch == '+' ? 2 : ch == 'o' ? 3 : 0;
        int t = (xx >> 3) + (yy >> 3) * 16, r = yy & 7, bit = 0x80 >> (xx & 7), base = t * 32;
        if (c & 1) spr[base + r * 2] |= bit;
        if (c & 2) spr[base + r * 2 + 1] |= bit;
    }
    auto words = [](std::initializer_list<int> ws) { std::vector<u8> v; for (int w : ws) { v.push_back(w & 0xFF); v.push_back(w >> 8); } return v; };
    std::vector<u8> bgpal = words({0x0000, 0x7E89, 0, 0, 0, 0x7F54, 0, 0});
    std::vector<u8> objpal = words({0x0000, 0x7E89, 0x7FDD, 0x0C63});
    std::vector<std::vector<u8>> waves;
    for (int ph = 0; ph < 16; ph++) {
        std::vector<u8> t = {24, 0, 0};
        for (int k = 0; k < 24; k++) {
            int off = pyround(2 * sin(2 * CL_PI * (k / 24.0 + ph / 16.0))) & 0x3FF;
            t.push_back(1); t.push_back(off & 0xFF); t.push_back(off >> 8);
        }
        t.insert(t.end(), {1, 0, 0, 0});
        waves.push_back(t);
    }
    auto textrow = [&](const std::string& text, int palno) {
        std::vector<int> row(32, 0); int col = (32 - (int)text.size()) / 2;
        for (size_t k = 0; k < text.size(); k++) row[col + k] = (int)CHARS.find(text[k]) | (palno << 10);
        std::vector<u8> b; for (int w : row) { b.push_back(w & 0xFF); b.push_back(w >> 8); } return b;
    };
    auto app = [&](const std::vector<u8>& v) { a.c.insert(a.c.end(), v.begin(), v.end()); };

    a.L("reset");
    a.B({0x78, 0x18, 0xFB});
    a.B({0xC2, 0x38});
    ldx(0x1FFF); a.B({0x9A});
    a.B({0xA9}); a.W(0); a.B({0x5B});
    a.B({0xE2, 0x20});
    lda(0x80); sta(0x2100); stz(0x4200);
    stz(0x2105);
    lda(0x04); sta(0x2107);
    lda(0x01); sta(0x210B);
    lda(0x03); sta(0x2101);
    lda(0xFF); sta(0x210E); sta(0x210E);
    stz(0x210D); stz(0x210D);
    lda(0x80); sta(0x2115);
    ldx(0x1000); stx(0x2116); dma(0x01, 0x18, "font", (int)font.size());
    ldx(0x0400); stx(0x2116); dma(0x01, 0x18, "tmap", 2048);
    ldx(0x6000); stx(0x2116); dma(0x01, 0x18, "spr", (int)spr.size());
    stz(0x2121); dma(0x00, 0x22, "bgpal", 16);
    lda(0x80); sta(0x2121); dma(0x00, 0x22, "objpal", 8);
    stz(0x2102); stz(0x2103); ldx(0x0080);
    a.L("oam1"); stz(0x2104); lda(0xF0); sta(0x2104); stz(0x2104); stz(0x2104); a.B({0xCA, 0xD0}); a.R8("oam1");
    lda(0x02); sta(0x2104); ldx(0x001F);
    a.L("oam2"); stz(0x2104); a.B({0xCA, 0xD0}); a.R8("oam2");
    a.B({0xC2, 0x20}); a.B({0xA9}); a.W(120); a.B({0x85, 0x00}); a.B({0xA9}); a.W(168); a.B({0x85, 0x02}); a.B({0x64, 0x08}); a.B({0xE2, 0x20});
    a.L("w_aabb"); a.B({0xAE}); a.W(0x2140); a.B({0xE0}); a.W(0xBBAA); a.B({0xD0}); a.R8("w_aabb");
    ldx(0x0200); stx(0x2142); lda(0x01); sta(0x2141); lda(0xCC); sta(0x2140);
    a.L("w_cc"); a.B({0xCD}); a.W(0x2140); a.B({0xD0}); a.R8("w_cc");
    a.B({0xA0}); a.W(0);
    a.L("up"); a.B({0xB9}); a.A16("spcprog"); sta(0x2141); a.B({0x98}); sta(0x2140);
    a.L("w_up"); a.B({0xCD}); a.W(0x2140); a.B({0xD0}); a.R8("w_up");
    a.B({0xC8, 0xC0}); a.W((int)SPC_DEMO.size()); a.B({0xD0}); a.R8("up");
    ldx(0x0200); stx(0x2142); stz(0x2141);
    a.B({0x98, 0x18, 0x69, 0x02}); sta(0x2140);
    a.L("w_go"); a.B({0xCD}); a.W(0x2140); a.B({0xD0}); a.R8("w_go");
    lda(0x55); sta(0x2140); ldx(0x4000);
    a.L("w_echo"); a.B({0xAD}); a.W(0x2141); a.B({0xC9, 0x56, 0xF0}); a.R8("spc_ok"); a.B({0xCA, 0xD0}); a.R8("w_echo");
    ldx(0x0680); stx(0x2116); dma(0x01, 0x18, "failrow", 64); a.B({0x80}); a.R8("spc_done");
    a.L("spc_ok"); ldx(0x0680); stx(0x2116); dma(0x01, 0x18, "okrow", 64);
    a.L("spc_done");
    lda(0x11); sta(0x212C);
    lda(0x02); sta(0x4300); lda(0x0D); sta(0x4301); a.B({0xA2}); a.A16("wave0"); stx(0x4302); stz(0x4304);
    lda(0x01); sta(0x420C);
    lda(0x0F); sta(0x2100);
    lda(0x81); sta(0x4200);
    a.L("main"); a.B({0xCB, 0x80}); a.R8("main");
    a.L("nmi");
    a.B({0xC2, 0x30, 0x48, 0xDA, 0x5A, 0xE2, 0x20});
    a.B({0xAD}); a.W(0x4210);
    a.L("wj"); a.B({0xAD}); a.W(0x4212); a.B({0x29, 0x01, 0xD0}); a.R8("wj");
    a.B({0xC2, 0x20});
    a.B({0xAD}); a.W(0x4218); a.B({0x85, 0x06});
    a.B({0xE6, 0x08});
    struct S { int mask, op, var; const char* lb; } steps[4] = {{0x0100, 0xE6, 0, "s1"}, {0x0200, 0xC6, 0, "s2"}, {0x0800, 0xC6, 2, "s3"}, {0x0400, 0xE6, 2, "s4"}};
    for (auto& s : steps) { a.B({0xA5, 0x06, 0x89}); a.W(s.mask); a.B({0xF0}); a.R8(s.lb); a.B({s.op, s.var}); a.L(s.lb); }
    a.B({0xA5, 0x08, 0x4A, 0x29}); a.W(0x000F); a.B({0x0A, 0xAA, 0xBD}); a.A16("ptrtab"); a.B({0x8D}); a.W(0x4302);
    a.B({0xE2, 0x20});
    stz(0x2102); stz(0x2103); a.B({0xA5, 0x00}); sta(0x2104); a.B({0xA5, 0x02}); sta(0x2104);
    stz(0x2104); lda(0x30); sta(0x2104);
    a.B({0xC2, 0x30, 0x7A, 0xFA, 0x68, 0x40});
    a.L("irq"); a.B({0x40});
    a.L("spcprog"); app(SPC_DEMO);
    a.L("okrow"); app(textrow("SPC700 SOUND CPU ONLINE", 1));
    a.L("failrow"); app(textrow("SPC700 SOUND CPU FAILED", 0));
    a.L("font"); app(font);
    a.L("tmap"); app(tmap_b);
    a.L("spr"); app(spr);
    a.L("bgpal"); app(bgpal);
    a.L("objpal"); app(objpal);
    for (int i = 0; i < 16; i++) { a.L("wave" + std::to_string(i)); app(waves[i]); }
    a.L("ptrtab");
    for (int i = 0; i < 16; i++) a.A16("wave" + std::to_string(i));
    a.resolve();
    if (a.c.size() >= 0x7FB0) throw std::runtime_error("demo rom too large");
    std::vector<u8> rom(0x8000, 0);
    std::copy(a.c.begin(), a.c.end(), rom.begin());
    int h = 0x7FC0;
    const char* t = g_legacy_demo_text ? "VSNES DEMO" : "VSNES DEMO";
    for (int i = 0; i < 21; i++) rom[h + i] = i < (int)strlen(t) ? t[i] : ' ';
    rom[h + 0x15] = 0x20; rom[h + 0x16] = 0; rom[h + 0x17] = 5; rom[h + 0x18] = 0; rom[h + 0x19] = 1;
    auto vec = [&](int addr, int target) { rom[addr - 0x8000] = target & 0xFF; rom[addr - 0x7FFF] = target >> 8; };
    vec(0xFFEA, a.lab["nmi"]); vec(0xFFEE, a.lab["irq"]); vec(0xFFE6, a.lab["irq"]); vec(0xFFE4, a.lab["irq"]);
    vec(0xFFFC, a.lab["reset"]); vec(0xFFFE, a.lab["irq"]); vec(0xFFFA, a.lab["irq"]);
    rom[h + 0x1C] = 0xFF; rom[h + 0x1D] = 0xFF; rom[h + 0x1E] = 0; rom[h + 0x1F] = 0;
    u32 ck = 0; for (u8 b : rom) ck += b; ck &= 0xFFFF;
    int nck = ck ^ 0xFFFF;
    rom[h + 0x1C] = nck & 0xFF; rom[h + 0x1D] = nck >> 8; rom[h + 0x1E] = ck & 0xFF; rom[h + 0x1F] = ck >> 8;
    return rom;
}

// ===================================================================== image / audio writers
static u32 adler32(const u8* p, size_t n) { u32 a = 1, b = 0; for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; } return (b << 16) | a; }
static std::vector<u8> zlib_store(const std::vector<u8>& raw) {
    std::vector<u8> z = {0x78, 0x01};
    size_t i = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - i);
        bool last = i + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back((n >> 8) & 0xFF); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
        i += n;
    } while (i < raw.size());
    u32 ad = adler32(raw.data(), raw.size());
    z.push_back(ad >> 24); z.push_back((ad >> 16) & 0xFF); z.push_back((ad >> 8) & 0xFF); z.push_back(ad & 0xFF);
    return z;
}
static void write_png(const std::string& path, const u8* rgb, int w, int h) {
    std::vector<u8> raw; raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3); }
    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto be32 = [&](std::vector<u8>& v, u32 x) { v.push_back(x >> 24); v.push_back((x >> 16) & 0xFF); v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF); };
    auto chunk = [&](const char* type, const std::vector<u8>& d) {
        be32(out, (u32)d.size());
        std::vector<u8> td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
        out.insert(out.end(), td.begin(), td.end());
        be32(out, crc32_buf(td.data(), td.size()));
    };
    std::vector<u8> ihdr; be32(ihdr, w); be32(ihdr, h); ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr); chunk("IDAT", zlib_store(raw)); chunk("IEND", {});
    write_file(path, out);
}
static void write_wav(const std::string& path, const std::vector<int16_t>& pcm, int rate = 32000) {
    std::vector<u8> o;
    auto le32 = [&](u32 x) { for (int i = 0; i < 4; i++) o.push_back((x >> (8 * i)) & 0xFF); };
    auto le16 = [&](u32 x) { o.push_back(x & 0xFF); o.push_back((x >> 8) & 0xFF); };
    u32 data = (u32)pcm.size() * 2;
    o.insert(o.end(), {'R', 'I', 'F', 'F'}); le32(36 + data); o.insert(o.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    le32(16); le16(1); le16(2); le32(rate); le32(rate * 4); le16(4); le16(16);
    o.insert(o.end(), {'d', 'a', 't', 'a'}); le32(data);
    for (int16_t s : pcm) le16((u16)s);
    write_file(path, o);
}

// ===================================================================== inflate (RFC 1951) for .zip ROMs
struct Inflate {
    const u8* in; size_t inlen, pos = 0; u32 bitbuf = 0; int bitcnt = 0; std::vector<u8> out;
    struct Huff { short count[16]; short sym[288]; };
    Inflate(const u8* p, size_t n) : in(p), inlen(n) {}
    int bits(int need) {
        u32 val = bitbuf;
        while (bitcnt < need) {
            if (pos >= inlen) throw std::runtime_error("inflate: input exhausted");
            val |= (u32)in[pos++] << bitcnt; bitcnt += 8;
        }
        bitbuf = val >> need; bitcnt -= need;
        return (int)(val & ((1u << need) - 1));
    }
    void stored() {
        bitbuf = 0; bitcnt = 0;
        if (pos + 4 > inlen) throw std::runtime_error("inflate: bad stored block");
        unsigned len = in[pos] | (in[pos + 1] << 8); unsigned nlen = in[pos + 2] | (in[pos + 3] << 8); pos += 4;
        if (len != (~nlen & 0xFFFF)) throw std::runtime_error("inflate: stored length mismatch");
        if (pos + len > inlen) throw std::runtime_error("inflate: stored block truncated");
        out.insert(out.end(), in + pos, in + pos + len); pos += len;
    }
    static void build(Huff& h, const short* length, int n) {
        for (int i = 0; i < 16; i++) h.count[i] = 0;
        for (int s = 0; s < n; s++) h.count[length[s]]++;
        short offs[16]; offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; s++) if (length[s] != 0) h.sym[offs[length[s]]++] = (short)s;
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= bits(1);
            int count = h.count[len];
            if (code - count < first) return h.sym[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        throw std::runtime_error("inflate: bad code");
    }
    void codes(const Huff& lencode, const Huff& distcode) {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lencode);
            if (sym < 256) out.push_back((u8)sym);
            else if (sym == 256) return;
            else {
                sym -= 257;
                if (sym >= 29) throw std::runtime_error("inflate: bad length");
                int len = lbase[sym] + bits(lext[sym]);
                int ds = decode(distcode);
                if (ds >= 30) throw std::runtime_error("inflate: bad distance");
                size_t dist = dbase[ds] + bits(dext[ds]);
                if (dist > out.size()) throw std::runtime_error("inflate: distance too far");
                size_t from = out.size() - dist;
                for (int i = 0; i < len; i++) out.push_back(out[from + i]);
            }
        }
    }
    void fixed() {
        static Huff lc, dc; static bool init = false;
        if (!init) {
            short lengths[288]; int s = 0;
            for (; s < 144; s++) lengths[s] = 8;
            for (; s < 256; s++) lengths[s] = 9;
            for (; s < 280; s++) lengths[s] = 7;
            for (; s < 288; s++) lengths[s] = 8;
            build(lc, lengths, 288);
            for (s = 0; s < 30; s++) lengths[s] = 5;
            build(dc, lengths, 30);
            init = true;
        }
        codes(lc, dc);
    }
    void dynamic() {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw std::runtime_error("inflate: bad counts");
        short lengths[320] = {0};
        for (int i = 0; i < ncode; i++) lengths[order[i]] = (short)bits(3);
        Huff lc, dc; build(lc, lengths, 19);
        int index = 0;
        while (index < nlen + ndist) {
            int sym = decode(lc);
            if (sym < 16) lengths[index++] = (short)sym;
            else {
                int len = 0, rep;
                if (sym == 16) { if (index == 0) throw std::runtime_error("inflate: repeat with no first"); len = lengths[index - 1]; rep = 3 + bits(2); }
                else if (sym == 17) rep = 3 + bits(3);
                else rep = 11 + bits(7);
                if (index + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                while (rep--) lengths[index++] = (short)len;
            }
        }
        build(lc, lengths, nlen); build(dc, lengths + nlen, ndist);
        codes(lc, dc);
    }
    std::vector<u8> run() {
        int last;
        do {
            last = bits(1); int type = bits(2);
            if (type == 0) stored(); else if (type == 1) fixed(); else if (type == 2) dynamic();
            else throw std::runtime_error("inflate: bad block type");
        } while (!last);
        return std::move(out);
    }
};

// Return the first SNES ROM inside a .zip archive (stored or deflate).
static std::vector<u8> unzip_first_rom(const std::vector<u8>& z, std::string* name_out = nullptr) {
    auto r16 = [&](size_t p) -> u32 { return z[p] | (z[p + 1] << 8); };
    auto r32 = [&](size_t p) -> u32 { return z[p] | (z[p + 1] << 8) | (z[p + 2] << 16) | ((u32)z[p + 3] << 24); };
    if (z.size() < 22) throw std::runtime_error("not a zip file");
    size_t eocd = std::string::npos;
    for (size_t p = z.size() - 22 + 1; p-- > 0 && z.size() - p < 22 + 65536;) if (r32(p) == 0x06054b50) { eocd = p; break; }
    if (eocd == std::string::npos) throw std::runtime_error("zip: no central directory");
    u32 n = r16(eocd + 10), cd = r32(eocd + 16);
    size_t p = cd;
    for (u32 i = 0; i < n; i++) {
        if (p + 46 > z.size() || r32(p) != 0x02014b50) throw std::runtime_error("zip: bad central directory");
        u32 method = r16(p + 10), csize = r32(p + 20), usize = r32(p + 24);
        u32 nl = r16(p + 28), xl = r16(p + 30), cl = r16(p + 32), lho = r32(p + 42);
        std::string name((const char*)&z[p + 46], nl);
        p += 46 + nl + xl + cl;
        if (!is_rom_name(name)) continue;
        if (lho + 30 > z.size() || r32(lho) != 0x04034b50) throw std::runtime_error("zip: bad local header");
        size_t data = lho + 30 + r16(lho + 26) + r16(lho + 28);
        if (data + csize > z.size()) throw std::runtime_error("zip: truncated");
        if (name_out) *name_out = name;
        if (method == 0) return std::vector<u8>(z.begin() + data, z.begin() + data + csize);
        if (method == 8) { Inflate inf(&z[data], csize); auto o = inf.run(); if (usize && o.size() != usize) throw std::runtime_error("zip: size mismatch"); return o; }
        throw std::runtime_error("zip: unsupported compression method " + std::to_string(method));
    }
    throw std::runtime_error("no SNES ROM inside zip");
}

static std::vector<u8> load_rom_bytes(const std::string& path) {
    std::vector<u8> d = read_file(path);
    if (ends_with(lower(path), ".zip")) return unzip_first_rom(d);
    return d;
}

// ===================================================================== config / library
static std::string home_dir() {
    const char* h = getenv("HOME");
#if defined(_WIN32)
    if (!h) h = getenv("USERPROFILE");
#endif
    return h ? h : ".";
}
static std::string CONFIG_PATH() { return (fs::path(home_dir()) / ".virtualsnes.json").string(); }
static std::string json_escape(const std::string& s) {
    std::string o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else o += c; } return o;
}
// flat string key/value config in ~/.virtualsnes.json (all keys preserved on write)
static std::map<std::string, std::string> config_load_all() {
    std::map<std::string, std::string> m;
    for (std::string path : {(fs::path(home_dir()) / ".acsnes.json").string(), CONFIG_PATH()}) {
        std::ifstream f(path); if (!f) continue;
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t i = 0;
        auto str = [&](std::string& out) -> bool {
            while (i < s.size() && s[i] != '"') { if (s[i] == '}') return false; i++; }
            if (i >= s.size()) return false;
            i++; out.clear();
            while (i < s.size() && s[i] != '"') { if (s[i] == '\\' && i + 1 < s.size()) { i++; out += s[i] == 'n' ? '\n' : s[i]; } else out += s[i]; i++; }
            i++; return true;
        };
        for (;;) {
            std::string k, v;
            if (!str(k)) break;
            while (i < s.size() && (s[i] == ' ' || s[i] == ':' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
            if (i < s.size() && s[i] == '"') { if (!str(v)) break; }
            else { size_t st = i; while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != '\n') i++; v = s.substr(st, i - st);
                   while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back(); }
            m[k] = v;
        }
    }
    return m;
}
static void config_save_all(const std::map<std::string, std::string>& m) {
    std::ofstream f(CONFIG_PATH());
    if (!f) return;
    f << "{\n"; size_t n = 0;
    for (auto& kv : m) f << "  \"" << json_escape(kv.first) << "\": \"" << json_escape(kv.second) << "\"" << (++n < m.size() ? ",\n" : "\n");
    f << "}\n";
}
static std::string config_get(const std::string& key) { auto m = config_load_all(); auto it = m.find(key); return it == m.end() ? "" : it->second; }
static void config_set(const std::string& key, const std::string& val) { auto m = config_load_all(); m[key] = val; config_save_all(m); }

static std::string exe_dir;
static std::vector<std::string> default_rom_dirs() {
    std::string home = home_dir(); std::vector<std::string> dirs;
    const char* envs[] = {"VIRTUALSNES_ROMS", "CATSNES_ROMS", "ACSNES_ROMS"};
    std::string env; for (auto e : envs) if (getenv(e)) { env = getenv(e); break; }
    for (std::string d : {config_get("roms_dir"), env, (fs::path(exe_dir) / "roms").string(),
                          (fs::path(home) / "ROMs" / "SNES").string(), (fs::path(home) / "Roms" / "SNES").string(),
                          (fs::path(home) / "SNES").string(), (fs::path(home) / "Games" / "SNES").string()}) {
        std::error_code ec;
        if (!d.empty() && fs::is_directory(d, ec) && std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
    }
    return dirs;
}
struct LibEntry { std::string path, file, title, mapper, speed, chip_name, compat, region_name; int sram_size; u64 size; };
static bool probe_rom_file(const std::string& path, LibEntry& e) {
    try {
        std::vector<u8> data;
        if (ends_with(lower(path), ".zip")) data = unzip_first_rom(read_file(path));
        else {
            std::ifstream f(path, std::ios::binary); if (!f) return false;
            data.resize(0x410000 + 0x40); f.read((char*)data.data(), (std::streamsize)data.size()); data.resize((size_t)f.gcount());
        }
        if (data.size() > 0x410040) data.resize(0x410040);
        RomInfo i = analyze_rom(data);
        e.path = path; e.file = fs::path(path).filename().string(); e.title = i.title; e.mapper = i.mapper; e.speed = i.speed;
        e.chip_name = i.chip_name; e.compat = i.compat; e.region_name = i.region_name; e.sram_size = i.sram_size;
        e.size = fs::file_size(path);
        return true;
    } catch (...) { return false; }
}
static std::vector<LibEntry> scan_rom_library(size_t max_files = 2000) {
    std::vector<LibEntry> entries; std::vector<std::string> seen;
    for (auto& root : default_rom_dirs()) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string name = it->path().filename().string();
            if (!(is_rom_name(name) || ends_with(lower(name), ".zip"))) continue;
            std::string ap = fs::absolute(it->path(), ec).string();
            if (std::find(seen.begin(), seen.end(), ap) != seen.end()) continue;
            seen.push_back(ap);
            LibEntry e; if (probe_rom_file(it->path().string(), e)) entries.push_back(e);
            if (entries.size() >= max_files) break;
        }
        if (entries.size() >= max_files) break;
    }
    auto ord = [](const std::string& c) { return c == "ok" ? 0 : c == "unknown" ? 1 : c == "chip" ? 2 : 9; };
    std::sort(entries.begin(), entries.end(), [&](const LibEntry& a, const LibEntry& b) {
        if (ord(a.compat) != ord(b.compat)) return ord(a.compat) < ord(b.compat);
        if (lower(a.title) != lower(b.title)) return lower(a.title) < lower(b.title);
        return lower(a.file) < lower(b.file);
    });
    return entries;
}
static void library_summary(const std::vector<LibEntry>& e, int& ok, int& chip, int& unk) {
    ok = chip = unk = 0;
    for (auto& x : e) { if (x.compat == "ok") ok++; else if (x.compat == "chip") chip++; else if (x.compat == "unknown") unk++; }
}

// ===================================================================== minimal JSON (SingleStepTests)
struct JV {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    double num = 0; std::string str; std::vector<JV> arr; std::vector<std::pair<std::string, JV>> obj;
    const JV* get(const std::string& k) const { for (auto& kv : obj) if (kv.first == k) return &kv.second; return nullptr; }
    i64 i(const std::string& k) const { const JV* v = get(k); return v ? (i64)v->num : 0; }
};
struct JParser {
    const std::string& s; size_t p;
    JParser(const std::string& str, size_t pos) : s(str), p(pos) {}
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t' || s[p] == ',')) p++; }
    bool value(JV& v) {
        while (p < s.size() && isspace((unsigned char)s[p])) p++;
        if (p >= s.size()) return false;
        char c = s[p];
        if (c == '{') {
            v.t = JV::OBJ; p++;
            for (;;) {
                ws(); if (p >= s.size()) return false;
                if (s[p] == '}') { p++; return true; }
                JV key; if (!value(key) || key.t != JV::STR) return false;
                while (p < s.size() && (isspace((unsigned char)s[p]) || s[p] == ':')) p++;
                JV val; if (!value(val)) return false;
                v.obj.emplace_back(key.str, std::move(val));
            }
        }
        if (c == '[') {
            v.t = JV::ARR; p++;
            for (;;) {
                ws(); if (p >= s.size()) return false;
                if (s[p] == ']') { p++; return true; }
                JV el; if (!value(el)) return false;
                v.arr.push_back(std::move(el));
            }
        }
        if (c == '"') {
            v.t = JV::STR; p++;
            while (p < s.size() && s[p] != '"') { if (s[p] == '\\') p++; if (p < s.size()) v.str += s[p++]; }
            if (p >= s.size()) return false;
            p++; return true;
        }
        if (c == 't' || c == 'f' || c == 'n') {
            v.t = c == 'n' ? JV::NUL : JV::BOOL; v.num = c == 't';
            while (p < s.size() && isalpha((unsigned char)s[p])) p++;
            return true;
        }
        size_t st = p;
        while (p < s.size() && (isdigit((unsigned char)s[p]) || s[p] == '-' || s[p] == '+' || s[p] == '.' || s[p] == 'e' || s[p] == 'E')) p++;
        if (st == p) return false;
        v.t = JV::NUM; v.num = strtod(s.substr(st, p - st).c_str(), nullptr);
        return true;
    }
};

struct FlatBus : Bus {
    std::vector<u8> m; FlatBus() : m(1 << 24, 0) {}
    u8 read(u32 a) override { return m[a & 0xFFFFFF]; }
    void write(u32 a, u8 v) override { wc++; m[a & 0xFFFFFF] = v; }
};
struct FlatSpcBus : SPCBus {
    u8 m[0x10000]; FlatSpcBus() { memset(m, 0, sizeof m); }
    u8 read(int a) override { return m[a & 0xFFFF]; }
    void write(int a, int v) override { m[a & 0xFFFF] = (u8)v; }
};

static int cputest(const std::string& dir, int per_file) {
    std::vector<std::string> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) if (ends_with(e.path().string(), ".json")) files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    if (files.empty()) { fprintf(stderr, "no *.json test files in %s\n", dir.c_str()); return 2; }
    std::unique_ptr<FlatBus> bus(new FlatBus()); CPU65816 c(bus.get());
    std::unique_ptr<FlatSpcBus> sbus(new FlatSpcBus()); SPC700 sc(sbus.get());
    std::map<std::string, int> fails; std::map<std::string, std::string> first; u64 total = 0;
    for (auto& path : files) {
        std::string tag = fs::path(path).filename().string(); tag = tag.substr(0, tag.size() - 5);
        std::ifstream f(path, std::ios::binary);
        std::error_code fec; u64 fsz = fs::file_size(path, fec);
        size_t want = (size_t)std::min<u64>(fec ? (u64)1 << 40 : fsz, (u64)per_file * 24576 + 65536);
        std::string txt(want, '\0'); f.read(&txt[0], (std::streamsize)want); txt.resize((size_t)f.gcount());
        size_t start = txt.find('[');
        if (start == std::string::npos) continue;
        std::vector<JV> tests;
        for (int pass = 0; pass < 2; pass++) {
            tests.clear();
            JParser jp(txt, start + 1);
            for (int n = 0; n < per_file; n++) {
                jp.ws();
                JV t; if (!jp.value(t) || t.t != JV::OBJ) break;
                tests.push_back(std::move(t));
            }
            if ((int)tests.size() >= per_file || fec || txt.size() >= fsz) break;
            f.clear(); f.seekg(0); txt.assign((size_t)fsz, '\0'); f.read(&txt[0], (std::streamsize)fsz); txt.resize((size_t)f.gcount());
        }
        for (JV& t : tests) {
            const JV* I = t.get("initial"); const JV* F = t.get("final");
            if (!I || !F) continue;
            std::string name = t.get("name") ? t.get("name")->str : "?";
            std::vector<std::string> bad;
            const JV* iram = I->get("ram"); const JV* fram = F->get("ram");
            if (I->get("psw")) {
                if (iram) for (auto& e : iram->arr) sbus->m[(int)e.arr[0].num & 0xFFFF] = (u8)e.arr[1].num;
                sc.PC = (int)I->i("pc"); sc.A = (int)I->i("a"); sc.X = (int)I->i("x"); sc.Y = (int)I->i("y"); sc.SP = (int)I->i("sp"); sc.PSW = (int)I->i("psw");
                sc.stopped = false; sc.trap = 0x10000; sc.cyc = 0; sc.run(1);
                struct { const char* k; int v; } sregs[] = {{"pc", sc.PC}, {"a", sc.A}, {"x", sc.X}, {"y", sc.Y}, {"sp", sc.SP}, {"psw", sc.PSW}};
                for (auto& r : sregs) if (r.v != F->i(r.k)) bad.push_back(r.k);
                if (fram) for (auto& e : fram->arr) if (sbus->m[(int)e.arr[0].num & 0xFFFF] != (int)e.arr[1].num) { char b[32]; snprintf(b, 32, "aram$%04X", (int)e.arr[0].num); bad.push_back(b); }
                total++;
                std::string k = "spc " + tag;
                if (!bad.empty()) { fails[k]++; if (!first.count(k)) { std::string s = name + " "; for (size_t q = 0; q < bad.size() && q < 4; q++) s += bad[q] + " "; first[k] = s; } }
                if (iram) for (auto& e : iram->arr) sbus->m[(int)e.arr[0].num & 0xFFFF] = 0;
                if (fram) for (auto& e : fram->arr) sbus->m[(int)e.arr[0].num & 0xFFFF] = 0;
                continue;
            }
            if (iram) for (auto& e : iram->arr) bus->m[(u32)e.arr[0].num & 0xFFFFFF] = (u8)e.arr[1].num;
            c.PC = (int)I->i("pc"); c.S = (int)I->i("s"); c.P = (int)I->i("p"); c.A = (int)I->i("a"); c.X = (int)I->i("x"); c.Y = (int)I->i("y");
            c.DB = (int)I->i("dbr"); c.D = (int)I->i("d"); c.PB = (int)I->i("pbr"); c.E = (int)I->i("e");
            if (c.E) { c.S = 0x100 | (c.S & 0xFF); c.P |= 0x30; }
            if (c.P & 0x10) { c.X &= 0xFF; c.Y &= 0xFF; }
            c.waiting = c.stopped = c.nmi_pending = c.irq_line = false; c.ik_valid = false;
            c.cyc = 0; c.run(1);
            struct { const char* k; int v; } regs[] = {{"pc", c.PC}, {"s", c.S}, {"p", c.P}, {"a", c.A}, {"x", c.X}, {"y", c.Y},
                                                      {"dbr", c.DB}, {"d", c.D}, {"pbr", c.PB}, {"e", c.E}};
            for (auto& r : regs) if (r.v != F->i(r.k)) bad.push_back(r.k);
            if (fram) for (auto& e : fram->arr) if (bus->m[(u32)e.arr[0].num & 0xFFFFFF] != (int)e.arr[1].num) { char b[32]; snprintf(b, 32, "ram$%06X", (u32)e.arr[0].num); bad.push_back(b); }
            total++;
            if (!bad.empty()) { fails[tag]++; if (!first.count(tag)) { std::string s = name + " "; for (size_t q = 0; q < bad.size() && q < 4; q++) s += bad[q] + " "; first[tag] = s; } }
            if (iram) for (auto& e : iram->arr) bus->m[(u32)e.arr[0].num & 0xFFFFFF] = 0;
            if (fram) for (auto& e : fram->arr) bus->m[(u32)e.arr[0].num & 0xFFFFFF] = 0;
        }
    }
    int nf = 0; for (auto& kv : fails) nf += kv.second;
    printf("%s CPU self-test: %llu tests from %zu files, %d failing\n", APP, (unsigned long long)total, files.size(), nf);
    for (auto& kv : fails) printf("  %s: %d failing, first %s\n", kv.first.c_str(), kv.second, first[kv.first].c_str());
    return fails.empty() ? 0 : 1;
}

// ===================================================================== machine interface
struct Machine {
    virtual ~Machine() {}
    virtual RomInfo load(const std::vector<u8>& data, const std::string& path) = 0;
    virtual void run_frame() = 0;
    virtual std::vector<int16_t> take_audio() = 0;       // 32 kHz stereo interleaved
    virtual void reset() = 0;
    virtual std::vector<u8> save_state() = 0;
    virtual void load_state(const std::vector<u8>& d) = 0;
    virtual void save_sram() = 0;
    virtual const u8* frame(int& w, int& h) = 0;
    virtual void set_joy(int j1, int j2) = 0;
    virtual void set_skip_render(bool s) = 0;
    virtual double fps() { return 60.0988; }
    virtual std::string title() = 0;
    virtual std::string mapper() = 0;
    virtual std::string speed() = 0;
    virtual std::string chip_name() = 0;
    virtual std::string compat() = 0;
    virtual std::string rom_path() = 0;
    virtual std::string core_name() = 0;
    virtual std::string dsp_name() = 0;
    virtual std::string state_ext(int slot) = 0;
    virtual SNES* snes() { return nullptr; }
    virtual void close() {}
};

struct CoreMachine : Machine {
    std::unique_ptr<SNES> s;
    CoreMachine() : s(new SNES()) {}
    RomInfo load(const std::vector<u8>& d, const std::string& p) override { return s->load(d, p); }
    void run_frame() override { s->run_frame(); }
    std::vector<int16_t> take_audio() override { return s->apu.dspc.take(); }
    void reset() override { s->reset(); }
    std::vector<u8> save_state() override { return s->save_state(); }
    void load_state(const std::vector<u8>& d) override { s->load_state(d); }
    void save_sram() override { s->save_sram(); }
    const u8* frame(int& w, int& h) override { w = s->frame_w; h = s->frame_h; return s->frame.data(); }
    void set_joy(int j1, int j2) override { s->joy1 = j1; s->joy2 = j2; }
    void set_skip_render(bool v) override { s->skip_render = v; }
    std::string title() override { return s->title; }
    std::string mapper() override { return s->mapper; }
    std::string speed() override { return s->speed; }
    std::string chip_name() override { return s->chip_name; }
    std::string compat() override { return s->compat; }
    std::string rom_path() override { return s->rom_path; }
    std::string core_name() override { return "custom C++ core"; }
    std::string dsp_name() override { return "snes9x"; }
    std::string state_ext(int slot) override { char b[16]; snprintf(b, sizeof b, ".vs%02d", slot); return b; }
    SNES* snes() override { return s.get(); }
};

// ===================================================================== native SNES9x (libretro) host
// Optional: a trusted snes9x_libretro shared library placed next to the executable, or --core PATH.
// SNES9x remains credited to its authors; its license accompanies the separately installed core.
// Native cores execute machine code: only select a trusted local build. Nothing is downloaded.
struct retro_system_info { const char* library_name; const char* library_version; const char* valid_extensions; bool need_fullpath; bool block_extract; };
struct retro_game_info { const char* path; const void* data; size_t size; const char* meta; };
struct retro_game_geometry { unsigned base_width, base_height, max_width, max_height; float aspect_ratio; };
struct retro_system_timing { double fps, sample_rate; };
struct retro_system_av_info { retro_game_geometry geometry; retro_system_timing timing; };
struct retro_variable { const char* key; const char* value; };

struct NativeSNES;
static NativeSNES* g_native = nullptr;

struct NativeSNES : Machine {
    void* lib = nullptr;
    std::string core, m_title, m_rom_path;
    bool loaded = false, fullpath = false, m_skip = false;
    int joy1 = 0, joy2 = 0, pixel_format = 0;
    double m_fps = 60.0988, audio_rate = 32040.0;
    std::vector<u8> m_frame; int fw = 256, fh = 224;
    std::vector<int16_t> chunks;
    i64 source_total = 0, output_total = 0; bool have_prev = false; int16_t prev[2] = {0, 0};
    std::string err;
    std::vector<u8> rom_buf; std::string path_buf;
    std::string system_dir, save_dir;
    std::map<std::string, std::string> variables;
    // function pointers
    unsigned (*api_version)() = nullptr;
    void (*get_system_info)(retro_system_info*) = nullptr;
    void (*r_init)() = nullptr; void (*r_deinit)() = nullptr; void (*r_run)() = nullptr; void (*r_reset)() = nullptr;
    void (*unload_game)() = nullptr; bool (*load_game)(const retro_game_info*) = nullptr;
    void (*get_av)(retro_system_av_info*) = nullptr;
    size_t (*ser_size)() = nullptr; bool (*ser)(void*, size_t) = nullptr; bool (*unser)(const void*, size_t) = nullptr;
    size_t (*mem_size)(unsigned) = nullptr; void* (*mem_data)(unsigned) = nullptr;

    void* sym(const char* n) {
#if defined(_WIN32)
        void* p = (void*)GetProcAddress((HMODULE)lib, n);
#else
        void* p = dlsym(lib, n);
#endif
        if (!p) throw std::runtime_error(std::string("libretro symbol missing: ") + n);
        return p;
    }
    explicit NativeSNES(const std::string& path) : core(path) {
#if defined(_WIN32)
        lib = (void*)LoadLibraryA(fs::absolute(path).string().c_str());
#else
        lib = dlopen(fs::absolute(path).string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
#if defined(_WIN32)
        if (!lib) throw std::runtime_error("cannot load core " + path + " (error " + std::to_string((unsigned long)GetLastError()) + ")");
#else
        if (!lib) { const char* e = dlerror(); throw std::runtime_error("cannot load core " + path + (e ? std::string(": ") + e : std::string())); }
#endif
        g_native = this;
        m_frame.assign(256 * 224 * 3, 0);
        system_dir = (fs::path(home_dir()) / ".virtualsnes" / "system").string();
        save_dir = (fs::path(home_dir()) / ".virtualsnes" / "saves").string();
        std::error_code ec; fs::create_directories(system_dir, ec); fs::create_directories(save_dir, ec);
        api_version = (unsigned (*)())sym("retro_api_version");
        if (api_version() != 1) throw std::runtime_error("Unsupported libretro API");
        get_system_info = (void (*)(retro_system_info*))sym("retro_get_system_info");
        retro_system_info info{}; get_system_info(&info);
        std::string name = info.library_name ? info.library_name : "";
        if (lower(name).find("snes9x") == std::string::npos) throw std::runtime_error("The selected library is not a SNES9x libretro core");
        core_label = name + (info.library_version ? std::string(" ") + info.library_version : "");
        fullpath = info.need_fullpath;
        ((void (*)(bool (*)(unsigned, void*)))sym("retro_set_environment"))(&cb_env);
        ((void (*)(void (*)(const void*, unsigned, unsigned, size_t)))sym("retro_set_video_refresh"))(&cb_video);
        ((void (*)(void (*)(int16_t, int16_t)))sym("retro_set_audio_sample"))(&cb_sample);
        ((void (*)(size_t (*)(const int16_t*, size_t)))sym("retro_set_audio_sample_batch"))(&cb_batch);
        ((void (*)(void (*)()))sym("retro_set_input_poll"))(&cb_poll);
        ((void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned)))sym("retro_set_input_state"))(&cb_input);
        r_init = (void (*)())sym("retro_init"); r_deinit = (void (*)())sym("retro_deinit");
        r_run = (void (*)())sym("retro_run"); r_reset = (void (*)())sym("retro_reset");
        unload_game = (void (*)())sym("retro_unload_game");
        load_game = (bool (*)(const retro_game_info*))sym("retro_load_game");
        get_av = (void (*)(retro_system_av_info*))sym("retro_get_system_av_info");
        ser_size = (size_t (*)())sym("retro_serialize_size");
        ser = (bool (*)(void*, size_t))sym("retro_serialize");
        unser = (bool (*)(const void*, size_t))sym("retro_unserialize");
        mem_size = (size_t (*)(unsigned))sym("retro_get_memory_size");
        mem_data = (void* (*)(unsigned))sym("retro_get_memory_data");
        r_init();
    }
    std::string core_label;
    static bool cb_env(unsigned cmd, void* data) {
        NativeSNES* s = g_native; if (!s) return false;
        if (getenv("VSNES_ENVDEBUG")) fprintf(stderr, "[env %u]\n", cmd);
        switch (cmd) {
        case 10: { int v = *(int*)data; if (v < 0 || v > 2) return false; s->pixel_format = v; return true; }
        case 9: case 30: *(const char**)data = s->system_dir.c_str(); return true;
        case 31: *(const char**)data = s->save_dir.c_str(); return true;
        case 3: case 18: case 17: *(bool*)data = false; return true;
        case 16: {
            retro_variable* vars = (retro_variable*)data;
            for (int i = 0; vars[i].key; i++) {
                std::string v = vars[i].value ? vars[i].value : "";
                size_t semi = v.find(';'); std::string rest = semi == std::string::npos ? v : v.substr(semi + 1);
                size_t a = rest.find_first_not_of(' '); rest = a == std::string::npos ? "" : rest.substr(a);
                size_t bar = rest.find('|'); s->variables[vars[i].key] = bar == std::string::npos ? rest : rest.substr(0, bar);
            }
            return true;
        }
        case 15: {
            retro_variable* var = (retro_variable*)data;
            auto it = s->variables.find(var->key ? var->key : "");
            if (it == s->variables.end()) return false;
            var->value = it->second.c_str(); return true;
        }
        case 39: *(unsigned*)data = 0; return true;
        case 32: { auto* av = (retro_system_av_info*)data; s->m_fps = av->timing.fps; s->audio_rate = av->timing.sample_rate; return true; }
        case 1: case 11: case 35: return true;
        }
        return false;
    }
    static void cb_video(const void* data, unsigned width, unsigned height, size_t pitch) {
        NativeSNES* s = g_native; if (!s || !data || s->m_skip) return;
        if (!(width > 0 && width <= 2048 && height > 0 && height <= 2048)) { s->err = "Invalid native video dimensions"; return; }
        s->fw = width; s->fh = height; s->m_frame.resize((size_t)width * height * 3);
        const u8* src = (const u8*)data;
        for (unsigned y = 0; y < height; y++) {
            const u8* row = src + y * pitch; u8* o = &s->m_frame[(size_t)y * width * 3];
            for (unsigned x = 0; x < width; x++) {
                int r, g, b;
                if (s->pixel_format == 1) { u32 p; memcpy(&p, row + x * 4, 4); r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255; }
                else {
                    u16 p; memcpy(&p, row + x * 2, 2);
                    int gb = s->pixel_format == 2 ? 6 : 5;
                    r = ((p >> (5 + gb)) & 31) * 255 / 31;
                    g = ((p >> 5) & ((1 << gb) - 1)) * 255 / ((1 << gb) - 1);
                    b = (p & 31) * 255 / 31;
                }
                o[x * 3] = (u8)r; o[x * 3 + 1] = (u8)g; o[x * 3 + 2] = (u8)b;
            }
        }
    }
    static size_t cb_batch(const int16_t* data, size_t frames) {
        if (g_native && frames) g_native->chunks.insert(g_native->chunks.end(), data, data + frames * 2);
        return frames;
    }
    static void cb_sample(int16_t l, int16_t r) { if (g_native) { g_native->chunks.push_back(l); g_native->chunks.push_back(r); } }
    static void cb_poll() {}
    static int16_t cb_input(unsigned port, unsigned device, unsigned index, unsigned button) {
        static const int bits[12] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4};
        NativeSNES* s = g_native;
        if (!s || device != 1 || index || port > 1 || button >= 12) return 0;
        return ((port == 0 ? s->joy1 : s->joy2) >> bits[button]) & 1;
    }
    RomInfo load(const std::vector<u8>& data, const std::string& path) override {
        if (loaded) { save_sram(); unload_game(); loaded = false; }
        RomInfo info = analyze_rom(data);
        rom_buf = info.data;
        rom_buf.resize(info.data.size() + 0x10000, 0);   // snes9x probes data+0xFFC0 even on 32 KB ROMs
        path_buf = path.empty() ? "" : fs::absolute(path).string();
        if (fullpath && path.empty()) throw std::runtime_error("This core needs a ROM path; supply a .sfc or .smc file");
        if (fullpath && ends_with(lower(path), ".zip")) throw std::runtime_error("This core needs an uncompressed .sfc/.smc file");
        retro_game_info g{path_buf.empty() ? nullptr : path_buf.c_str(), rom_buf.data(), info.data.size(), nullptr};
        if (!load_game(&g)) throw std::runtime_error("SNES9x could not load this ROM");
        loaded = true; m_rom_path = path; m_title = info.title;
        retro_system_av_info av{}; get_av(&av);
        m_fps = av.timing.fps; audio_rate = av.timing.sample_rate;
        if (!(m_fps >= 1 && m_fps <= 240 && audio_rate >= 8000 && audio_rate <= 192000)) throw std::runtime_error("Invalid core audio/video timing");
        chunks.clear(); source_total = output_total = 0; have_prev = false;
        restore_sram();
        return info;
    }
    void restore_sram() {
        if (m_rom_path.empty()) return;
        std::string p = strip_ext(m_rom_path) + ".srm";
        size_t sz = mem_size(0); void* ptr = mem_data(0);
        if (sz && ptr && fs::exists(p)) { auto d = read_file(p); memcpy(ptr, d.data(), std::min(sz, d.size())); }
    }
    void save_sram() override {
        if (!loaded || m_rom_path.empty()) return;
        size_t sz = mem_size(0); void* ptr = mem_data(0);
        if (sz && ptr) {
            std::string p = strip_ext(m_rom_path) + ".srm";
            write_file(p + ".tmp", std::vector<u8>((u8*)ptr, (u8*)ptr + sz));
            std::error_code ec; fs::rename(p + ".tmp", p, ec);
        }
    }
    void run_frame() override {
        r_run();
        if (!err.empty()) { std::string e = err; err.clear(); throw std::runtime_error("SNES9x video callback failed: " + e); }
    }
    std::vector<int16_t> take_audio() override {
        std::vector<int16_t> outv;
        if (chunks.empty()) return outv;
        std::vector<int16_t> pcm; pcm.swap(chunks);
        i64 n = (i64)pcm.size() / 2;
        i64 start = source_total; source_total += n;
        i64 end = (i64)((source_total - 1) * 32000 / audio_rate) + 1;
        // streaming linear resample to 32 kHz; absolute positions keep fractional phase
        std::vector<int16_t> src; i64 x0 = start;
        if (have_prev) { src.push_back(prev[0]); src.push_back(prev[1]); x0 = start - 1; }
        src.insert(src.end(), pcm.begin(), pcm.end());
        prev[0] = pcm[pcm.size() - 2]; prev[1] = pcm[pcm.size() - 1]; have_prev = true;
        i64 cnt = (i64)src.size() / 2;
        const double ratio = audio_rate / 32000.0;
        for (i64 k = output_total; k < end; k++) {
            double xa = k * ratio;                               // same arithmetic as numpy.interp
            for (int ch = 0; ch < 2; ch++) {
                double v;
                if (xa <= (double)x0) v = src[ch];
                else if (xa >= (double)(x0 + cnt - 1)) v = src[(cnt - 1) * 2 + ch];
                else {
                    i64 j = (i64)std::floor(xa) - x0;
                    double y0 = src[j * 2 + ch], y1 = src[(j + 1) * 2 + ch];
                    double slope = (y1 - y0) / 1.0;
                    v = slope * (xa - (double)(x0 + j)) + y0;
                }
                outv.push_back((int16_t)v);
            }
        }
        output_total = end;
        return outv;
    }
    void reset() override { r_reset(); chunks.clear(); source_total = output_total = 0; have_prev = false; }
    std::vector<u8> save_state() override {
        size_t sz = ser_size(); std::vector<u8> buf(sz);
        if (!sz || !ser(buf.data(), sz)) throw std::runtime_error("State save failed");
        std::vector<u8> out = {'V', 'S', 'N', 'E', 'S', '9', 1}; out.insert(out.end(), buf.begin(), buf.end());
        return out;
    }
    void load_state(const std::vector<u8>& d) override {
        static const u8 m1[7] = {'V', 'S', 'N', 'E', 'S', '9', 1}, m2[7] = {'A', 'C', 'S', 'N', '9', 'X', 1};
        if (d.size() < 7 || (memcmp(d.data(), m1, 7) && memcmp(d.data(), m2, 7))) throw std::runtime_error("This state belongs to a different core");
        if (!unser(d.data() + 7, d.size() - 7)) throw std::runtime_error("State load failed");
        chunks.clear(); source_total = output_total = 0; have_prev = false;
    }
    const u8* frame(int& w, int& h) override { w = fw; h = fh; return m_frame.data(); }
    void set_joy(int j1, int j2) override { joy1 = j1; joy2 = j2; }
    void set_skip_render(bool v) override { m_skip = v; }
    double fps() override { return m_fps; }
    std::string title() override { return m_title; }
    std::string mapper() override { return "SNES9x"; }
    std::string speed() override { return "Native"; }
    std::string chip_name() override { return "SNES9x"; }
    std::string compat() override { return "ok"; }
    std::string rom_path() override { return m_rom_path; }
    std::string core_name() override { return core_label; }
    std::string dsp_name() override { return "native SNES9x"; }
    std::string state_ext(int slot) override { char b[16]; snprintf(b, sizeof b, ".s9x%02d", slot); return b; }
    void close() override {
        try { save_sram(); } catch (...) {}
        if (loaded) { unload_game(); loaded = false; }
        if (r_deinit) { r_deinit(); r_deinit = nullptr; }
    }
    ~NativeSNES() { close(); if (g_native == this) g_native = nullptr; }
};

struct Args {
    std::string backend = "auto", core, rom, roms, shot, wav, dsp = "auto", hold, cputest;
    int scale = 0, headless = 0, per_file = 60;   // scale 0 = default 600x400 window
    bool mute = false, list_library = false;
};

static std::unique_ptr<Machine> create_system(const Args& a) {
    std::string core = a.core;
    if (core.empty()) { for (auto e : {"VIRTUALSNES_CORE", "VSNES_CORE", "ACSNES_CORE"}) if (getenv(e)) { core = getenv(e); break; } }
    if (a.backend == "python" || a.backend == "native-cpp" || a.backend == "cpp") return std::unique_ptr<Machine>(new CoreMachine());
    if (core.empty()) {
        for (auto n : {"snes9x_libretro.so", "snes9x_libretro.dylib", "snes9x_libretro.dll"}) {
            std::string c = (fs::path(exe_dir) / n).string();
            std::error_code ec; if (fs::is_regular_file(c, ec)) { core = c; break; }
        }
    }
    if (!core.empty()) return std::unique_ptr<Machine>(new NativeSNES(core));
    if (a.backend == "snes9x") throw std::runtime_error("Native SNES9x core missing. Use --core /path/to/snes9x_libretro.so (or .dylib/.dll).");
    return std::unique_ptr<Machine>(new CoreMachine());
}

static RomInfo load_into(Machine& m, const std::string& path) {
    std::vector<u8> data = load_rom_bytes(path);
    m.save_sram();
    return m.load(data, path);
}

// ===================================================================== headless
static int headless(const Args& a) {
    std::unique_ptr<Machine> m = create_system(a);
    if (!a.rom.empty()) load_into(*m, a.rom); else m->load(build_demo_rom(), "");
    if (!a.hold.empty()) m->set_joy((int)strtol(a.hold.c_str(), nullptr, 0), 0);
    std::vector<int16_t> pcm;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < a.headless; i++) {
        m->run_frame();
        std::vector<int16_t> s = m->take_audio();
        if (!a.wav.empty()) pcm.insert(pcm.end(), s.begin(), s.end());
    }
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!a.wav.empty()) { write_wav(a.wav, pcm); printf("audio -> %s\n", a.wav.c_str()); }
    double fps = a.headless / std::max(dt, 1e-9);
    printf("%s: audio engine = %s DSP\n", APP, m->dsp_name().c_str());
    printf("%s: '%s' %s/%s [%s] %d frames in %.2fs = %.1f fps (%.0f%% of SNES speed)\n", APP, m->title().c_str(),
           m->mapper().c_str(), m->speed().c_str(), m->chip_name().c_str(), a.headless, dt, fps, fps / m->fps() * 100);
    if (SNES* s = m->snes()) {
        CPU65816& c = s->cpu;
        printf("CPU A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X PC=%02X:%04X P=%02X E=%d\n", c.A, c.X, c.Y, c.S, c.D, c.DB, c.PB, c.PC, c.P, c.E);
    }
    if (!a.shot.empty()) { int w, h; const u8* f = m->frame(w, h); write_png(a.shot, f, w, h); printf("screenshot -> %s\n", a.shot.c_str()); }
    m->close();
    return 0;
}

// ===================================================================== native file pickers (no toolkit in-process)
static std::string run_capture(const std::string& cmd) {
#if defined(_WIN32)
    (void)cmd; return "";
#else
    FILE* p = popen(cmd.c_str(), "r"); if (!p) return "";
    std::string out; char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int rc = pclose(p);
    if (rc != 0) return "";
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}
static std::string shq(const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; } return o + "'"; }
static std::string ask_rom_path(const std::string& title) {
#if defined(_WIN32)
    char file[MAX_PATH] = {0};
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.lpstrTitle = title.c_str();
    ofn.lpstrFilter = "SNES ROMs\0*.sfc;*.smc;*.swc;*.fig;*.zip\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? std::string(file) : "";
#elif defined(__APPLE__)
    std::string t; for (char c : title) { if (c == '"' || c == '\\') t += '\\'; t += c; }
    return run_capture("osascript -e " + shq("set theFile to choose file with prompt \"" + t + "\"") + " -e 'POSIX path of theFile' 2>/dev/null");
#else
    std::string r = run_capture("zenity --file-selection --title=" + shq(title) +
                                " --file-filter='SNES ROMs | *.sfc *.smc *.swc *.fig *.zip' --file-filter='All files | *' 2>/dev/null");
    if (r.empty()) r = run_capture("kdialog --getopenfilename . '*.sfc *.smc *.swc *.fig *.zip' --title " + shq(title) + " 2>/dev/null");
    return r;
#endif
}

static std::string ask_save_path(const std::string& title, const std::string& suggest) {
#if defined(_WIN32)
    char file[MAX_PATH] = {0};
    strncpy(file, suggest.c_str(), MAX_PATH - 1);
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.lpstrTitle = title.c_str();
    ofn.lpstrFilter = "All files\0*.*\0"; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    return GetSaveFileNameA(&ofn) ? std::string(file) : "";
#elif defined(__APPLE__)
    std::string t; for (char c : title) { if (c == '"' || c == '\\') t += '\\'; t += c; }
    return run_capture("osascript -e " + shq("set f to choose file name with prompt \"" + t + "\" default name \"" + fs::path(suggest).filename().string() + "\"") + " -e 'POSIX path of f' 2>/dev/null");
#else
    std::string r = run_capture("zenity --file-selection --save --confirm-overwrite --title=" + shq(title) + " --filename=" + shq(suggest) + " 2>/dev/null");
    if (r.empty()) r = run_capture("kdialog --getsavefilename " + shq(suggest) + " --title " + shq(title) + " 2>/dev/null");
    return r;
#endif
}

// ===================================================================== SDL2 desktop GUI (Snes9x Win32 layout)
// The window, menus, hotkeys and dialogs follow the classic Snes9x Win32 front-end
// (menu structure taken from its snes9x.rc resource; default keys from its defaults).
// Only the layout is mirrored: no Snes9x code, art or data tables are used, and the window
// keeps the vsnes name. Features vsnes does not have (movies, AVI, netplay, cheats, MultiCart,
// SPC/memory-pack export) are shown greyed out, exactly where Snes9x has them.
#ifdef VSNES_HAVE_SDL
// UI font: Liberation Sans Regular/Bold rasterised at 11 px (4-bit alpha).
// Liberation Sans: Copyright (c) 2012 Red Hat, Inc. Licensed under the SIL Open Font License 1.1
// (https://openfontlicense.org). Reserved Font Name: "Liberation".
struct UiGlyph { int adv, x0, w; const char* px; };
static const int UI_FONT_H = 13, UI_FONT_ASC = 10;
static const UiGlyph UI_FONT_REG[95] = {
    {3, 0, 3, "000000000000000000000000000000000000000"},
    {3, 0, 3, "0000000E00E00E00D00D00C00000F0000000000"},
    {4, 0, 4, "0000000076867575656500000000000000000000000000000000"},
    {6, 0, 7, "0000000000000000804400170800AFFFFF006208000901700EFFFFC008062003508000000000000000000000000"},
    {6, 0, 6, "00000000360009CDB26836576836000BDA30004AC700361DA5363B1ACDB2003600000000000000"},
    {10, 0, 10, "000000000000000000001AA3005700741A00B000830B08300074193969901AA3B1B074000650B065001A00B07300A2006990000000000000000000000000000000"},
    {7, 0, 8, "0000000000000000009AB10002A0660001B2B20002DA20002C5A055085095B107700D9000AAB7AB1000000000000000000000000"},
    {2, 0, 2, "00006757560000000000000000"},
    {4, 0, 4, "0000000000A306800D102C004A004A002C000D10068000A30000"},
    {4, 0, 4, "0000000086000C10068002C000E000E002C006800C1086000000"},
    {4, 0, 5, "00000000000270088AA009B001845000000000000000000000000000000000000"},
    {6, 0, 6, "0000000000000000000002000029000029005BCDBA002900002900000000000000000000000000"},
    {3, 0, 3, "0000000000000000000000000000E0080090000"},
    {4, 0, 4, "0000000000000000000000006CC2000000000000000000000000"},
    {3, 0, 3, "0000000000000000000000000000F0000000000"},
    {3, 0, 4, "0000000000C0039007500B100C0048008400C000000000000000"},
    {6, 0, 6, "00000000000005CC701C00B36800687600497600596800671C00C306CC70000000000000000000"},
    {6, 0, 6, "00000000000002BB00095B00003B00003B00003B00003B00003B001CDEC7000000000000000000"},
    {6, 0, 6, "00000000000007CC903C00B40000950002C1002C3003C2000C10006ECCC7000000000000000000"},
    {6, 0, 6, "00000000000008CC904B00A50001B3009E700000A50000696A00A608CC90000000000000000000"},
    {6, 0, 6, "0000000000000009B0004BB001B3B00A32B05702B08BBCE90002B00002B0000000000000000000"},
    {6, 0, 6, "0000000000001ECCC22B00003A00004DBC901400960000595900A609CC80000000000000000000"},
    {6, 0, 6, "00000000000003CC900C20723A00005AABA06C00964900590C109604BC90000000000000000000"},
    {6, 0, 6, "0000000000005CCCD80000B2000580000C10004A0000940000D20000E000000000000000000000"},
    {6, 0, 6, "00000000000008BB904B00952C11B308CC904A00967700594B009608BB90000000000000000000"},
    {6, 0, 6, "00000000000007CB604B00B26800764B00A808CB970000861801D108CC50000000000000000000"},
    {3, 0, 3, "0000000000000F00000000000000F0000000000"},
    {3, 0, 3, "0000000000000E00000000000000E0080090000"},
    {6, 0, 6, "00000000000000000000000000017A05BA406C200006BA4000027A000000000000000000000000"},
    {6, 0, 6, "0000000000000000000000000000005CCCCA0000005CCCCA000000000000000000000000000000"},
    {6, 0, 6, "000000000000000000000000594000027B8200007D028C83594000000000000000000000000000"},
    {6, 0, 6, "00000000000007DCA04B00881100490000B4001C50007700000000009600000000000000000000"},
    {11, 0, 11, "00000000000000000000000004AAAA500007700007700481AA992B00A0A402E0820B0D002A091190D00A70B00A07B94BA200B10000000003B10002300003AAAA810000000000000"},
    {7, 0, 8, "0000000000000000001E70000078C00000C2B30003B0690009601E000ECCCD506A0004B0B40000D1000000000000000000000000"},
    {7, 0, 7, "000000000000001ECCC801D000C31D002C11ECCD801D000871D0004A1D000971ECCC90000000000000000000000"},
    {8, 0, 8, "0000000000000000019DCC500A8001C23D000000590000005A0000003D0000000B9001A3008DCC50000000000000000000000000"},
    {8, 0, 8, "00000000000000001ECCC8001D0009B01D0000D31D0000A51D0000A51D0000D21D0009A01ECCC800000000000000000000000000"},
    {7, 0, 7, "000000000000001ECCCC81D000001D000001ECCCC41D000001D000001D000001ECCCCA000000000000000000000"},
    {7, 0, 7, "000000000000001ECCCC31D000001D000001D000001ECCCC11D000001D000001D00000000000000000000000000"},
    {9, 0, 9, "000000000000000000019DCC6000B8001B403C00000005900000006A006CDB03D00003B00B80007A0008DCC700000000000000000000000000000"},
    {8, 0, 8, "00000000000000001D0000E01D0000E01D0000E01ECCCCF01D0000E01D0000E01D0000E01D0000E0000000000000000000000000"},
    {3, 0, 3, "0000000E00E00E00E00E00E00E00E0000000000"},
    {6, 0, 6, "00000000000000ADA00005A00005A00005A00005A00005A09608702CCA10000000000000000000"},
    {7, 0, 8, "00000000000000001D000C401D00B5001D0A60001D9B00001E7B50001D01D3001D003D101D0006B0000000000000000000000000"},
    {6, 0, 6, "0000000000001D00001D00001D00001D00001D00001D00001D00001ECCC9000000000000000000"},
    {9, 0, 9, "0000000000000000001F60003F41CB0008C41CB200C941C57048941C0C093941C0A3C0941C04C70941C00D2094000000000000000000000000000"},
    {8, 0, 8, "00000000000000001F5000D01CC000D01C6800D01C0C20D01C03B0D01C0095D01C001CD01C0007F0000000000000000000000000"},
    {9, 0, 9, "000000000000000000019DDC5000B8002C503C00004C06900001E06900001E03C00004B00B8002C40009DCB400000000000000000000000000000"},
    {7, 0, 7, "000000000000001ECCCA11D000881D0004A1D001A61ECCB701D000001D000001D00000000000000000000000000"},
    {9, 0, 9, "000000000000000000019DDC5000B8002C503C00004C06900001E06900001E03C00004B00B8001C40019DDB4000000C400000003CB00000000000"},
    {8, 0, 8, "00000000000000001ECCCC401D0003D01D0000E11D0005C01ECCEA101D004B001D000A601D0001D2000000000000000000000000"},
    {7, 0, 7, "0000000000000004BBC900D100850E1000006DA61000048D5000004B4C0007906CBC91000000000000000000000"},
    {7, 0, 7, "000000000000009CDECC5002D000002D000002D000002D000002D000002D000002D000000000000000000000000"},
    {8, 0, 8, "00000000000000002D0000D12D0000D12D0000D12D0000D12D0000D11E0000E00C6007A001ACC910000000000000000000000000"},
    {7, 0, 8, "0000000000000000B40000E15A0005A00D100A4009601D0003B0680000C2C2000069B000001E5000000000000000000000000000"},
    {10, 0, 11, "0000000000000000000000C3006C000C38700AC101D03B00C7505900E1393909500A4760C0D10068A20B4C0002BC007B70000C9003F300000000000000000000000000000000000"},
    {7, 0, 8, "00000000000000003C00088007903C0000B4C300002E7000004CB00001C196000A501D206A0005B0000000000000000000000000"},
    {7, 0, 8, "00000000000000006A0005B00C400D2003C078000088C000000E4000000C2000000C2000000C2000000000000000000000000000"},
    {7, 0, 7, "000000000000003CCCCF100007800003C00001C20000A600006A00002D100009ECCCC4000000000000000000000"},
    {3, 0, 3, "0000003DA3A03A03A03A03A03A03A03A03DA000"},
    {3, 0, 4, "00000000C000830048000B000B100750039000C0000000000000"},
    {3, 0, 3, "0000009D40940940940940940940940949D4000"},
    {5, 0, 6, "00000000000003D5000A2B00390750A200B0000000000000000000000000000000000000000000"},
    {6, -1, 8, "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000001AAAAAA200000000"},
    {4, 0, 4, "00002C1003800000000000000000000000000000000000000000"},
    {6, 0, 7, "00000000000000000000000000001AAB8000000B2009AAD406A00B407803E401CA87C1000000000000000000000"},
    {6, 0, 6, "0000000000004A00004A00004B9BA04E10964B00694B00694E00964B9BA0000000000000000000"},
    {6, 0, 6, "00000000000000000000000007BB503B01D07700007700003B00D107BC50000000000000000000"},
    {6, 0, 6, "00000000000000008500008509B9A54B00C57700957700954B00D509BAA5000000000000000000"},
    {6, 0, 6, "00000000000000000000000007AA903A00867DBBB67800003C009307BB80000000000000000000"},
    {3, 0, 4, "000000000AC00E009EA00E000E000E000E000E00000000000000"},
    {6, 0, 6, "00000000000000000000000009BA954B00D57700A57700954A01D50AB9950300B30ABB70000000"},
    {6, 0, 6, "0000000000003B00003B00003B9BB03E10A53B00863B00863B00863B0086000000000000000000"},
    {2, 0, 2, "000039004A4A4A4A4A4A000000"},
    {2, -1, 3, "00000003900004A04A04A04A04A04A04A2D5000"},
    {6, 0, 6, "0000000000004A00004A00004A03C14A2C104BC3004D8A004A0A604A01D2000000000000000000"},
    {2, 0, 2, "00004A4A4A4A4A4A4A4A000000"},
    {9, 0, 9, "0000000000000000000000000000000000003A9C86BC13D00E40953B00D10763A00D10763A00D10763A00D1076000000000000000000000000000"},
    {6, 0, 6, "0000000000000000000000003A9BB03E10A53B00863B00863B00863B0086000000000000000000"},
    {6, 0, 6, "00000000000000000000000007BB903B00957700697700683B00A507BB80000000000000000000"},
    {6, 0, 6, "0000000000000000000000004AABA04E10964B00694B00694E00964B9BA04A00004A0000000000"},
    {6, 0, 6, "00000000000000000000000009B9A54B00D57700957700954B00D509CAA5000085000085000000"},
    {4, 0, 4, "00000000000000003AB63E103B003B003B003B00000000000000"},
    {6, 0, 6, "0000000000000000000000001ABC606800602C95000049C04300D02BBB60000000000000000000"},
    {3, 0, 3, "0000000000B09E92C02C02C02C00CA000000000"},
    {6, 0, 6, "0000000000000000000000004A00954A00954A00954A00953C00C50AB995000000000000000000"},
    {6, 0, 6, "000000000000000000000000B300B46801D01D06800B3B30058C0000E700000000000000000000"},
    {8, -1, 9, "0000000000000000000000000000000000000D00A901C0940BB05805828918401B6565C000BA12AA0007C00D60000000000000000000000000000"},
    {6, 0, 6, "0000000000000000000000006902C00B3B3002D80003CA000C29508701C1000000000000000000"},
    {6, 0, 6, "000000000000000000000000B300A45900D00D0580095A2003AB0000C60001C0007C4000000000"},
    {6, 0, 6, "0000000000000000000000004BBCC0000C4000880004C0001D20007DBBA0000000000000000000"},
    {4, 0, 4, "0000000005C5094009300C2099000B2009300930094004C50000"},
    {3, 0, 3, "0000000D00D00D00D00D00D00D00D00D00D0000"},
    {4, 0, 4, "000000008C1009400840077001C507600840084009408B000000"},
    {6, 0, 6, "0000000000000000000000000000003BB4044008B9000000000000000000000000000000000000"},
};
static const UiGlyph UI_FONT_BOLD[95] = {
    {3, 0, 3, "000000000000000000000000000000000000000"},
    {4, 0, 4, "000000000E800D800D700C700C6000000E800E80000000000000"},
    {5, 0, 5, "00000000003F2E63F1D62F1D50000000000000000000000000000000000000000"},
    {6, 0, 6, "00000000000000C06602A0A26DDCEB0840B00A13809ECDC81A0930470C00000000000000000000"},
    {6, 0, 6, "0000000024000AEEB17E57C87D45002EEC60017CE900357D9B57BB2BEEB2002500000000000000"},
    {10, 0, 10, "000000000000000000002CC6007900882E02D100A61F0B5000883E5B9CB02CD6D5E0B5000974D0A6003C02E0B500C4009DA0000000000000000000000000000000"},
    {8, 0, 8, "0000000000000000007DD50000E36C0000E5C80002DE60001EBE33B07D08CA805E31EF5108DE98D7000000000000000000000000"},
    {3, 0, 3, "0000006F05E04D0000000000000000000000000"},
    {4, 0, 4, "0000000002E50AB01E504F206F006F004F201E500AB002E50000"},
    {4, 0, 4, "00000000AC002E500BB007E006F106F107E00BB02E50AC000000"},
    {4, 0, 5, "000000000005A00BCDD11DE404958000000000000000000000000000000000000"},
    {6, 0, 6, "000000000000000000001200005C00005C008FFFFE137C33005C00002400000000000000000000"},
    {3, 0, 3, "0000000000000000000000003F43F40930C0000"},
    {4, 0, 4, "0000000000000000000000008FF3244100000000000000000000"},
    {3, 0, 3, "0000000000000000000000003F43F4000000000"},
    {3, 0, 3, "00000006C0990C70E43F16D09A0B70000000000"},
    {6, 0, 6, "00000000000006DD702F75F36E00E78E00C97E00C96E00E71E76F305DE70000000000000000000"},
    {6, 0, 6, "00000000000007DF103A9F10006F10006F10006F10006F10017F214FFFFC000000000000000000"},
    {6, 0, 6, "00000000000009EE905E65F60000E70009E2009E500AE4005F83329FFFFA000000000000000000"},
    {6, 0, 6, "00000000000009EE906E56F60003F500CE800026E50000DA8E55E71AEE90000000000000000000"},
    {6, 0, 7, "00000000000000000DF00009DF0003D7F000C55F007A05F00CFFFFF01117F200005F00000000000000000000000"},
    {6, 0, 6, "0000000000003FFFF53F41104F10005FBEB14A65E90000BB6E55E809ED90000000000000000000"},
    {6, 0, 6, "00000000000005DE901E85E46E00008DAEB18F53E86F00CA2E74E705DE90000000000000000000"},
    {6, 0, 6, "0000000000008FFFF91334E60008C0002E50009D0000E80003F40005F200000000000000000000"},
    {6, 0, 6, "00000000000009EEA15F53E74F42E609FFB05E42E79E00CB6E42E809DEA1000000000000000000"},
    {6, 0, 6, "00000000000008DD706F56F38E00E86F44E90AEBDA0000D84E57F309ED60000000000000000000"},
    {4, 0, 4, "00000000000000000E900E90000000000E900E90000000000000"},
    {4, 0, 4, "00000000000000000E900E90000000000E900E8005700A300000"},
    {6, 0, 6, "00000000000000000000000000048C3AED958D60003AED9500038C000000000000000000000000"},
    {6, 0, 6, "0000000000000000000000000000008FFFFE1222228FFFFE000000000000000000000000000000"},
    {6, 0, 6, "0000000000000000000000007A610027CEC70002BE27CEC77A6100000000000000000000000000"},
    {7, 0, 7, "0000000000000007DEC404E74BE000009E00009D40006E1000000000009D000009D000000000000000000000000"},
    {11, 0, 11, "00000000000000000000000006BBBA40000A60001A500861AA8C0C00C0B20790B0391C00860C0571C01D34904908B95BA100C00000000006A10019400004BBBB500000000000000"},
    {8, 0, 8, "0000000000000000002FE100007DE60000C9AB0002F56E1007E01E600CFFFFB03F7228F28E1001E7000000000000000000000000"},
    {8, 0, 8, "00000000000000004FFFEC404F623CD04F400BC04FFFFE404F6239E24F4002F54F6229F34FFFEC60000000000000000000000000"},
    {8, 0, 8, "000000000000000001AEEC600CD647D34F5000007F1000007F1000004F5000000CD647D4019DEC70000000000000000000000000"},
    {8, 0, 8, "00000000000000004FFED8004F737EB04F4006F44F4002F64F4002F64F4006F34F736EB04FFFD800000000000000000000000000"},
    {7, 0, 7, "000000000000004FFFFFA4F733324F400004FFFFF54F733314F400004F733324FFFFFD000000000000000000000"},
    {7, 0, 7, "000000000000004FFFFF44F733314F400004FFFFF24F733304F400004F400004F40000000000000000000000000"},
    {9, 0, 9, "000000000000000000019DED8000CE647D504F50000007F10000007F106FFD04F50129D00CE646DC0019DEC810000000000000000000000000000"},
    {8, 0, 8, "00000000000000004F4005F34F4005F34F4005F34FFFFFF34F8448F34F4005F34F4005F34F4005F3000000000000000000000000"},
    {3, 0, 3, "0000004F44F44F44F44F44F44F44F4000000000"},
    {6, 0, 6, "00000000000000AFF50026F50003F50003F50003F50003F48D49E22BED50000000000000000000"},
    {8, 0, 8, "00000000000000004F400AD24F409E304F49E3004FCF80004FDAE3004F40CD104F402EA04F4006F6000000000000000000000000"},
    {7, 0, 7, "000000000000004F400004F400004F400004F400004F400004F400004F733314FFFFF5000000000000000000000"},
    {9, 0, 9, "0000000000000000004FE000CF64FD401EE64FA806BE64F6D0A6E64F2E2E2E64F2A9D0E64F26E80E64F21F40E6000000000000000000000000000"},
    {8, 0, 8, "00000000000000004FC003F34FE503F34FAD03F34F3E63F34F28D3F34F20E9F34F206EF34F200DF3000000000000000000000000"},
    {9, 0, 9, "000000000000000000019DEC5000CE64AF504F5000CD07F10008F07F10008F04F5000CC00CE64AF50019DEC500000000000000000000000000000"},
    {7, 0, 7, "000000000000004FFFEB24F735DB4F400AE4F734EA4FFFEA14F400004F400004F40000000000000000000000000"},
    {9, 0, 9, "000000000000000000019DEC5000BE64AF504F5000CC07F10008F07F10008E04F5000CC00CD539F50019EFD6000001EA31000005DE40000000000"},
    {8, 0, 8, "00000000000000004FFFEC504F733AE24F4004F44F733AE14FFFFD304F408E204F400DB04F4005F6000000000000000000000000"},
    {7, 0, 7, "0000000000000005CEDA12F824E74F600000CFD9400048CF800000BC6D634DA08DED91000000000000000000000"},
    {7, 0, 7, "00000000000000DFFFFF8238F531006F200006F200006F200006F200006F200006F200000000000000000000000"},
    {8, 0, 8, "00000000000000005F3004F45F3004F45F3004F45F3004F45F3004F33F5006F20DC45DC002BED910000000000000000000000000"},
    {7, 0, 8, "0000000000000000BD0007E16F200CB01E701F500AB06E1005E1AA0000E6E500009DE000004F9000000000000000000000000000"},
    {10, 0, 11, "0000000000000000000000DA00BF205F39D00EE607E05F12EA90AB02F46C6C0D700D7993E2F4009AB50E7E0006DE20CDB0002FE008F800000000000000000000000000000000000"},
    {7, 0, 8, "00000000000000005F400DA00BC08E2002E9E700007FD00000AEE20005F4DB000D904F508E100AD0000000000000000000000000"},
    {7, 0, 8, "00000000000000008E100BD01E702F5006D09C0000D8E400005FB000001F7000001F7000001F7000000000000000000000000000"},
    {7, 0, 7, "000000000000004FFFFF20333DC00007E20003E60001DA0000AD10005F73331AFFFFF6000000000000000000000"},
    {4, 0, 4, "000000005FF85F005F005F005F005F005F005F005F105FF80000"},
    {3, 0, 3, "000000C709A06D03F10E40C609906C000000000"},
    {4, 0, 4, "00000000CFF005F005F005F005F005F005F005F005F0CFF00000"},
    {6, 0, 7, "0000000000000000CE40004D7A000A71E202E10A8089003E0000000000000000000000000000000000000000000"},
    {6, -1, 8, "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000777777100000000"},
    {4, 0, 4, "7D000C7002D00000000000000000000000000000000000000000"},
    {6, 0, 7, "00000000000000000000000000001BEE8000004F401BDCF508E02F509E29E602CE69D2000000000000000000000"},
    {7, 0, 7, "000000000000003F400003F400003F8DD503FA1AE03F405F33F405F33FA1AE03F7DD50000000000000000000000"},
    {6, 0, 6, "00000000000000000000000006DEA03F62D87F00007F00003F62D906DEA1000000000000000000"},
    {7, 0, 7, "0000000000000000008E000008E008ECAE04F61DE07F108E07F108E04F62DE009EC9E0000000000000000000000"},
    {6, 0, 6, "00000000000000000000000006DD903F30D67FEEE97F10003F62D607DE90000000000000000000"},
    {4, 0, 4, "0000000008E90E90CFFA0E800E800E800E800E80000000000000"},
    {7, 0, 7, "000000000000000000000000000008EC9E04F62DE07F108E07F108E04F62DE009ECAE00000BB01AEEB200000000"},
    {7, 0, 7, "000000000000003F400003F400003F7DD503FA3BD03F407F03F407F03F407F03F407F0000000000000000000000"},
    {3, 0, 3, "0000003F40103F43F43F43F43F43F4000000000"},
    {3, -1, 4, "0000000003F4001003F403F403F403F403F403F405F32EB00000"},
    {6, 0, 7, "000000000000003F400003F400003F42D803F5C9003FDE1003F9D9003F45E303F40BC0000000000000000000000"},
    {3, 0, 3, "0000003F43F43F43F43F43F43F43F4000000000"},
    {10, 0, 10, "00000000000000000000000000000000000000003F6DD3BE703FA3ED3AE03F40CA06F13F40CA06F13F40CA06F13F40CA06F1000000000000000000000000000000"},
    {7, 0, 7, "00000000000000000000000000003F6DD503FA3BD03F407F03F407F03F407F03F407F0000000000000000000000"},
    {7, 0, 7, "000000000000000000000000000005CEB303E71AD07F105F37F105F33E71BD005CEB30000000000000000000000"},
    {7, 0, 7, "00000000000000000000000000003F7DD503FA1AE03F405F33F405F33F91AE03F8DD503F400003F400000000000"},
    {7, 0, 7, "000000000000000000000000000008EC9E04F61DE07F108E07F108E04F62DE009EC9E000008E000008E00000000"},
    {4, 0, 5, "000000000000000000003F7E13FB403F5003F4003F4003F400000000000000000"},
    {6, 0, 6, "00000000000000000000000009EEA14F20622ED940016AE62600C82BEEB1000000000000000000"},
    {4, 0, 4, "0000000000000C50CFF72F501F501F501F700BE6000000000000"},
    {7, 0, 7, "00000000000000000000000000004F208E04F208E04F208E04F309E03F83DE009EB8E0000000000000000000000"},
    {6, 0, 7, "0000000000000000000000000000BB00AD06E10E701E54F200BA9C0005ED60001EE100000000000000000000000"},
    {9, -1, 10, "00000000000000000000000000000000000000000D70AF30D609A0DC62F206D2E6A5E002F8A2E8A000DD60DD60009F20AF20000000000000000000000000000000"},
    {6, 0, 7, "00000000000000000000000000007E21E800BB9C0002EE20003EE4000C98D108D10DA0000000000000000000000"},
    {6, 0, 7, "0000000000000000000000000000AC00AD05F20E700D73F2008B8C0002EC60000CE10002DA0006EB10000000000"},
    {6, 0, 6, "0000000000000000000000005FFFC0013E8000CC0008E2003E71108FFFF1000000000000000000"},
    {4, 0, 5, "000000000003DF008E1008C000BB00CE3001CA0009C0008C0008E1002DF000000"},
    {3, 0, 3, "0000002F32F32F32F32F32F32F32F32F32F3000"},
    {4, 0, 5, "0000000000BE7000AC0008D0007E2000CF106E3008D0008D000AC00BE50000000"},
    {6, 0, 7, "000000000000000000000000000000000005ED7260534AEB0000000000000000000000000000000000000000000"},
};

struct UiFont { SDL_Texture* tex = nullptr; SDL_Rect r[95]; int adv[95], x0[95]; };

struct Gfx {
    SDL_Window* win = nullptr; SDL_Renderer* ren = nullptr; SDL_Texture* tex = nullptr; int tex_w = 0, tex_h = 0; bool tex_linear = false;
    UiFont reg, bold; int small_glyph[128][7];
    void build(UiFont& F, const UiGlyph* G) {
        int tw = 0; for (int i = 0; i < 95; i++) tw += G[i].w + 1;
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, tw, UI_FONT_H, 32, SDL_PIXELFORMAT_RGBA32);
        SDL_LockSurface(s);
        int x = 0;
        for (int i = 0; i < 95; i++) {
            const UiGlyph& g = G[i];
            for (int y = 0; y < UI_FONT_H; y++) for (int c = 0; c < g.w; c++) {
                char h = g.px[y * g.w + c]; int a = (h >= 'A' ? h - 'A' + 10 : h - '0') * 17;
                u8* p = (u8*)s->pixels + y * s->pitch + (x + c) * 4; p[0] = p[1] = p[2] = 255; p[3] = (u8)a;
            }
            F.r[i] = SDL_Rect{x, 0, g.w, UI_FONT_H}; F.adv[i] = g.adv; F.x0[i] = g.x0;
            x += g.w + 1;
        }
        SDL_UnlockSurface(s);
        F.tex = SDL_CreateTextureFromSurface(ren, s);
        SDL_SetTextureBlendMode(F.tex, SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(s);
    }
    void init_fonts() {
        build(reg, UI_FONT_REG); build(bold, UI_FONT_BOLD);
        memset(small_glyph, 0, sizeof small_glyph);
        for (auto& g : FONT) glyph_rows(g.rows, small_glyph[(int)g.ch]);
    }
    static int gi(char ch) { int c = (unsigned char)ch; return (c >= 32 && c < 127) ? c - 32 : '?' - 32; }
    int text_w(const std::string& s, bool b = false) { const UiFont& F = b ? bold : reg; int w = 0; for (char c : s) w += F.adv[gi(c)]; return w; }
    // label width ignoring '&' mnemonic markers
    int label_w(const std::string& s, bool b = false) { std::string t; for (size_t i = 0; i < s.size(); i++) { if (s[i] == '&' && i + 1 < s.size() && s[i + 1] != '&') continue; t += s[i]; } return text_w(t, b); }
    void color(SDL_Color c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); }
    void fill(int x, int y, int w, int h, SDL_Color c) { color(c); SDL_Rect r{x, y, w, h}; SDL_RenderFillRect(ren, &r); }
    void fill(const SDL_Rect& r, SDL_Color c) { fill(r.x, r.y, r.w, r.h, c); }
    void hline(int x1, int x2, int y, SDL_Color c) { color(c); SDL_RenderDrawLine(ren, x1, y, x2, y); }
    void vline(int x, int y1, int y2, SDL_Color c) { color(c); SDL_RenderDrawLine(ren, x, y1, x, y2); }
    void line(int x1, int y1, int x2, int y2, SDL_Color c) { color(c); SDL_RenderDrawLine(ren, x1, y1, x2, y2); }
    int text(int x, int y, const std::string& s, SDL_Color c, bool b = false) {
        UiFont& F = b ? bold : reg;
        SDL_SetTextureColorMod(F.tex, c.r, c.g, c.b); SDL_SetTextureAlphaMod(F.tex, c.a);
        for (char ch : s) { int i = gi(ch); SDL_Rect d{x + F.x0[i], y, F.r[i].w, UI_FONT_H}; SDL_RenderCopy(ren, F.tex, &F.r[i], &d); x += F.adv[i]; }
        return x;
    }
    // menu-style label: '&' marks the mnemonic, underlined when `ul` is set
    void label(int x, int y, const std::string& s, SDL_Color c, bool ul, bool b = false) {
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '&' && i + 1 < s.size() && s[i + 1] != '&') {
                int w = text_w(std::string(1, s[i + 1]), b);
                int nx = text(x, y, std::string(1, s[i + 1]), c, b);
                if (ul) hline(x, x + w - 2, y + UI_FONT_ASC + 1, c);
                x = nx; i++; continue;
            }
            x = text(x, y, std::string(1, s[i]), c, b);
        }
    }
    void text_clip(int x, int y, int maxw, const std::string& s, SDL_Color c, bool b = false) {
        if (text_w(s, b) <= maxw) { text(x, y, s, c, b); return; }
        std::string t = s; while (!t.empty() && text_w(t + "...", b) > maxw) t.pop_back();
        text(x, y, t + "...", c, b);
    }
    // white text with a 1 px black outline (on-screen messages)
    void osd(int x, int y, const std::string& s) {
        SDL_Color k{0, 0, 0, 255}, w{255, 255, 255, 255};
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) text(x + dx, y + dy, s, k);
        text(x, y, s, w);
    }
};

// classic Windows (2000/XP classic theme) system colours
static const SDL_Color C_FACE{212, 208, 200, 255}, C_HILITE{255, 255, 255, 255}, C_SHADOW{128, 128, 128, 255},
    C_DKSHADOW{64, 64, 64, 255}, C_TEXT{0, 0, 0, 255}, C_SEL{10, 36, 106, 255}, C_SELTEXT{255, 255, 255, 255},
    C_GRAY{128, 128, 128, 255}, C_WINDOW{255, 255, 255, 255}, C_CAP1{10, 36, 106, 255}, C_CAP2{166, 202, 240, 255},
    C_BLACK{0, 0, 0, 255}, C_FOLDER{240, 200, 80, 255};

static void raised(Gfx& g, const SDL_Rect& r, bool button = true) {
    g.fill(r, C_FACE);
    g.hline(r.x, r.x + r.w - 2, r.y, button ? C_HILITE : C_FACE); g.vline(r.x, r.y, r.y + r.h - 2, button ? C_HILITE : C_FACE);
    g.hline(r.x + 1, r.x + r.w - 3, r.y + 1, button ? C_FACE : C_HILITE); g.vline(r.x + 1, r.y + 1, r.y + r.h - 3, button ? C_FACE : C_HILITE);
    g.hline(r.x, r.x + r.w - 1, r.y + r.h - 1, C_DKSHADOW); g.vline(r.x + r.w - 1, r.y, r.y + r.h - 1, C_DKSHADOW);
    g.hline(r.x + 1, r.x + r.w - 2, r.y + r.h - 2, C_SHADOW); g.vline(r.x + r.w - 2, r.y + 1, r.y + r.h - 2, C_SHADOW);
}
static void sunken(Gfx& g, const SDL_Rect& r, SDL_Color inside = C_WINDOW) {
    g.fill(r, inside);
    g.hline(r.x, r.x + r.w - 1, r.y, C_SHADOW); g.vline(r.x, r.y, r.y + r.h - 1, C_SHADOW);
    g.hline(r.x + 1, r.x + r.w - 2, r.y + 1, C_DKSHADOW); g.vline(r.x + 1, r.y + 1, r.y + r.h - 2, C_DKSHADOW);
    g.hline(r.x, r.x + r.w - 1, r.y + r.h - 1, C_HILITE); g.vline(r.x + r.w - 1, r.y, r.y + r.h - 1, C_HILITE);
    g.hline(r.x + 1, r.x + r.w - 2, r.y + r.h - 2, C_FACE); g.vline(r.x + r.w - 2, r.y + 1, r.y + r.h - 2, C_FACE);
}
static void pressed_frame(Gfx& g, const SDL_Rect& r) {
    g.fill(r, C_FACE);
    g.hline(r.x, r.x + r.w - 1, r.y, C_DKSHADOW); g.vline(r.x, r.y, r.y + r.h - 1, C_DKSHADOW);
    g.hline(r.x + 1, r.x + r.w - 2, r.y + 1, C_SHADOW); g.vline(r.x + 1, r.y + 1, r.y + r.h - 2, C_SHADOW);
    g.hline(r.x, r.x + r.w - 1, r.y + r.h - 1, C_HILITE); g.vline(r.x + r.w - 1, r.y, r.y + r.h - 1, C_HILITE);
}
static void check_glyph(Gfx& g, int x, int y, SDL_Color c) {   // 7x7 tick
    for (int i = 0; i < 3; i++) { g.line(x, y + 2 + i, x + 2, y + 4 + i, c); g.line(x + 2, y + 4 + i, x + 6, y + i, c); }
}
static void dot_glyph(Gfx& g, int x, int y, SDL_Color c) { g.fill(x + 1, y, 4, 6, c); g.fill(x, y + 1, 6, 4, c); }
static void arrow_glyph(Gfx& g, int x, int y, SDL_Color c) { for (int i = 0; i < 4; i++) g.vline(x + i, y + i, y + 6 - i, c); }

struct AudioOut {
    SDL_AudioDeviceID dev = 0; std::string err; int rate = 32000;
    int frame_bytes = 32000 * 4 / 60, high = 0;
    bool playing = false; int underruns = 0, drops = 0;
    double phase = 0; int16_t last[2] = {0, 0};
    std::vector<int16_t> tmp;
    void open(int r) {
        close(); rate = r; phase = 0; last[0] = last[1] = 0;
        frame_bytes = rate * 4 / 60; high = frame_bytes * 14;
        SDL_AudioSpec want; memset(&want, 0, sizeof want);
        want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
        SDL_AudioSpec have;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!dev) { err = SDL_GetError(); return; }
        err.clear(); SDL_PauseAudioDevice(dev, 0);
    }
    // input is 32 kHz stereo; linear-resample to the device rate with a continuous phase
    void push(const std::vector<int16_t>& pcm) {
        if (!dev || pcm.empty()) return;
        Uint32 q = SDL_GetQueuedAudioSize(dev);
        if (playing && q == 0) underruns++;
        playing = true;
        if (q > (Uint32)high) { drops++; return; }
        if (rate == 32000) { SDL_QueueAudio(dev, pcm.data(), (Uint32)(pcm.size() * 2)); return; }
        size_t n = pcm.size() / 2; double step = 32000.0 / rate;
        tmp.clear();
        while (phase < (double)n) {
            int i0 = (int)std::floor(phase); double f = phase - i0;
            for (int ch = 0; ch < 2; ch++) {
                double a = i0 - 1 < 0 ? last[ch] : pcm[(i0 - 1) * 2 + ch];
                double b = pcm[i0 * 2 + ch];
                tmp.push_back((int16_t)(a + (b - a) * f));
            }
            phase += step;
        }
        phase -= (double)n; last[0] = pcm[(n - 1) * 2]; last[1] = pcm[(n - 1) * 2 + 1];
        if (!tmp.empty()) SDL_QueueAudio(dev, tmp.data(), (Uint32)(tmp.size() * 2));
    }
    int level() { return dev ? (int)SDL_GetQueuedAudioSize(dev) : 0; }
    bool synced() { return dev != 0; }
    void clear() { if (dev) SDL_ClearQueuedAudio(dev); playing = false; }
    void close() { if (dev) { SDL_CloseAudioDevice(dev); dev = 0; } }
};

// ---- menu model -----------------------------------------------------------------
struct MItem {
    std::string label, key;
    std::function<void()> act;
    std::function<bool()> checked;
    bool radio = false, disabled = false, sep = false;
    std::function<bool()> enabled;
    std::vector<MItem> sub;
    std::function<std::vector<MItem>()> dyn;
    bool has_children() const { return !sub.empty() || (bool)dyn; }
    std::vector<MItem> children() const { return dyn ? dyn() : sub; }
    bool is_enabled() const { return !disabled && (!enabled || enabled()); }
};
static MItem M_SEP() { MItem m; m.sep = true; return m; }
static MItem M_ACT(const std::string& l, const std::string& k, std::function<void()> f) { MItem m; m.label = l; m.key = k; m.act = std::move(f); return m; }
static MItem M_CHK(const std::string& l, const std::string& k, std::function<bool()> c, std::function<void()> f) { MItem m = M_ACT(l, k, std::move(f)); m.checked = std::move(c); return m; }
static MItem M_RAD(const std::string& l, std::function<bool()> c, std::function<void()> f) { MItem m = M_CHK(l, "", std::move(c), std::move(f)); m.radio = true; return m; }
static MItem M_DIS(const std::string& l, const std::string& k = "") { MItem m; m.label = l; m.key = k; m.disabled = true; return m; }
static MItem M_SUB(const std::string& l, std::vector<MItem> s) { MItem m; m.label = l; m.sub = std::move(s); return m; }
static MItem M_DYN(const std::string& l, std::function<std::vector<MItem>()> d) { MItem m; m.label = l; m.dyn = std::move(d); return m; }
static char mnemonic(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); i++) if (s[i] == '&' && s[i + 1] != '&') return (char)tolower((unsigned char)s[i + 1]);
    return 0;
}

// ---- key bindings (Snes9x defaults) ------------------------------------------------
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4 };
struct KeyBind { SDL_Scancode sc; int mods; };
static const char* PAD_NAMES[12] = {"Up", "Down", "Left", "Right", "A", "B", "X", "Y", "L", "R", "Start", "Select"};
static const int PAD_BITS[12] = {11, 10, 9, 8, 7, 15, 6, 14, 5, 4, 12, 13};   // SNES joypad register bits
static const SDL_Scancode PAD_DEFAULT[2][12] = {
    {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_V, SDL_SCANCODE_C,
     SDL_SCANCODE_D, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN},
    {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_H, SDL_SCANCODE_G,
     SDL_SCANCODE_T, SDL_SCANCODE_F, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_P, SDL_SCANCODE_O}};
enum { HK_SPEEDUP, HK_SPEEDDOWN, HK_PAUSE, HK_FRAMEADV, HK_FASTFWD, HK_SCREENSHOT, HK_RESET, HK_SAVE0, HK_LOAD0 = HK_SAVE0 + 10, HK_COUNT = HK_LOAD0 + 10 };
static std::string hk_name(int i) {
    static const char* N[] = {"Speed Up", "Speed Down", "Pause", "Frame Advance", "Fast Forward", "Save Screenshot", "Reset Game"};
    if (i < HK_SAVE0) return N[i];
    if (i < HK_LOAD0) return "Save Slot " + std::to_string(i - HK_SAVE0);
    return "Load Slot " + std::to_string(i - HK_LOAD0);
}
static KeyBind hk_default(int i) {
    switch (i) {
    case HK_SPEEDUP: return {SDL_SCANCODE_EQUALS, 0};
    case HK_SPEEDDOWN: return {SDL_SCANCODE_MINUS, 0};
    case HK_PAUSE: return {SDL_SCANCODE_PAUSE, 0};
    case HK_FRAMEADV: return {SDL_SCANCODE_BACKSLASH, 0};
    case HK_FASTFWD: return {SDL_SCANCODE_TAB, 0};
    case HK_SCREENSHOT: return {SDL_SCANCODE_F12, 0};
    case HK_RESET: return {SDL_SCANCODE_R, MOD_CTRL | MOD_ALT};
    }
    int slot = i < HK_LOAD0 ? i - HK_SAVE0 : i - HK_LOAD0;
    return {(SDL_Scancode)(SDL_SCANCODE_F1 + slot), i < HK_LOAD0 ? MOD_SHIFT : 0};
}
static std::string key_name(SDL_Scancode sc, int mods = 0) {
    if (sc == SDL_SCANCODE_UNKNOWN) return "(none)";
    std::string n = SDL_GetScancodeName(sc);
    if (n == "Return") n = "Enter";
    if (n.empty()) n = "#" + std::to_string((int)sc);
    std::string p;
    if (mods & MOD_CTRL) p += "Ctrl+";
    if (mods & MOD_ALT) p += "Alt+";
    if (mods & MOD_SHIFT) p += "Shift+";
    return p + n;
}
static int ev_mods(Uint16 m) { return ((m & KMOD_SHIFT) ? MOD_SHIFT : 0) | ((m & KMOD_CTRL) ? MOD_CTRL : 0) | ((m & KMOD_ALT) ? MOD_ALT : 0); }
static bool is_modifier(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT || sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL ||
           sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT || sc == SDL_SCANCODE_LGUI || sc == SDL_SCANCODE_RGUI;
}

struct GuiSettings {
    bool autoskip = true, pause_inactive = false, show_fps = false, stretch = true, aspect = true, bilinear = false;
    bool sync_sound = true, muted = false, pad_enabled[2] = {true, true};
    int max_skip = 4, rate = 32000, buffer_ms = 64, chmask = 0;
    SDL_Scancode pad[2][12];
    KeyBind hk[HK_COUNT];
    GuiSettings() {
        for (int p = 0; p < 2; p++) for (int b = 0; b < 12; b++) pad[p][b] = PAD_DEFAULT[p][b];
        for (int i = 0; i < HK_COUNT; i++) hk[i] = hk_default(i);
    }
    void load(const std::map<std::string, std::string>& c) {
        auto gb = [&](const char* k, bool& v) { auto it = c.find(k); if (it != c.end()) v = it->second == "1" || it->second == "true"; };
        auto gi = [&](const char* k, int& v) { auto it = c.find(k); if (it != c.end() && !it->second.empty()) v = atoi(it->second.c_str()); };
        gb("autoskip", autoskip); gb("pause_inactive", pause_inactive); gb("show_fps", show_fps); gb("stretch", stretch);
        gb("aspect", aspect); gb("bilinear", bilinear); gb("sync_sound", sync_sound); gb("muted", muted);
        gb("pad1_enabled", pad_enabled[0]); gb("pad2_enabled", pad_enabled[1]);
        gi("max_skip", max_skip); gi("rate", rate); gi("buffer_ms", buffer_ms);
        for (int p = 0; p < 2; p++) for (int b = 0; b < 12; b++) {
            std::string k = "pad" + std::to_string(p + 1) + "_" + PAD_NAMES[b]; auto it = c.find(k);
            if (it != c.end()) pad[p][b] = (SDL_Scancode)atoi(it->second.c_str());
        }
        for (int i = 0; i < HK_COUNT; i++) {
            auto it = c.find("hk_" + std::to_string(i));
            if (it != c.end()) { int sc = 0, md = 0; if (sscanf(it->second.c_str(), "%d:%d", &sc, &md) == 2) hk[i] = {(SDL_Scancode)sc, md}; }
        }
        max_skip = std::max(0, std::min(9, max_skip)); buffer_ms = std::max(16, std::min(208, buffer_ms));
    }
    void store(std::map<std::string, std::string>& c) const {
        auto sb = [&](const char* k, bool v) { c[k] = v ? "1" : "0"; };
        sb("autoskip", autoskip); sb("pause_inactive", pause_inactive); sb("show_fps", show_fps); sb("stretch", stretch);
        sb("aspect", aspect); sb("bilinear", bilinear); sb("sync_sound", sync_sound); sb("muted", muted);
        sb("pad1_enabled", pad_enabled[0]); sb("pad2_enabled", pad_enabled[1]);
        c["max_skip"] = std::to_string(max_skip); c["rate"] = std::to_string(rate); c["buffer_ms"] = std::to_string(buffer_ms);
        for (int p = 0; p < 2; p++) for (int b = 0; b < 12; b++) c["pad" + std::to_string(p + 1) + "_" + PAD_NAMES[b]] = std::to_string((int)pad[p][b]);
        for (int i = 0; i < HK_COUNT; i++) c["hk_" + std::to_string(i)] = std::to_string((int)hk[i].sc) + ":" + std::to_string(hk[i].mods);
    }
};

static int gui(Args& a) {
    if (!a.roms.empty()) config_set("roms_dir", fs::absolute(a.roms).string());
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "%s: SDL init failed: %s\n", APP, SDL_GetError()); return 1; }
    }
    std::map<std::string, std::string> cfg = config_load_all();
    GuiSettings S; S.load(cfg);
    if (a.mute) S.muted = true;
    auto save_cfg = [&]() { S.store(cfg); config_save_all(cfg); };
    AudioOut audio; audio.open(S.rate);

    std::unique_ptr<Machine> snes = create_system(a);
    RomInfo cur_info;
    if (!a.rom.empty()) cur_info = load_into(*snes, a.rom); else cur_info = snes->load(build_demo_rom(), "");
    bool core_dsp = snes->snes() != nullptr;
    auto apply_channels = [&]() { if (SNES* s = snes->snes()) s->apu.dspc.m.mute_mask = S.chmask; };
    apply_channels();

    const int MENU_H = 20;
    bool menu_visible = true, fullscreen = false;
    int W = 600, H = 400 - MENU_H;     // default client: 600 x 400 including the menu bar
    if (a.scale > 0) { int sc = std::max(1, std::min(10, a.scale)); W = 256 * sc; H = 224 * sc; }
    Gfx g;
    g.win = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H + MENU_H,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g.win) { fprintf(stderr, "%s: cannot create window: %s\n", APP, SDL_GetError()); return 1; }
    g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_ACCELERATED);
    if (!g.ren) g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_SOFTWARE);
    if (!g.ren) { fprintf(stderr, "%s: cannot create renderer: %s\n", APP, SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(g.ren, SDL_BLENDMODE_BLEND);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    g.init_fonts();

    // ---- runtime state
    bool paused = false, paused_inactive = false, turbo = false, advance_one = false, running = true;
    std::string msg; double msg_t = 0;
    double fps = 0; int fcount = 0, dcount = 0; double dfps = 0; bool skip_next = false;
    static const int SPEEDS[] = {10, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 175, 200, 250, 300, 400};
    int speed_i = 10;
    std::vector<u8> oops;
    auto now = []() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    double ft = now();
    auto note = [&](const std::string& m) { msg = m; msg_t = now() + 3.0; };

    // ---- recent games
    std::vector<std::string> recent;
    for (int i = 0; i < 10; i++) { auto it = cfg.find("recent" + std::to_string(i)); if (it != cfg.end() && !it->second.empty()) recent.push_back(it->second); }
    auto push_recent = [&](const std::string& p) {
        std::string ap = fs::absolute(p).string();
        recent.erase(std::remove(recent.begin(), recent.end(), ap), recent.end());
        recent.insert(recent.begin(), ap); if (recent.size() > 10) recent.resize(10);
        for (int i = 0; i < 10; i++) cfg["recent" + std::to_string(i)] = i < (int)recent.size() ? recent[i] : "";
        save_cfg();
    };

    // ---- core actions
    auto state_file = [&](int idx) {
        std::string base = snes->rom_path().empty() ? (fs::current_path() / "virtualsnes_demo").string() : snes->rom_path();
        return strip_ext(base) + snes->state_ext(idx);
    };
    auto after_load = [&](const std::string& path) {
        paused = false; audio.clear(); oops.clear(); apply_channels();
        if (!path.empty()) push_recent(path);
        note(snes->compat() == "chip" ? snes->title() + ": needs " + snes->chip_name() + " (not emulated)" : "Loaded " + snes->title());
    };
    auto try_load = [&](const std::string& p) {
        try { cur_info = load_into(*snes, p); after_load(p); }
        catch (std::exception& e) { note(std::string("Load failed: ") + e.what()); }
    };
    auto save_state_to = [&](const std::string& f, const std::string& label) {
        try { write_file(f, snes->save_state()); note(label + " saved"); } catch (std::exception& e) { note(std::string("Save failed: ") + e.what()); }
    };
    auto load_state_from = [&](const std::string& f, const std::string& label) {
        try {
            std::vector<u8> d = read_file(f);
            try { oops = snes->save_state(); } catch (...) {}
            snes->load_state(d); audio.clear(); note(label + " loaded");
        } catch (std::exception& e) { note(label + ": " + e.what()); }
    };
    auto save_slot = [&](int idx) { save_state_to(state_file(idx), "State " + std::to_string(idx)); };
    auto load_slot = [&](int idx) { load_state_from(state_file(idx), "State " + std::to_string(idx)); };
    auto screenshot = [&]() {
        std::string p = (fs::current_path() / ("virtualsnes_" + std::to_string((long long)time(nullptr)) + ".png")).string();
        int w, h; const u8* f = snes->frame(w, h);
        try { write_png(p, f, w, h); note("Screenshot saved: " + fs::path(p).filename().string()); } catch (std::exception& e) { note(e.what()); }
    };
    auto set_speed = [&](int d) {
        speed_i = std::max(0, std::min((int)(sizeof SPEEDS / sizeof SPEEDS[0]) - 1, speed_i + d));
        char b[64]; snprintf(b, sizeof b, "Speed: %d%% (%.1f ms/frame)", SPEEDS[speed_i], 1000.0 / snes->fps() * 100.0 / SPEEDS[speed_i]);
        note(b); audio.clear();
    };
    auto toggle_pause = [&]() { paused = !paused; paused_inactive = false; audio.clear(); note(paused ? "Paused" : "Unpaused"); };
    auto reset_game = [&]() { snes->reset(); apply_channels(); audio.clear(); note("Game reset"); };
    auto toggle_fullscreen = [&]() { fullscreen = !fullscreen; SDL_SetWindowFullscreen(g.win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); };
    auto mh = [&]() { return (menu_visible && !fullscreen) ? MENU_H : 0; };
    auto set_window_scale = [&](int n) {
        if (fullscreen) toggle_fullscreen();
        int w = 256 * n, h = 224 * n;
        SDL_Rect ub; if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(g.win), &ub) == 0) { w = std::min(w, ub.w); h = std::min(h, ub.h - mh() - 30); }
        SDL_SetWindowSize(g.win, w, h + mh());
    };
    auto toggle_menubar = [&]() {
        menu_visible = !menu_visible;
        if (!fullscreen) { int cw, ch; SDL_GetWindowSize(g.win, &cw, &ch); SDL_SetWindowSize(g.win, cw, ch + (menu_visible ? MENU_H : -MENU_H)); }
    };

    // ---- dialogs (modal, drawn inside the window) ---------------------------------
    std::string dialog;           // "", openrom, rominfo, about, input, hotkeys, emusettings, sound, display
    struct Hit { SDL_Rect r; std::function<void()> f; std::function<void()> dbl; };
    std::vector<Hit> hits;        // clickable regions laid out by the last drawn frame
    int mouse_down_hit = -1;
    GuiSettings S_backup;
    int capture = -1;             // key capture target: 0..23 pad fields (pad*12+b), 100+i hotkeys
    int input_pad = 0;
    auto open_dialog = [&](const std::string& d) { dialog = d; S_backup = S; capture = -1; audio.clear(); };
    auto close_dialog = [&](bool ok) {
        if (!ok) { S = S_backup; }
        if (S.rate != audio.rate) audio.open(S.rate);
        apply_channels(); save_cfg(); dialog.clear(); capture = -1;
    };

    // Open ROM dialog state
    std::string od_dir; std::vector<std::string> od_dirs; struct OdRom { std::string path, file, desc; u64 size; };
    std::vector<OdRom> od_roms; int od_dsel = 0, od_rsel = 0, od_dscroll = 0, od_rscroll = 0, od_focus = 1;
    std::map<std::string, std::string> desc_cache;
    auto rom_desc = [&](const std::string& path) -> std::string {
        auto it = desc_cache.find(path); if (it != desc_cache.end()) return it->second;
        std::string d;
        try {
            std::vector<u8> data;
            if (ends_with(lower(path), ".zip")) data = unzip_first_rom(read_file(path));
            else { std::ifstream f(path, std::ios::binary); data.resize(0x10400); f.read((char*)data.data(), (std::streamsize)data.size()); data.resize((size_t)f.gcount()); }
            d = analyze_rom(data).title;
        } catch (...) { d = "?"; }
        desc_cache[path] = d; return d;
    };
    auto od_scan = [&](const std::string& dir) {
        std::error_code ec;
        std::string d = dir.empty() ? fs::current_path(ec).string() : dir;
        if (!fs::is_directory(d, ec)) d = fs::current_path(ec).string();
        od_dir = fs::absolute(d, ec).lexically_normal().string();
        od_dirs.clear(); od_roms.clear();
        fs::path p(od_dir);
        if (p.has_parent_path() && p.parent_path() != p) od_dirs.push_back("..");
#if defined(_WIN32)
        if (!(p.has_parent_path() && p.parent_path() != p))
            for (char c = 'A'; c <= 'Z'; c++) { std::string dr = std::string(1, c) + ":\\"; if (fs::exists(dr, ec)) od_dirs.push_back(dr); }
#endif
        std::vector<std::string> ds;
        for (auto it = fs::directory_iterator(od_dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            if (it->is_directory(e2)) ds.push_back(name);
            else if (it->is_regular_file(e2) && (is_rom_name(name) || ends_with(lower(name), ".zip")))
                od_roms.push_back({it->path().string(), name, "", (u64)it->file_size(e2)});
        }
        std::sort(ds.begin(), ds.end(), [](const std::string& x, const std::string& y) { return lower(x) < lower(y); });
        od_dirs.insert(od_dirs.end(), ds.begin(), ds.end());
        std::sort(od_roms.begin(), od_roms.end(), [](const OdRom& x, const OdRom& y) { return lower(x.file) < lower(y.file); });
        od_dsel = 0; od_rsel = 0; od_dscroll = od_rscroll = 0;
    };
    auto od_enter = [&](int i) {
        if (i < 0 || i >= (int)od_dirs.size()) return;
        std::string n = od_dirs[i];
        fs::path np = n == ".." ? fs::path(od_dir).parent_path() : (n.size() == 3 && n[1] == ':' ? fs::path(n) : fs::path(od_dir) / n);
        od_scan(np.string());
    };
    auto od_open = [&]() {
        if (od_rsel < 0 || od_rsel >= (int)od_roms.size()) return;
        std::string p = od_roms[od_rsel].path;
        cfg["roms_dir"] = od_dir; save_cfg();
        dialog.clear(); try_load(p);
    };
    auto open_rom_dialog = [&]() {
        std::string start = cfg.count("last_dir") && !cfg["last_dir"].empty() ? cfg["last_dir"] : (cfg.count("roms_dir") ? cfg["roms_dir"] : "");
        if (start.empty()) { auto ds = default_rom_dirs(); if (!ds.empty()) start = ds[0]; }
        od_scan(start); dialog = "openrom"; audio.clear();
    };

    // ---- menus (structure of the Snes9x Win32 menu resource) --------------------
    auto bank_menu = [&](bool save) {
        std::vector<MItem> banks;
        for (int b = 0; b < 10; b++) {
            std::vector<MItem> slots;
            for (int s = 0; s < 10; s++) {
                int idx = b * 10 + s;
                std::string key = b == 0 ? ((save ? "Shift+F" : "F") + std::to_string(s + 1)) : "";
                slots.push_back(M_ACT("Slot #&" + std::to_string(s), key, [&, idx, save]() { if (save) save_slot(idx); else load_slot(idx); }));
            }
            banks.push_back(M_SUB("Bank #&" + std::to_string(b), slots));
        }
        banks.push_back(M_SEP());
        if (!save) {
            MItem o = M_ACT("Oops File", "", [&]() {
                if (oops.empty()) return;
                std::vector<u8> cur; try { cur = snes->save_state(); } catch (...) {}
                try { snes->load_state(oops); oops = cur; audio.clear(); note("Oops: previous state restored"); } catch (std::exception& e) { note(e.what()); }
            });
            o.enabled = [&]() { return !oops.empty(); };
            banks.push_back(o); banks.push_back(M_SEP());
        }
        banks.push_back(M_ACT("&Select File", "", [&, save]() {
            std::string sug = state_file(0);
            if (save) { std::string f = ask_save_path(std::string(APP) + " - Save Game Position", sug); if (!f.empty()) save_state_to(f, fs::path(f).filename().string()); }
            else { std::string f = ask_rom_path(std::string(APP) + " - Load Game Position"); if (!f.empty()) load_state_from(f, fs::path(f).filename().string()); }
        }));
        return banks;
    };
    auto recent_menu = [&]() {
        std::vector<MItem> v;
        if (recent.empty()) { v.push_back(M_DIS("Dummy")); return v; }
        for (size_t i = 0; i < recent.size(); i++) {
            std::string label = "&" + std::to_string((i + 1) % 10) + " " + fs::path(recent[i]).filename().string();
            std::string p = recent[i];
            v.push_back(M_ACT(label, "", [&, p]() { try_load(p); }));
        }
        return v;
    };
    std::vector<MItem> rate_menu = {M_CHK("&Mute Sound", "", [&]() { return S.muted; }, [&]() { S.muted = !S.muted; audio.clear(); note(S.muted ? "Sound muted" : "Sound on"); save_cfg(); })};
    for (int r : {8000, 11025, 16000, 22050, 30000, 32000, 35000, 44100, 48000}) {
        char lb[16]; snprintf(lb, sizeof lb, "%dKHz", r / 1000);
        rate_menu.push_back(M_RAD(lb, [&, r]() { return S.rate == r; }, [&, r]() { S.rate = r; audio.open(r); save_cfg(); }));
    }
    std::vector<MItem> buf_menu;
    for (int ms = 16; ms <= 208; ms += 16) buf_menu.push_back(M_RAD(std::to_string(ms) + "ms", [&, ms]() { return S.buffer_ms == ms; }, [&, ms]() { S.buffer_ms = ms; audio.clear(); save_cfg(); }));
    std::vector<MItem> ch_menu;
    for (int c = 0; c < 8; c++) {
        MItem m = M_CHK("Channel &" + std::to_string(c + 1), "", [&, c]() { return !(S.chmask & (1 << c)); }, [&, c]() { S.chmask ^= 1 << c; apply_channels(); save_cfg(); });
        m.enabled = [&]() { return core_dsp; };
        ch_menu.push_back(m);
    }
    ch_menu.push_back(M_SEP());
    { MItem m = M_ACT("Enable All", "", [&]() { S.chmask = 0; apply_channels(); save_cfg(); }); m.enabled = [&]() { return core_dsp; }; ch_menu.push_back(m); }
    std::vector<MItem> size_menu;
    for (int n = 1; n <= 10; n++) size_menu.push_back(M_ACT((n == 10 ? std::string("1&0") : "&" + std::to_string(n)) + "x", "", [&, n]() { set_window_scale(n); }));
    MItem cheat_apply = M_DIS("&Apply Cheats"); cheat_apply.checked = []() { return true; };
    MItem np_sync = M_DIS("S&ync Using Reset Game"); np_sync.checked = []() { return true; };
    MItem joy = M_RAD("Use SNES Joypad(s)", []() { return true; }, []() {});

    const char* MENU_NAMES[8] = {"&File", "&Emulation", "&Input", "&Sound", "&Video", "&Cheat", "&Netplay", "&Help"};
    std::vector<std::vector<MItem>> MENUS = {
        {M_ACT("L&oad Game...", "Ctrl+O", open_rom_dialog), M_DYN("Recent &Games", recent_menu), M_DIS("Load MultiCart..."), M_SEP(),
         M_DYN("&Save Game Position", [&]() { return bank_menu(true); }), M_DYN("&Load Game Position", [&]() { return bank_menu(false); }),
         M_DIS("&Save with Preview"), M_DIS("&Load with Preview"), M_SEP(),
         M_SUB("Save Other", {M_DIS("S&ave SPC Data"), M_ACT("Save Screenshot", "", screenshot),
                              M_ACT("Sa&ve S-RAM Data", "", [&]() { try { snes->save_sram(); note("S-RAM saved"); } catch (std::exception& e) { note(e.what()); } }),
                              M_DIS("Save Memory Pack")}),
         M_ACT("ROM Information...", "", [&]() { open_dialog("rominfo"); }), M_SEP(),
         M_DIS("Movie Play..."), M_DIS("Movie Record..."), M_DIS("Movie Stop"), M_SEP(), M_DIS("AVI Recording"), M_SEP(),
         M_ACT("&Reset Game", "", reset_game), M_ACT("E&xit", "Alt+F4", [&]() { running = false; })},
        {M_CHK("&Pause", "", [&]() { return paused; }, toggle_pause),
         M_CHK("Pause &When Inactive", "", [&]() { return S.pause_inactive; }, [&]() { S.pause_inactive = !S.pause_inactive; save_cfg(); }),
         M_ACT("&Frame Advance", "", [&]() { paused = true; advance_one = true; }), M_SEP(),
         M_DIS("&Hacks..."), M_ACT("&Settings...", "Alt+F8", [&]() { open_dialog("emusettings"); })},
        {M_ACT("&Input Configuration...", "Alt+F7", [&]() { open_dialog("input"); }), M_ACT("&Customize Hotkeys...", "Alt+F9", [&]() { open_dialog("hotkeys"); }), M_SEP(),
         M_DIS("Enable Background Input"), M_DIS("Background Keyboard Hotkeys"), M_SEP(), M_DIS("Detect gamepad changes"), M_SEP(),
         joy, M_DIS("Use SNES Mouse"), M_DIS("Use Super Scope"), M_DIS("Use Super Multitap (5-player)"), M_DIS("Use Konami Justifier"),
         M_DIS("Use Mouse in alternate port"), M_DIS("Use Multitaps (8-player)"), M_DIS("Use Dual Justifiers"), M_DIS("Use M.A.C.S. Rifle")},
        {M_SUB("&Playback Rate", rate_menu), M_SUB("&Buffer Length", buf_menu), M_SUB("&Channels", ch_menu), M_SEP(),
         M_CHK("S&ync Sound", "Alt+]", [&]() { return S.sync_sound; }, [&]() { S.sync_sound = !S.sync_sound; audio.clear(); save_cfg(); }),
         M_ACT("&Settings...", "Alt+T", [&]() { open_dialog("sound"); })},
        {M_CHK("&Hide Menubar", "Esc", [&]() { return !menu_visible; }, toggle_menubar),
         M_CHK("&Full Screen", "Alt+Enter", [&]() { return fullscreen; }, toggle_fullscreen), M_SUB("&Window Size", size_menu), M_SEP(),
         M_CHK("&Stretch Image", "Alt+Backspace", [&]() { return S.stretch; }, [&]() { S.stretch = !S.stretch; save_cfg(); }),
         M_CHK("&Maintain Aspect Ratio", "", [&]() { return S.aspect; }, [&]() { S.aspect = !S.aspect; save_cfg(); }),
         M_CHK("&Bilinear Filtering", "", [&]() { return S.bilinear; }, [&]() { S.bilinear = !S.bilinear; save_cfg(); }),
         M_CHK("Show Frame &Rate", "", [&]() { return S.show_fps; }, [&]() { S.show_fps = !S.show_fps; save_cfg(); }), M_SEP(),
         M_ACT("&Display Configuration...", "Alt+F5", [&]() { open_dialog("display"); })},
        {M_DIS("&Game Genie, Pro-Action Replay Codes"), M_DIS("&Search for New Cheats"), cheat_apply},
        {M_DIS("&Connect to Server..."), M_DIS("&Disconnect from Server"), M_SEP(), M_DIS("&Act as Server"),
         M_DIS("&Re-sync all Clients Using Freeze File Now"), M_DIS("&Send ROM Image to Clients Now"), M_DIS("S&end ROM Image to Clients"),
         np_sync, M_SEP(), M_DIS("&Options...")},
        {M_ACT("&About...", "", [&]() { open_dialog("about"); })}};
    std::vector<SDL_Rect> bar_rects;
    { int x = 0; for (auto n : MENU_NAMES) { int w = g.label_w(n) + 14; bar_rects.push_back({x, 0, w, MENU_H}); x += w; } }

    // menu interaction state
    int active = -1;                    // open top-level menu
    bool bar_focus = false;             // Alt pressed: menu bar keyboard focus
    int bar_idx = 0;                    // highlighted menu-bar item while it has keyboard focus
    bool show_ul = false;               // mnemonic underlines (keyboard navigation)
    std::vector<int> msel;              // selected item per open level
    struct PanelRow { SDL_Rect r; int level, index; };
    std::vector<PanelRow> prow; std::vector<SDL_Rect> ppanel;
    bool alt_alone = false;
    auto level_items = [&](int L) {
        std::vector<MItem> items = MENUS[active];
        for (int l = 0; l < L; l++) {
            int i = msel[l];
            if (i < 0 || i >= (int)items.size()) return std::vector<MItem>();
            items = items[i].children();
        }
        return items;
    };
    auto open_menu = [&](int i, bool kbd) {
        active = i; msel.assign(1, -1); bar_focus = false; audio.clear();
        if (kbd) { auto it = level_items(0); for (int k = 0; k < (int)it.size(); k++) if (!it[k].sep) { msel[0] = k; break; } }
    };
    auto close_menus = [&]() { active = -1; msel.clear(); bar_focus = false; show_ul = false; };
    auto activate = [&](int L, int i) {
        auto items = level_items(L);
        if (i < 0 || i >= (int)items.size()) return;
        const MItem& it = items[i];
        if (it.sep || !it.is_enabled()) return;
        if (it.has_children()) {
            msel.resize(L + 1); msel[L] = i;
            auto ch = it.children(); int first = -1;
            for (int k = 0; k < (int)ch.size(); k++) if (!ch[k].sep) { first = k; break; }
            msel.push_back(first);
            return;
        }
        auto f = it.act; close_menus(); if (f) f();
    };

    auto draw_menubar = [&](int mx, int my) {
        if (!mh()) return;
        g.fill(0, 0, W, MENU_H, C_FACE);
        for (int i = 0; i < 8; i++) {
            SDL_Rect r = bar_rects[i];
            bool hot = active < 0 && !bar_focus && mx >= r.x && mx < r.x + r.w && my >= 0 && my < MENU_H;
            if (i == active) { r.y += 1; r.h -= 2; g.hline(r.x, r.x + r.w - 1, r.y, C_SHADOW); g.vline(r.x, r.y, r.y + r.h - 1, C_SHADOW);
                               g.hline(r.x, r.x + r.w - 1, r.y + r.h - 1, C_HILITE); g.vline(r.x + r.w - 1, r.y, r.y + r.h - 1, C_HILITE); }
            else if (hot || (bar_focus && i == bar_idx)) {
                r.y += 1; r.h -= 2;
                g.hline(r.x, r.x + r.w - 1, r.y, C_HILITE); g.vline(r.x, r.y, r.y + r.h - 1, C_HILITE);
                g.hline(r.x, r.x + r.w - 1, r.y + r.h - 1, C_SHADOW); g.vline(r.x + r.w - 1, r.y, r.y + r.h - 1, C_SHADOW);
            }
            int off = i == active ? 1 : 0;
            g.label(bar_rects[i].x + 7 + off, 3 + off, MENU_NAMES[i], C_TEXT, show_ul);
        }
    };
    std::function<void(int, int, int, int)> draw_level;
    draw_level = [&](int L, int x, int y, int mx_) {
        auto items = level_items(L);
        if (items.empty()) return;
        int lw = 0, kw = 0, h = 6;
        for (auto& it : items) { if (it.sep) { h += 9; continue; } lw = std::max(lw, g.label_w(it.label)); kw = std::max(kw, g.text_w(it.key)); h += 17; }
        int width = 20 + lw + (kw ? 24 + kw : 0) + 26;
        if (x + width > W) x = L == 0 ? W - width : std::max(0, ppanel.back().x - width + 3);
        x = std::max(0, x);
        int wh = H + mh();
        if (y + h > wh) y = std::max(0, wh - h);
        SDL_Rect pr{x, y, width, h};
        raised(g, pr, false);
        g.hline(x, x + width - 2, y, C_FACE); g.vline(x, y, y + h - 2, C_FACE);
        ppanel.push_back(pr);
        int cy = y + 3, child_y = -1;
        for (int i = 0; i < (int)items.size(); i++) {
            const MItem& it = items[i];
            if (it.sep) { g.hline(x + 3, x + width - 4, cy + 3, C_SHADOW); g.hline(x + 3, x + width - 4, cy + 4, C_HILITE); cy += 9; continue; }
            SDL_Rect r{x + 3, cy, width - 6, 17};
            prow.push_back({r, L, i});
            bool sel = L < (int)msel.size() && msel[L] == i;
            bool en = it.is_enabled();
            if (sel && en) g.fill(r, C_SEL);
            SDL_Color tc = sel && en ? C_SELTEXT : C_TEXT;
            bool chk = it.checked && it.checked();
            if (!en) {
                if (!sel) g.label(r.x + 19, r.y + 3, it.label, C_HILITE, show_ul);
                g.label(r.x + 18, r.y + 2, it.label, C_GRAY, show_ul);
                if (!it.key.empty()) g.text(r.x + r.w - 14 - g.text_w(it.key), r.y + 2, it.key, C_GRAY);
                if (chk) check_glyph(g, r.x + 6, r.y + 5, C_GRAY);
            } else {
                g.label(r.x + 18, r.y + 2, it.label, tc, show_ul);
                if (!it.key.empty()) g.text(r.x + r.w - 14 - g.text_w(it.key), r.y + 2, it.key, tc);
                if (chk) { if (it.radio) dot_glyph(g, r.x + 7, r.y + 6, tc); else check_glyph(g, r.x + 6, r.y + 5, tc); }
            }
            if (it.has_children()) arrow_glyph(g, r.x + r.w - 9, r.y + 5, en ? tc : C_GRAY);
            if (sel && it.has_children() && (int)msel.size() > L + 1) child_y = cy;
            cy += 17;
        }
        if (child_y >= 0) draw_level(L + 1, x + width - 3, child_y - 3, mx_);
    };
    auto draw_menus = [&]() {
        prow.clear(); ppanel.clear();
        if (active < 0 || !mh()) return;
        draw_level(0, bar_rects[active].x, MENU_H - 1, 0);
    };

    // ---- dialog drawing helpers
    auto dlg_frame = [&](int w, int h, const std::string& title) -> SDL_Rect {
        int ww = W, wh = H + mh();
        w = std::min(w, ww - 4); h = std::min(h, H - 4);           // keep dialogs below the menu bar
        int x = (ww - w) / 2, y = mh() + (H - h) / 2;
        g.fill(0, 0, ww, wh, SDL_Color{0, 0, 0, 60});
        SDL_Rect r{x, y, w, h};
        raised(g, r, false);
        g.hline(x, x + w - 2, y, C_FACE); g.vline(x, y, y + h - 2, C_FACE);
        // caption with the classic gradient
        for (int i = 0; i < w - 8; i++) {
            double t = (double)i / std::max(1, w - 9);
            SDL_Color c{(u8)(C_CAP1.r + (C_CAP2.r - C_CAP1.r) * t), (u8)(C_CAP1.g + (C_CAP2.g - C_CAP1.g) * t), (u8)(C_CAP1.b + (C_CAP2.b - C_CAP1.b) * t), 255};
            g.vline(x + 4 + i, y + 4, y + 21, c);
        }
        g.text(x + 8, y + 6, title, C_SELTEXT, true);
        SDL_Rect xb{x + w - 22, y + 6, 16, 14};
        raised(g, xb);
        g.line(xb.x + 4, xb.y + 3, xb.x + 10, xb.y + 9, C_TEXT); g.line(xb.x + 5, xb.y + 3, xb.x + 11, xb.y + 9, C_TEXT);
        g.line(xb.x + 10, xb.y + 3, xb.x + 4, xb.y + 9, C_TEXT); g.line(xb.x + 11, xb.y + 3, xb.x + 5, xb.y + 9, C_TEXT);
        hits.push_back({xb, [&]() { close_dialog(false); }, nullptr});
        return SDL_Rect{x + 4, y + 26, w - 8, h - 30};
    };
    auto button = [&](int x, int y, int w, int h, const std::string& lbl, std::function<void()> f, bool def = false) {
        SDL_Rect r{x, y, w, h};
        int mx, my; Uint32 mb = SDL_GetMouseState(&mx, &my);
        bool down = (mb & SDL_BUTTON_LMASK) && mx >= x && mx < x + w && my >= y && my < y + h && mouse_down_hit >= 0;
        if (def) { g.fill(x - 1, y - 1, w + 2, h + 2, C_BLACK); }
        if (down) pressed_frame(g, r); else raised(g, r);
        int tw = g.label_w(lbl);
        g.label(x + (w - tw) / 2 + (down ? 1 : 0), y + (h - UI_FONT_H) / 2 + (down ? 1 : 0), lbl, C_TEXT, true);
        hits.push_back({r, std::move(f), nullptr});
    };
    auto checkbox = [&](int x, int y, const std::string& lbl, bool v, std::function<void()> f, bool en = true) {
        SDL_Rect box{x, y + 1, 13, 13};
        sunken(g, box, en ? C_WINDOW : C_FACE);
        if (v) check_glyph(g, x + 3, y + 4, en ? C_TEXT : C_GRAY);
        if (!en) g.text(x + 19, y + 1, lbl, C_HILITE);
        g.text(x + 18, y, lbl, en ? C_TEXT : C_GRAY);
        if (en) hits.push_back({SDL_Rect{x, y, 20 + g.text_w(lbl), 15}, std::move(f), nullptr});
    };
    auto radio = [&](int x, int y, const std::string& lbl, bool v, std::function<void()> f) {
        // 12 px round radio button
        static const char* RING[12] = {"....gggg....", "..ggkkkkgg..", ".gkk....kkw.", ".gk......lw.", "gk........lw", "gk........lw",
                                       "gk........lw", "gk........lw", ".gk......lw.", ".lll....llw.", "..wwllllww..", "....wwww...."};
        for (int r = 0; r < 12; r++) for (int c = 0; c < 12; c++) {
            char ch = RING[r][c]; SDL_Color col;
            if (ch == 'g') col = C_SHADOW; else if (ch == 'k') col = C_DKSHADOW; else if (ch == 'w') col = C_HILITE; else if (ch == 'l') col = C_FACE;
            else { if (r > 1 && r < 10 && c > 1 && c < 10) col = C_WINDOW; else continue; }
            g.fill(x + c, y + 1 + r, 1, 1, col);
        }
        if (v) { g.fill(x + 4, y + 5, 4, 4, C_TEXT); g.fill(x + 5, y + 4, 2, 6, C_TEXT); g.fill(x + 3, y + 6, 6, 2, C_TEXT); }
        g.text(x + 17, y, lbl, C_TEXT);
        hits.push_back({SDL_Rect{x, y, 20 + g.text_w(lbl), 15}, std::move(f), nullptr});
    };
    auto groupbox = [&](int x, int y, int w, int h, const std::string& lbl) {
        g.hline(x, x + w - 1, y + 6, C_SHADOW); g.hline(x + 1, x + w - 2, y + 7, C_HILITE);
        g.hline(x, x + w - 1, y + h - 2, C_SHADOW); g.hline(x, x + w - 1, y + h - 1, C_HILITE);
        g.vline(x, y + 6, y + h - 1, C_SHADOW); g.vline(x + 1, y + 7, y + h - 2, C_HILITE);
        g.vline(x + w - 2, y + 6, y + h - 2, C_SHADOW); g.vline(x + w - 1, y + 6, y + h - 1, C_HILITE);
        int tw = g.text_w(lbl);
        g.fill(x + 7, y, tw + 6, 13, C_FACE);
        g.text(x + 10, y, lbl, C_TEXT);
    };
    auto textbox = [&](SDL_Rect r, const std::vector<std::string>& lines) {
        sunken(g, r, C_WINDOW);
        int y = r.y + 4;
        for (auto& l : lines) { if (y + UI_FONT_H > r.y + r.h - 3) break; g.text_clip(r.x + 5, y, r.w - 10, l, C_TEXT); y += 14; }
    };
    auto keyfield = [&](int x, int y, int w, const std::string& txt, bool active_cap, std::function<void()> f) {
        SDL_Rect r{x, y, w, 19};
        sunken(g, r, active_cap ? C_SEL : C_WINDOW);
        g.text_clip(x + 5, y + 3, w - 10, active_cap ? std::string("Press a key...") : txt, active_cap ? C_SELTEXT : C_TEXT);
        hits.push_back({r, std::move(f), nullptr});
    };
    auto spinner = [&](int x, int y, const std::string& val, std::function<void()> dec, std::function<void()> inc) {
        SDL_Rect r{x, y, 54, 19}; sunken(g, r, C_WINDOW); g.text(x + 6, y + 3, val, C_TEXT);
        button(x + 56, y, 18, 19, "-", std::move(dec)); button(x + 76, y, 18, 19, "+", std::move(inc));
    };
    auto ok_cancel = [&](const SDL_Rect& c) {
        button(c.x + c.w - 156, c.y + c.h - 30, 72, 23, "OK", [&]() { close_dialog(true); }, true);
        button(c.x + c.w - 78, c.y + c.h - 30, 72, 23, "Cancel", [&]() { close_dialog(false); });
    };

    auto rominfo_lines = [&]() {
        const RomInfo& I = cur_info; std::vector<std::string> v; char b[160];
        const u8* h = I.data.size() >= I.header_off + 0x40 ? I.data.data() + I.header_off : nullptr;
        v.push_back("Name: " + I.title);
        snprintf(b, sizeof b, "Speed: %02X/%s", I.map_mode, I.speed.c_str()); v.push_back(b);
        v.push_back("ROM Map: " + I.mapper);
        snprintf(b, sizeof b, "Type: %02x", I.chipset); v.push_back(b);
        v.push_back("Kart contents: " + I.chip_name);
        auto bits = [](u64 bytes) { char t[32]; if (bytes >= 0x20000) snprintf(t, sizeof t, "%llu Mbits", (unsigned long long)(bytes / 0x20000)); else snprintf(t, sizeof t, "%llu Kbits", (unsigned long long)(bytes / 128)); return std::string(t); };
        if (I.rom_size_code >= 1 && I.rom_size_code <= 13) v.push_back("Header ROM Size: " + bits((u64)1024 << I.rom_size_code));
        else { snprintf(b, sizeof b, "Header ROM Size: $%02X", I.rom_size_code); v.push_back(b); }
        v.push_back("Calculated ROM Size: " + bits(I.data.size()));
        snprintf(b, sizeof b, "SRAM size: %d KB (%d Kbits)", I.sram_size / 1024, I.sram_size * 8 / 1024); v.push_back(b);
        u32 sum = 0; for (u8 x : I.data) sum += x;
        snprintf(b, sizeof b, "Actual Checksum: %04X", sum & 0xFFFF); v.push_back(b);
        if (h) {
            snprintf(b, sizeof b, "Header Checksum: %04X", h[0x1E] | (h[0x1F] << 8)); v.push_back(b);
            snprintf(b, sizeof b, "Header Checksum Complement: %04X", h[0x1C] | (h[0x1D] << 8)); v.push_back(b);
        }
        v.push_back(std::string("Output: ") + ((I.region > 12 || I.region < 2) ? "NTSC 60Hz" : "PAL 50Hz"));
        snprintf(b, sizeof b, "CRC32: %08X", crc32_buf(I.data.data(), I.data.size())); v.push_back(b);
        if (h) { snprintf(b, sizeof b, "Licensee: $%02X", h[0x1A]); v.push_back(b); }
        v.push_back("ROM Version: 1." + std::to_string(h ? h[0x1B] : 0));
        v.push_back("Region: " + I.region_name);
        v.push_back("Status: " + std::string(I.compat == "ok" ? "Supported" : I.compat == "chip" ? "Needs an enhancement chip vsnes does not emulate" : "Unknown chipset"));
        return v;
    };
    auto about_lines = [&]() {
        return std::vector<std::string>{std::string(COPY), "Virtual SNES Emulator, C++ edition", "",
            "65C816 + SPC700 cores, S-DSP (port of blargg's snes_spc 0.9.0 DSP as",
            "used in Snes9x, LGPL 2.1+), SA-1, DSP-1, SuperFX, OBC1, S-RTC, S-DD1,",
            "CX4, ST010/011, SPC7110 and SGB support.", "", "Core in use: " + snes->core_name(),
            "UI font: Liberation Sans (SIL Open Font License 1.1).", "",
            "The window layout mirrors the classic Snes9x Win32 front-end.",
            "Not affiliated with or endorsed by Nintendo or the Snes9x team.",
            "No game data is included."};
    };

    auto draw_dialog = [&]() {
        hits.clear();
        if (dialog.empty()) return;
        if (dialog == "openrom") {
            SDL_Rect c = dlg_frame(std::min(W - 8, 640), std::min(H - 6, 380), "Open ROM");
            int bot = 52, treew = std::min(190, c.w / 3);
            SDL_Rect tree{c.x + 4, c.y + 4, treew, c.h - bot - 6}, list{c.x + treew + 8, c.y + 4, c.w - treew - 12, c.h - bot - 6};
            sunken(g, tree, C_WINDOW); sunken(g, list, C_WINDOW);
            // folder pane: current folder, then sub-folders
            int ry = tree.y + 3, rh = 16;
            g.text_clip(tree.x + 5, ry, tree.w - 10, od_dir, C_TEXT, true); ry += rh + 2;
            int dvis = std::max(1, (tree.y + tree.h - 3 - ry) / rh);
            if (od_dsel < od_dscroll) od_dscroll = od_dsel;
            if (od_dsel >= od_dscroll + dvis) od_dscroll = od_dsel - dvis + 1;
            for (int i = od_dscroll; i < std::min((int)od_dirs.size(), od_dscroll + dvis); i++) {
                SDL_Rect r{tree.x + 3, ry, tree.w - 6, rh};
                bool sel = i == od_dsel;
                if (sel) g.fill(r, od_focus == 0 ? C_SEL : C_FACE);
                g.fill(r.x + 4, r.y + 4, 12, 9, C_FOLDER); g.fill(r.x + 4, r.y + 2, 5, 3, C_FOLDER);
                g.hline(r.x + 4, r.x + 15, r.y + 12, C_SHADOW);
                g.text_clip(r.x + 21, r.y + 1, r.w - 24, od_dirs[i], sel && od_focus == 0 ? C_SELTEXT : C_TEXT);
                int idx = i;
                hits.push_back({r, [&, idx]() { od_focus = 0; od_dsel = idx; }, [&, idx]() { od_enter(idx); }});
                ry += rh;
            }
            // ROM list: File / Description / Size
            int cw0 = list.w * 45 / 100, cw1 = list.w * 37 / 100, cw2 = list.w - cw0 - cw1 - 4;
            SDL_Rect hdr{list.x + 2, list.y + 2, list.w - 4, 18};
            const char* cols[3] = {"File", "Description", "Size"}; int cws[3] = {cw0, cw1, cw2}, hx = hdr.x;
            for (int k = 0; k < 3; k++) { raised(g, SDL_Rect{hx, hdr.y, cws[k], hdr.h}); g.text(hx + 6, hdr.y + 3, cols[k], C_TEXT); hx += cws[k]; }
            int ly = hdr.y + hdr.h + 1, lvis = std::max(1, (list.y + list.h - 3 - ly) / rh);
            if (od_rsel < od_rscroll) od_rscroll = od_rsel;
            if (od_rsel >= od_rscroll + lvis) od_rscroll = od_rsel - lvis + 1;
            if (od_roms.empty()) g.text(list.x + 8, ly + 4, "No SNES ROMs in this folder (.sfc .smc .swc .fig .bin .zip)", C_GRAY);
            for (int i = od_rscroll; i < std::min((int)od_roms.size(), od_rscroll + lvis); i++) {
                OdRom& e = od_roms[i];
                if (e.desc.empty()) e.desc = rom_desc(e.path);
                SDL_Rect r{list.x + 2, ly, list.w - 4, rh};
                bool sel = i == od_rsel;
                if (sel) g.fill(r, od_focus == 1 ? C_SEL : C_FACE);
                SDL_Color tc = sel && od_focus == 1 ? C_SELTEXT : C_TEXT;
                char sz[32];
                if (e.size >= 1048576) snprintf(sz, sizeof sz, "%.2f MB", e.size / 1048576.0);
                else snprintf(sz, sizeof sz, "%llu KB", (unsigned long long)((e.size + 1023) / 1024));
                g.text_clip(r.x + 6, r.y + 1, cw0 - 10, e.file, tc);
                g.text_clip(r.x + cw0 + 6, r.y + 1, cw1 - 10, e.desc, tc);
                g.text_clip(r.x + cw0 + cw1 + 6, r.y + 1, cw2 - 10, sz, tc);
                int idx = i;
                hits.push_back({r, [&, idx]() { od_focus = 1; od_rsel = idx; }, [&, idx]() { od_rsel = idx; od_open(); }});
                ly += rh;
            }
            // bottom row: the Snes9x combo boxes (fixed to Auto here), Open ROM / Cancel
            int by = c.y + c.h - bot + 4;
            const char* lbls[4] = {"Memory Type:", "Interleave Mode:", "Video System:", "Header:"};
            int lx = c.x + 4, cwid = std::max(60, (c.w - 200) / 4);
            for (int k = 0; k < 4; k++) {
                g.text(lx, by, lbls[k], C_TEXT);
                SDL_Rect cb{lx, by + 16, cwid - 6, 20}; sunken(g, cb, C_FACE); g.text(cb.x + 4, cb.y + 3, "Auto", C_GRAY);
                SDL_Rect ab{cb.x + cb.w - 17, cb.y + 2, 15, 16}; raised(g, ab);
                for (int t = 0; t < 4; t++) g.hline(ab.x + 4 + t, ab.x + 10 - t, ab.y + 6 + t, C_GRAY);
                lx += cwid;
            }
            button(c.x + c.w - 168, by + 14, 80, 23, "&Open ROM", [&]() { od_open(); }, true);
            button(c.x + c.w - 84, by + 14, 80, 23, "&Cancel", [&]() { dialog.clear(); });
        } else if (dialog == "rominfo") {
            auto v = rominfo_lines();
            SDL_Rect c = dlg_frame(420, std::min(H - 6, 40 + 14 * (int)v.size() + 50), "Rom Info");
            textbox(SDL_Rect{c.x + 6, c.y + 6, c.w - 12, c.h - 46}, v);
            button(c.x + c.w - 78, c.y + c.h - 32, 72, 23, "OK", [&]() { dialog.clear(); }, true);
        } else if (dialog == "about") {
            auto v = about_lines();
            SDL_Rect c = dlg_frame(440, std::min(H - 6, 40 + 14 * (int)v.size() + 50), std::string(APP) + " - About Dialog");
            textbox(SDL_Rect{c.x + 6, c.y + 6, c.w - 12, c.h - 46}, v);
            button(c.x + c.w - 78, c.y + c.h - 32, 72, 23, "OK", [&]() { dialog.clear(); }, true);
        } else if (dialog == "input") {
            SDL_Rect c = dlg_frame(470, 330, "Input Configuration");
            int x = c.x + 10, y = c.y + 8;
            g.text(x, y + 3, "Joypad #:", C_TEXT);
            for (int p = 0; p < 2; p++) radio(x + 70 + p * 70, y + 2, std::to_string(p + 1), input_pad == p, [&, p]() { input_pad = p; capture = -1; });
            checkbox(x + 230, y + 2, "Enabled", S.pad_enabled[input_pad], [&]() { S.pad_enabled[input_pad] = !S.pad_enabled[input_pad]; });
            groupbox(c.x + 6, y + 26, c.w - 12, 214, "Joypad " + std::to_string(input_pad + 1) + " keys (click a field, then press a key)");
            int col_w = (c.w - 40) / 2;
            static const int ORDER[2][6] = {{0, 1, 2, 3, 10, 11}, {4, 5, 6, 7, 8, 9}};
            for (int colI = 0; colI < 2; colI++) for (int r = 0; r < 6; r++) {
                int b = ORDER[colI][r];
                int fx = c.x + 18 + colI * (col_w + 8), fy = y + 46 + r * 31;
                g.text(fx, fy + 3, std::string(PAD_NAMES[b]) + ":", C_TEXT);
                int id = input_pad * 12 + b;
                keyfield(fx + 56, fy, col_w - 60, key_name(S.pad[input_pad][b]), capture == id, [&, id]() { capture = id; });
            }
            button(c.x + 10, c.y + c.h - 30, 110, 23, "Restore Defaults", [&]() { for (int b = 0; b < 12; b++) S.pad[input_pad][b] = PAD_DEFAULT[input_pad][b]; capture = -1; });
            ok_cancel(c);
        } else if (dialog == "hotkeys") {
            SDL_Rect c = dlg_frame(std::min(W - 8, 580), std::min(H - 6, 372), "Customize Hotkeys");
            int rows = (HK_COUNT + 1) / 2, rh = std::max(12, std::min(22, (c.h - 70) / rows)), col_w = (c.w - 30) / 2;
            g.text(c.x + 10, c.y + 6, "Click a field, then press the new key (with Shift / Ctrl / Alt). Del clears.", C_TEXT);
            for (int i = 0; i < HK_COUNT; i++) {
                int colI = i / rows, r = i % rows;
                int fx = c.x + 10 + colI * (col_w + 10), fy = c.y + 26 + r * rh;
                g.text(fx, fy + (rh - UI_FONT_H) / 2, hk_name(i) + ":", C_TEXT);
                int id = 100 + i;
                SDL_Rect fr{fx + 110, fy, col_w - 114, rh - 2};
                sunken(g, fr, capture == id ? C_SEL : C_WINDOW);
                g.text_clip(fr.x + 4, fr.y + (fr.h - UI_FONT_H) / 2, fr.w - 8, capture == id ? std::string("Press a key...") : key_name(S.hk[i].sc, S.hk[i].mods), capture == id ? C_SELTEXT : C_TEXT);
                hits.push_back({fr, [&, id]() { capture = id; }, nullptr});
            }
            button(c.x + 10, c.y + c.h - 30, 110, 23, "Restore Defaults", [&]() { for (int i = 0; i < HK_COUNT; i++) S.hk[i] = hk_default(i); capture = -1; });
            ok_cancel(c);
        } else if (dialog == "emusettings") {
            SDL_Rect c = dlg_frame(400, 262, std::string(APP) + " - Emulator Settings");
            int x = c.x + 12, y = c.y + 8;
            groupbox(c.x + 6, y, c.w - 12, 82, "Frame skipping");
            checkbox(x + 6, y + 20, "Automatic frame skip (keeps game speed correct)", S.autoskip, [&]() { S.autoskip = !S.autoskip; });
            g.text(x + 6, y + 49, "Maximum skipped frames:", C_TEXT);
            spinner(x + 170, y + 46, std::to_string(S.max_skip), [&]() { S.max_skip = std::max(0, S.max_skip - 1); }, [&]() { S.max_skip = std::min(9, S.max_skip + 1); });
            groupbox(c.x + 6, y + 90, c.w - 12, 82, "General");
            checkbox(x + 6, y + 110, "Pause when inactive", S.pause_inactive, [&]() { S.pause_inactive = !S.pause_inactive; });
            char b[96]; snprintf(b, sizeof b, "Emulation speed: %d%%   ( = / - keys )", SPEEDS[speed_i]);
            g.text(x + 6, y + 132, b, C_TEXT);
            snprintf(b, sizeof b, "Last second: %.0f frames emulated, %.0f drawn", fps, dfps);
            g.text(x + 6, y + 148, b, C_GRAY);
            ok_cancel(c);
        } else if (dialog == "sound") {
            SDL_Rect c = dlg_frame(440, 300, std::string(APP) + " - Sound Settings");
            int x = c.x + 12, y = c.y + 8;
            groupbox(c.x + 6, y, 196, 168, "Output Sound Rate");
            int rates[9] = {8000, 11025, 16000, 22050, 30000, 32000, 35000, 44100, 48000};
            for (int i = 0; i < 9; i++) { int r = rates[i]; char lb[32]; snprintf(lb, sizeof lb, "%d Hz", r);
                radio(x + 6 + (i / 5) * 92, y + 22 + (i % 5) * 26, lb, S.rate == r, [&, r]() { S.rate = r; }); }
            groupbox(c.x + 210, y, c.w - 216, 168, "Options");
            checkbox(c.x + 222, y + 22, "Mute sound", S.muted, [&]() { S.muted = !S.muted; });
            checkbox(c.x + 222, y + 46, "Sync sound (pace video by audio)", S.sync_sound, [&]() { S.sync_sound = !S.sync_sound; });
            g.text(c.x + 222, y + 76, "Buffer length:", C_TEXT);
            spinner(c.x + 222, y + 94, std::to_string(S.buffer_ms) + " ms", [&]() { S.buffer_ms = std::max(16, S.buffer_ms - 16); }, [&]() { S.buffer_ms = std::min(208, S.buffer_ms + 16); });
            g.text(c.x + 222, y + 128, "DSP: " + snes->dsp_name(), C_TEXT);
            g.text(c.x + 222, y + 144, std::string("Device: ") + (audio.dev ? "SDL (" + std::to_string(audio.rate) + " Hz)" : "none"), C_TEXT);
            groupbox(c.x + 6, y + 174, c.w - 12, 52, "Channels");
            for (int ch = 0; ch < 8; ch++) checkbox(x + 6 + ch * 50, y + 196, std::to_string(ch + 1), !(S.chmask & (1 << ch)), [&, ch]() { S.chmask ^= 1 << ch; apply_channels(); }, core_dsp);
            ok_cancel(c);
        } else if (dialog == "display") {
            SDL_Rect c = dlg_frame(400, 236, std::string(APP) + " - Display Configuration");
            int x = c.x + 12, y = c.y + 8;
            groupbox(c.x + 6, y, c.w - 12, 140, "Display");
            checkbox(x + 6, y + 20, "Stretch image to the window", S.stretch, [&]() { S.stretch = !S.stretch; });
            checkbox(x + 6, y + 44, "Maintain 4:3 aspect ratio", S.aspect, [&]() { S.aspect = !S.aspect; });
            checkbox(x + 6, y + 68, "Bilinear filtering", S.bilinear, [&]() { S.bilinear = !S.bilinear; });
            checkbox(x + 6, y + 92, "Show frame rate", S.show_fps, [&]() { S.show_fps = !S.show_fps; });
            checkbox(x + 6, y + 116, "Full screen", fullscreen, [&]() { toggle_fullscreen(); });
            ok_cancel(c);
        }
    };

    // ---- input dispatch helpers
    auto hotkey_down = [&](SDL_Scancode sc, int mods) -> bool {
        for (int i = 0; i < HK_COUNT; i++) {
            if (S.hk[i].sc != sc || S.hk[i].mods != mods || sc == SDL_SCANCODE_UNKNOWN) continue;
            if (i == HK_SPEEDUP) set_speed(+1);
            else if (i == HK_SPEEDDOWN) set_speed(-1);
            else if (i == HK_PAUSE) toggle_pause();
            else if (i == HK_FRAMEADV) { paused = true; advance_one = true; }
            else if (i == HK_FASTFWD) turbo = true;
            else if (i == HK_SCREENSHOT) screenshot();
            else if (i == HK_RESET) reset_game();
            else if (i < HK_LOAD0) save_slot(i - HK_SAVE0);
            else load_slot(i - HK_LOAD0);
            return true;
        }
        return false;
    };
    auto accel = [&](SDL_Keycode k, int mods) -> bool {
        if (mods == MOD_CTRL && k == SDLK_o) { open_rom_dialog(); return true; }
        if (mods == MOD_ALT) {
            switch (k) {
            case SDLK_F4: running = false; return true;
            case SDLK_RETURN: toggle_fullscreen(); return true;
            case SDLK_BACKSPACE: S.stretch = !S.stretch; save_cfg(); return true;
            case SDLK_F5: open_dialog("display"); return true;
            case SDLK_F7: open_dialog("input"); return true;
            case SDLK_F8: open_dialog("emusettings"); return true;
            case SDLK_F9: open_dialog("hotkeys"); return true;
            case SDLK_t: open_dialog("sound"); return true;
            case SDLK_RIGHTBRACKET: S.sync_sound = !S.sync_sound; audio.clear(); note(std::string("Sync sound ") + (S.sync_sound ? "on" : "off")); save_cfg(); return true;
            }
        }
        if (mods == 0 && k == SDLK_ESCAPE) { if (fullscreen) toggle_fullscreen(); else toggle_menubar(); return true; }
        return false;
    };
    auto menu_key = [&](SDL_Keycode k) {
        int L = (int)msel.size() - 1;
        auto items = level_items(L);
        auto step = [&](int d) {
            int n = (int)items.size(); if (!n) return;
            int i = msel[L];
            for (int t = 0; t < n; t++) { i = (i + d + n) % n; if (!items[i].sep) break; }
            msel[L] = i;
        };
        if (k == SDLK_ESCAPE) { if (L > 0) msel.pop_back(); else { int a0 = active; close_menus(); bar_focus = true; show_ul = true; bar_idx = a0; } return; }
        if (k == SDLK_DOWN) { if (msel[L] < 0) msel[L] = -1; step(+1); return; }
        if (k == SDLK_UP) { step(-1); return; }
        if (k == SDLK_RIGHT) {
            if (msel[L] >= 0 && msel[L] < (int)items.size() && items[msel[L]].has_children() && items[msel[L]].is_enabled()) { activate(L, msel[L]); return; }
            open_menu((active + 1) % 8, true); show_ul = true; return;
        }
        if (k == SDLK_LEFT) { if (L > 0) { msel.pop_back(); return; } open_menu((active + 7) % 8, true); show_ul = true; return; }
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) { activate(L, msel[L]); return; }
        if (k >= 32 && k < 127) {
            char ch = (char)tolower((int)k);
            for (int i = 0; i < (int)items.size(); i++) if (!items[i].sep && mnemonic(items[i].label) == ch) { msel[L] = i; activate(L, i); return; }
        }
    };

    // ---- pacing
    double FRAME_T = 1.0 / snes->fps();
    int skips = 0, buf_extra = 0, last_under = 0, clean = 0;
    double next_t = now();
    auto target_bytes = [&]() { return std::max(audio.frame_bytes * 2, (int)((double)S.buffer_ms / 1000.0 * audio.rate * 4)) + buf_extra * audio.frame_bytes; };
    auto pace = [&]() {
        double speed = SPEEDS[speed_i] / 100.0;
        FRAME_T = 1.0 / (snes->fps() * speed);
        if (turbo) { next_t = now(); return; }
        if (!paused && dialog.empty() && active < 0 && !S.muted && S.sync_sound && audio.synced() && speed_i == 10) {
            int target = target_bytes();
            double limit = now() + 0.1;
            while (audio.level() > target && now() < limit) std::this_thread::sleep_for(std::chrono::microseconds(500));
            next_t = now(); return;
        }
        next_t += FRAME_T;
        double d = next_t - now();
        if (d > 0) {
            if (d > 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(d - 0.0015));
            while (now() < next_t) {}
        } else if (d < -0.25) next_t = now();
    };

    int mouse_x = 0, mouse_y = 0;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    turbo = false; alt_alone = false;
                    if (S.pause_inactive && !paused) { paused = true; paused_inactive = true; audio.clear(); }
                } else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    if (paused_inactive) { paused = false; paused_inactive = false; }
                }
                break;
            case SDL_MOUSEMOTION: {
                mouse_x = ev.motion.x; mouse_y = ev.motion.y;
                if (active >= 0) {
                    if (mouse_y < mh()) for (int i = 0; i < 8; i++) {
                        SDL_Rect& r = bar_rects[i];
                        if (mouse_x >= r.x && mouse_x < r.x + r.w && i != active) open_menu(i, false);
                    }
                    for (auto& pr : prow) if (mouse_x >= pr.r.x && mouse_x < pr.r.x + pr.r.w && mouse_y >= pr.r.y && mouse_y < pr.r.y + pr.r.h) {
                        auto items = level_items(pr.level);
                        if (pr.index >= (int)items.size()) break;
                        msel.resize(pr.level + 1); msel[pr.level] = pr.index;
                        if (items[pr.index].has_children() && items[pr.index].is_enabled()) msel.push_back(-1);
                        break;
                    }
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN: {
                if (ev.button.button != SDL_BUTTON_LEFT) break;
                int x = ev.button.x, y = ev.button.y;
                if (dialog.empty() && mh() && y < MENU_H) {
                    for (int i = 0; i < 8; i++) { SDL_Rect& r = bar_rects[i]; if (x >= r.x && x < r.x + r.w) { if (active == i) close_menus(); else open_menu(i, false); break; } }
                    break;
                }
                if (active >= 0) {
                    bool hit = false;
                    for (auto& pr : prow) if (x >= pr.r.x && x < pr.r.x + pr.r.w && y >= pr.r.y && y < pr.r.y + pr.r.h) { hit = true; activate(pr.level, pr.index); break; }
                    if (!hit) { bool inpanel = false; for (auto& pp : ppanel) if (x >= pp.x && x < pp.x + pp.w && y >= pp.y && y < pp.y + pp.h) inpanel = true; if (!inpanel) close_menus(); }
                    break;
                }
                mouse_down_hit = -1;
                for (int i = (int)hits.size() - 1; i >= 0; i--) { SDL_Rect& r = hits[i].r; if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { mouse_down_hit = i; break; } }
                if (mouse_down_hit >= 0 && ev.button.clicks >= 2 && hits[mouse_down_hit].dbl) { auto f = hits[mouse_down_hit].dbl; mouse_down_hit = -1; f(); }
                break;
            }
            case SDL_MOUSEBUTTONUP: {
                if (ev.button.button != SDL_BUTTON_LEFT || mouse_down_hit < 0) break;
                int x = ev.button.x, y = ev.button.y, i = mouse_down_hit; mouse_down_hit = -1;
                if (i < (int)hits.size()) { SDL_Rect r = hits[i].r; if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) { auto f = hits[i].f; if (f) f(); } }
                break;
            }
            case SDL_MOUSEWHEEL:
                if (dialog == "openrom") {
                    if (mouse_x < W / 3) od_dsel = std::max(0, std::min((int)od_dirs.size() - 1, od_dsel - ev.wheel.y));
                    else od_rsel = std::max(0, std::min((int)od_roms.size() - 1, od_rsel - ev.wheel.y));
                }
                break;
            case SDL_DROPFILE: { std::string f = ev.drop.file; SDL_free(ev.drop.file); dialog.clear(); close_menus(); try_load(f); break; }
            case SDL_KEYDOWN: {
                SDL_Keycode k = ev.key.keysym.sym; SDL_Scancode sc = ev.key.keysym.scancode; int mods = ev_mods(ev.key.keysym.mod);
                if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT) { if (!ev.key.repeat) alt_alone = true; break; }
                alt_alone = false;
                if (capture >= 0) {                                       // key capture for the binding dialogs
                    if (is_modifier(sc)) break;
                    if (sc == SDL_SCANCODE_ESCAPE) { capture = -1; break; }   // Esc cancels the capture
                    if (capture >= 100) { int i = capture - 100; S.hk[i] = (sc == SDL_SCANCODE_DELETE && !mods) ? KeyBind{SDL_SCANCODE_UNKNOWN, 0} : KeyBind{sc, mods}; }
                    else S.pad[capture / 12][capture % 12] = (sc == SDL_SCANCODE_DELETE) ? SDL_SCANCODE_UNKNOWN : sc;
                    capture = -1; break;
                }
                if (!dialog.empty()) {
                    if (dialog == "openrom") {
                        if (k == SDLK_ESCAPE) dialog.clear();
                        else if (k == SDLK_TAB) od_focus ^= 1;
                        else if (k == SDLK_BACKSPACE) { for (int i = 0; i < (int)od_dirs.size(); i++) if (od_dirs[i] == "..") { od_enter(i); break; } }
                        else if (k == SDLK_UP) { if (od_focus) od_rsel = std::max(0, od_rsel - 1); else od_dsel = std::max(0, od_dsel - 1); }
                        else if (k == SDLK_DOWN) { if (od_focus) od_rsel = std::min((int)od_roms.size() - 1, od_rsel + 1); else od_dsel = std::min((int)od_dirs.size() - 1, od_dsel + 1); }
                        else if (k == SDLK_PAGEUP) { if (od_focus) od_rsel = std::max(0, od_rsel - 10); else od_dsel = std::max(0, od_dsel - 10); }
                        else if (k == SDLK_PAGEDOWN) { if (od_focus) od_rsel = std::min((int)od_roms.size() - 1, od_rsel + 10); else od_dsel = std::min((int)od_dirs.size() - 1, od_dsel + 10); }
                        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if (od_focus) od_open(); else od_enter(od_dsel); }
                        od_rsel = std::max(0, od_rsel); od_dsel = std::max(0, od_dsel);
                    } else if (k == SDLK_ESCAPE) close_dialog(false);
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) close_dialog(true);
                    break;
                }
                if (active >= 0) { menu_key(k); break; }
                if (bar_focus) {
                    if (k == SDLK_ESCAPE) { bar_focus = false; show_ul = false; }
                    else if (k == SDLK_LEFT) bar_idx = (bar_idx + 7) % 8;
                    else if (k == SDLK_RIGHT) bar_idx = (bar_idx + 1) % 8;
                    else if (k == SDLK_DOWN || k == SDLK_RETURN || k == SDLK_UP) { open_menu(bar_idx, true); show_ul = true; }
                    else if (k >= 32 && k < 127) { char ch = (char)tolower((int)k); for (int i = 0; i < 8; i++) if (mnemonic(MENU_NAMES[i]) == ch) { open_menu(i, true); show_ul = true; break; } }
                    break;
                }
                if ((mods & MOD_ALT) && k >= 32 && k < 127 && mh()) {          // Alt+mnemonic opens a menu
                    char ch = (char)tolower((int)k); bool done = false;
                    for (int i = 0; i < 8; i++) if (mnemonic(MENU_NAMES[i]) == ch) { open_menu(i, true); show_ul = true; done = true; break; }
                    if (done) break;
                }
                if (accel(k, mods)) break;
                hotkey_down(sc, mods);
                break;
            }
            case SDL_KEYUP: {
                SDL_Scancode sc = ev.key.keysym.scancode;
                if ((sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT) && alt_alone && capture < 0 && dialog.empty() && mh()) {
                    if (active >= 0) close_menus();
                    else { bar_focus = !bar_focus; show_ul = bar_focus; bar_idx = 0; }
                }
                alt_alone = false;
                if (sc == S.hk[HK_FASTFWD].sc) turbo = false;
                break;
            }
            }
        }
        int ww, wh; SDL_GetRendererOutputSize(g.ren, &ww, &wh);
        W = std::max(256, ww); H = std::max(64, wh - mh());

        // joypads (disabled while a dialog or menu has focus, like Snes9x)
        const Uint8* ks = SDL_GetKeyboardState(nullptr);
        int jp[2] = {0, 0};
        if (dialog.empty() && active < 0 && !bar_focus)
            for (int p = 0; p < 2; p++) if (S.pad_enabled[p]) for (int b = 0; b < 12; b++) if (S.pad[p][b] != SDL_SCANCODE_UNKNOWN && ks[S.pad[p][b]]) jp[p] |= 1 << PAD_BITS[b];
        snes->set_joy(jp[0], jp[1]);

        bool modal = !dialog.empty() || active >= 0;
        bool run_now = (!paused && !modal) || advance_one;
        bool drew = true;
        if (run_now) {
            bool this_skip = advance_one ? false : skip_next;
            advance_one = false;
            snes->set_skip_render(this_skip);
            try { snes->run_frame(); }
            catch (std::exception& e) { note(std::string("Emulation error: ") + e.what()); paused = true; }
            fcount++;
            std::vector<int16_t> pcm = snes->take_audio();
            bool sound_sync = audio.synced() && !S.muted && !turbo && S.sync_sound && speed_i == 10;
            if (!S.muted && !turbo && !paused) audio.push(pcm);
            bool behind;
            if (sound_sync) {
                if (audio.underruns != last_under) { last_under = audio.underruns; buf_extra = std::min(5, buf_extra + 1); clean = 0; }
                else { clean++; if (clean > 600 && buf_extra > 0) { buf_extra--; clean = 0; } }
                behind = audio.level() < target_bytes() - audio.frame_bytes / 2;
            } else behind = now() > next_t + FRAME_T;
            if (S.autoskip && behind && skips < S.max_skip && !paused) { skip_next = true; skips++; }
            else { skip_next = false; skips = 0; }
            drew = !this_skip;
        }
        if (!drew && now() - ft < 1.0) { pace(); continue; }
        if (drew && run_now) dcount++;

        int fw, fh; const u8* fr = snes->frame(fw, fh);
        if (!g.tex || fw != g.tex_w || fh != g.tex_h || g.tex_linear != S.bilinear) {
            if (g.tex) SDL_DestroyTexture(g.tex);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, S.bilinear ? "linear" : "nearest");
            g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, fw, fh);
            g.tex_w = fw; g.tex_h = fh; g.tex_linear = S.bilinear;
        }
        SDL_UpdateTexture(g.tex, nullptr, fr, fw * 3);
        int top = mh();
        g.fill(0, top, W, H, C_BLACK);
        SDL_Rect dst;
        if (S.stretch) {
            double ratio = S.aspect ? 4.0 / 3.0 : (double)W / H;
            int vw = std::min(W, (int)(H * ratio)), vh = std::min(H, (int)(W / ratio));
            if (!S.aspect) { vw = W; vh = H; }
            dst = SDL_Rect{(W - vw) / 2, top + (H - vh) / 2, vw, vh};
        } else {
            int n = std::max(1, std::min(W / fw, H / fh));
            int vw = fw * n, vh = fh * n;
            if (S.aspect) vw = std::min(W, vh * 4 / 3);
            dst = SDL_Rect{(W - vw) / 2, top + (H - vh) / 2, vw, vh};
        }
        SDL_RenderCopy(g.ren, g.tex, nullptr, &dst);
        double tnow = now();
        if (tnow - ft >= 1.0) { fps = fcount / (tnow - ft); dfps = dcount / (tnow - ft); fcount = dcount = 0; ft = tnow; }
        // on-screen messages (bottom left) and frame rate (bottom right), Snes9x-style
        std::string m = !msg.empty() && tnow < msg_t ? msg : std::string();
        if (m.empty() && paused) m = paused_inactive ? "Paused (window inactive)" : "Paused";
        if (turbo) m = m.empty() ? "Fast forward" : m;
        if (!m.empty()) g.osd(dst.x + 6, dst.y + dst.h - UI_FONT_H - 4, m);
        if (S.show_fps) {
            char fb[48]; snprintf(fb, sizeof fb, "%02d/%02d", (int)std::lround(dfps), (int)std::lround(snes->fps() * SPEEDS[speed_i] / 100.0));
            g.osd(dst.x + dst.w - 6 - g.text_w(fb), dst.y + dst.h - UI_FONT_H - 4, fb);
        }
        draw_dialog();
        draw_menubar(mouse_x, mouse_y);
        draw_menus();
        SDL_RenderPresent(g.ren);
        pace();
    }
    try { snes->save_sram(); } catch (...) {}
    save_cfg();
    snes->close();
    audio.close();
    if (g.tex) SDL_DestroyTexture(g.tex);
    SDL_DestroyRenderer(g.ren); SDL_DestroyWindow(g.win);
    SDL_Quit();
    return 0;
}
#endif

// ===================================================================== main
static void usage() {
    printf("usage: vsnes [-h] [--backend {auto,snes9x,cpp}] [--core PATH] [--roms DIR] [--scale N]\n"
           "               [--headless N] [--shot PNG] [--wav WAV] [--dsp {auto,snes9x}] [--mute]\n"
           "               [--hold MASK] [--list-library] [--cputest DIR] [--per-file N] [rom]\n\n"
           "%s - Virtual SNES Emulator (classic Snes9x-style GUI). %s\n\n"
           "  rom                 .sfc/.smc/.zip ROM (omit for built-in demo)\n"
           "  --backend           auto uses an installed SNES9x libretro core, otherwise the built-in C++ core\n"
           "  --core PATH         trusted local snes9x_libretro .dll/.dylib/.so\n"
           "  --roms DIR          commercial ROM library folder (saved in ~/.virtualsnes.json)\n"
           "  --scale N           window size 256N x 224N (1-5); default window is 600x400\n"
           "  --headless N        run N frames without a window and report speed\n"
           "  --shot PNG          with --headless: save the last frame\n"
           "  --wav WAV           with --headless: write the emulated audio (32 kHz stereo)\n"
           "  --dsp ENGINE        audio engine (the Snes9x S-DSP port is always used)\n"
           "  --mute              start with audio muted (M toggles)\n"
           "  --hold MASK         with --headless: joypad bits to hold (e.g. 0x0100 = Right)\n"
           "  --list-library      scan ROM library and print catalog, then exit\n"
           "  --cputest DIR       run SingleStepTests 65816/spc700 *.json files in DIR, then exit\n"
           "  --per-file N        with --cputest: max tests per file (default 60)\n", APP, COPY);
}

int main(int argc, char** argv) {
    init_g1();
    try { exe_dir = fs::absolute(fs::path(argv[0])).parent_path().string(); } catch (...) { exe_dir = "."; }
    Args a;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto need = [&](const char* opt) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "vsnes: %s needs a value\n", opt); exit(2); }
            return argv[++i];
        };
        std::string val; size_t eq = s.find('=');
        if (s.rfind("--", 0) == 0 && eq != std::string::npos) { val = s.substr(eq + 1); s = s.substr(0, eq); }
        auto get = [&](const char* opt) { return val.empty() ? need(opt) : val; };
        if (s == "-h" || s == "--help") { usage(); return 0; }
        else if (s == "--backend") a.backend = get("--backend");
        else if (s == "--core") a.core = get("--core");
        else if (s == "--roms") a.roms = get("--roms");
        else if (s == "--scale") a.scale = atoi(get("--scale").c_str());
        else if (s == "--headless") a.headless = atoi(get("--headless").c_str());
        else if (s == "--shot") a.shot = get("--shot");
        else if (s == "--wav") a.wav = get("--wav");
        else if (s == "--dsp") a.dsp = get("--dsp");
        else if (s == "--hold") a.hold = get("--hold");
        else if (s == "--cputest") a.cputest = get("--cputest");
        else if (s == "--per-file") a.per_file = atoi(get("--per-file").c_str());
        else if (s == "--mute") a.mute = true;
        else if (s == "--list-library") a.list_library = true;
        else if (s == "--vsnes-demo") g_legacy_demo_text = true;
        else if (s == "--dump-demo") { write_file(get("--dump-demo"), build_demo_rom()); return 0; }
        else if (!s.empty() && s[0] == '-') { fprintf(stderr, "vsnes: unknown option %s\n", s.c_str()); usage(); return 2; }
        else a.rom = s;
    }
    try {
        if (!a.cputest.empty()) return cputest(a.cputest, a.per_file);
        if (!a.roms.empty()) config_set("roms_dir", fs::absolute(a.roms).string());
        if (a.list_library) {
            auto entries = scan_rom_library();
            int ok, chip, unk; library_summary(entries, ok, chip, unk);
            printf("%s commercial library: %zu ROMs  (%d playable mapper, %d enhancement chip, %d unknown)\n", APP, entries.size(), ok, chip, unk);
            printf("(No ROMs are bundled - place your dumps in the library folder.)\n");
            for (auto& e : entries)
                printf("  [%-7s] %-7s %-21s  %-22s  %s\n", e.compat.c_str(), e.mapper.c_str(), e.title.c_str(), e.chip_name.c_str(), e.file.c_str());
            return 0;
        }
        if (a.headless > 0) return headless(a);
#ifdef VSNES_HAVE_SDL
        return gui(a);
#else
        fprintf(stderr, "%s was built without SDL2 (headless only). Use --headless N, or rebuild with SDL2.\n", APP);
        return 1;
#endif
    } catch (std::exception& e) {
        fprintf(stderr, "%s: %s\n", APP, e.what());
        return 1;
    }
}
