// padscore: Wii Remote / Pro Controller (WPAD, KPAD). Only a Pro Controller on channel 0 exists,
// and only while the keyboard/host controllers are set to act as one (Input menu). Struct layouts
// and constants follow Cemu's padscore.
#include "../crashrec.h"
#include "../runtime.h"
#include "../input.h"
#include "../rumble.h"
#include <cstdio>
#include <string>
#include <vector>

namespace interp { bool repeat_input(); bool fresh_sticks(); }

namespace {
constexpr int32_t kWpadErrNone = 0, kWpadErrNoController = -1;
constexpr int32_t kKpadErrNone = 0, kKpadErrNoController = -2;
constexpr uint8_t kDevURCC = 31, kDevNone = 253, kFormatURCC = 22;

bool connected(uint32_t chan) { return chan == 0 && input::pro_controller(); }

// VPAD button bits (input::PadState) -> Pro Controller (URCC) bits
uint32_t pro_buttons(uint32_t v) {
    static const uint32_t map[][2] = {
        {input::kUp, 0x1},     {input::kLeft, 0x2},    {input::kZR, 0x4},     {input::kX, 0x8},
        {input::kA, 0x10},     {input::kY, 0x20},      {input::kB, 0x40},     {input::kZL, 0x80},
        {input::kR, 0x200},    {input::kPlus, 0x400},  {input::kHome, 0x800}, {input::kMinus, 0x1000},
        {input::kL, 0x2000},   {input::kDown, 0x4000}, {input::kRight, 0x8000},
        {input::kStickR, 0x10000}, {input::kStickL, 0x20000},
    };
    uint32_t out = 0;
    for (auto& m : map)
        if (v & m[0]) out |= m[1];
    return out;
}
// Test aid, Pro Controller input recording (one record per logic-pass read):
//   WWHD_INPUT_RECORD=path   writes every read (buttons, sticks) to path
//   WWHD_INPUT_PLAY=path,first   holding L+R+ZL+ZR starts playing path from record `first`; live
//                                input returns when the recording ends
struct PadRecord { uint32_t buttons; float lx, ly, rx, ry; };
input::PadState record_or_play(input::PadState live) {
    static FILE* rec = [] {
        const char* e = getenv("WWHD_INPUT_RECORD");
        FILE* f = e ? fopen(e, "wb") : nullptr;
        if (f) LOG("[pad] recording input to %s", e);
        return f;
    }();
    static std::vector<PadRecord> play;
    static size_t next = 0;
    static int state = [] {  // 0 off, 1 armed, 2 playing, 3 done
        const char* e = getenv("WWHD_INPUT_PLAY");
        if (!e) return 0;
        std::string path(e);
        size_t first = 0, comma = path.rfind(',');
        if (comma != std::string::npos) { first = strtoull(path.c_str() + comma + 1, nullptr, 10); path.resize(comma); }
        if (FILE* f = fopen(path.c_str(), "rb")) {
            PadRecord r;
            while (fread(&r, sizeof r, 1, f) == 1) play.push_back(r);
            fclose(f);
        }
        next = first;
        LOG("[pad] input playback %s: %zu records from %zu (L+R+ZL+ZR starts it)", path.c_str(), play.size(), first);
        return next < play.size() ? 1 : 0;
    }();
    if (rec) {
        PadRecord r{live.buttons, live.lx, live.ly, live.rx, live.ry};
        fwrite(&r, sizeof r, 1, rec);
        static int n = 0;
        if (++n % 30 == 0) fflush(rec);
    }
    constexpr uint32_t kCombo = input::kL | input::kR | input::kZL | input::kZR;
    if (state == 1 && (live.buttons & kCombo) == kCombo) { state = 2; LOG("[pad] input playback started"); }
    if (state == 2) {
        if (next >= play.size()) { state = 3; LOG("[pad] input playback finished"); }
        else {
            const PadRecord& r = play[next++];
            input::PadState p = live;
            p.buttons = r.buttons; p.lx = r.lx; p.ly = r.ly; p.rx = r.rx; p.ry = r.ry;
            return p;
        }
    }
    if (state == 1) live.buttons &= ~kCombo;  // the start combo itself never reaches the game
    return live;
}
}  // namespace

HLE(padscore, KPADInitEx) {}
HLE(padscore, KPADGetMplsWorkSize) { ret(c, 0x5FE0); }
HLE(padscore, KPADSetMplsWorkarea) {}
HLE(padscore, WPADEnableURCC) {}
HLE(padscore, WPADEnableWiiRemote) {}
HLE(padscore, WPADDisconnect) {}
// The Pro Controller motor: cmd 0 stops it, 1 runs it until the game stops it. The Pro
// Controller is only connected while the keyboard/controllers act as one (see connected).
HLE(padscore, WPADControlMotor) {
    uint32_t chan = arg(c, 0), cmd = arg(c, 1);
    if (connected(chan)) {
        TRACE("[pad] WPADControlMotor(%u, %u) -> %s", chan, cmd, cmd ? "rumble" : "stop");
        rumble::pro_motor(chan, cmd != 0);
    }
    ret(c, (uint32_t)kWpadErrNone);
}
HLE(padscore, WPADGetBatteryLevel) { ret(c, 4); }  // full
HLE(padscore, WPADCanSendStreamData) { ret(c, 0); }
HLE(padscore, WPADSendStreamData) { ret(c, (uint32_t)kWpadErrNoController); }
HLE(padscore, WPADControlSpeaker) { ret(c, (uint32_t)kWpadErrNoController); }

HLE(padscore, WPADProbe) {
    // (chan, uint32* type) -> WPAD error
    uint32_t chan = arg(c, 0), type = arg(c, 1);
    bool on = connected(chan);
    TRACE("[pad] WPADProbe(%u) -> %s", chan, on ? "pro" : "none");
    if (type) st32(type, on ? kDevURCC : kDevNone);
    ret(c, (uint32_t)(on ? kWpadErrNone : kWpadErrNoController));
}

HLE(padscore, KPADReadEx) {
    // (chan, KPADStatus* buf, count, int32* err) -> samples written
    uint32_t chan = arg(c, 0), st = arg(c, 1), count = arg(c, 2), err = arg(c, 3);
    TRACE("[pad] KPADReadEx(%u, %08X, %u) connected=%d", chan, st, count, connected(chan));
    if (!connected(chan) || !st || (int32_t)count <= 0) {
        if (err) st32(err, (uint32_t)(connected(chan) ? kKpadErrNone : kKpadErrNoController));
        ret(c, 0);
        return;
    }
    static uint32_t last = 0;
    static input::PadState last_p;
    const bool repeat = interp::repeat_input();
    input::PadState p = repeat ? last_p : record_or_play(crashrec::read(1));  // see interp.cpp; crash recovery records/replays it
    if (repeat && interp::fresh_sticks()) {  // true 60: sticks every pass, buttons on full passes
        input::PadState f = input::read();
        p.lx = f.lx; p.ly = f.ly; p.rx = f.rx; p.ry = f.ry;
    }
    last_p = p;
    uint32_t hold = pro_buttons(p.buttons);
    memset(mem::ptr(st), 0, 0xF0);
    st8(st + 0x5C, kDevURCC);     // devType
    st8(st + 0x5D, 0);            // wpadErr
    st8(st + 0x5F, kFormatURCC);  // data_format
    st32(st + 0x60, hold);                // ex_status.uc.hold
    st32(st + 0x64, hold & ~last);        // trig
    st32(st + 0x68, last & ~hold);        // release
    last = hold;
    stf32(st + 0x6C, p.lx); stf32(st + 0x70, p.ly);  // lstick
    stf32(st + 0x74, p.rx); stf32(st + 0x78, p.ry);  // rstick
    st32(st + 0x7C, 1);                   // charge
    st32(st + 0x80, 1);                   // cable
    if (err) st32(err, kKpadErrNone);
    ret(c, 1);
}
