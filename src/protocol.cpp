#include "protocol.h"

#include <cstdio>

namespace sq {

Report Request(uint8_t tid, uint8_t cls, uint8_t id, uint8_t size, const std::vector<uint8_t>& args) {
    Report r{};
    // r[0] is the report ID; the message starts at r[1].
    r[1] = New;
    r[2] = tid;
    r[6] = size;
    r[7] = cls;
    r[8] = id;
    for (size_t i = 0; i < args.size() && i < 80; i++) r[9 + i] = args[i];
    r[89] = Crc(r);
    return r;
}

uint8_t Crc(const Report& r) {
    uint8_t c = 0;
    for (int i = 3; i < 89; i++) c ^= r[i];  // message bytes 2..87
    return c;
}

Reply ParseReply(const Report& r) {
    Reply out;
    out.status = r[1];
    out.cls = r[7];
    out.id = r[8];
    for (int i = 0; i < 80; i++) out.args[i] = r[9 + i];
    return out;
}

namespace cmd {
Report Firmware(uint8_t tid) { return Request(tid, 0x00, 0x81, 0x02); }
Report DpiStages(uint8_t tid) { return Request(tid, 0x04, 0x86, 0x26, {Saved}); }
Report Dpi(uint8_t tid) { return Request(tid, 0x04, 0x85, 0x07, {Saved}); }
Report Battery(uint8_t tid) { return Request(tid, 0x07, 0x80, 0x02); }
Report Charging(uint8_t tid) { return Request(tid, 0x07, 0x84, 0x02); }
Report Polling(uint8_t tid) { return Request(tid, 0x00, 0x85, 0x01); }
Report HyperPolling(uint8_t tid) { return Request(tid, 0x00, 0xC0, 0x01); }
Report GetBrightness(uint8_t tid, uint8_t led, Store s) { return Request(tid, 0x0F, 0x84, 0x03, {s, led}); }
Report GetEffect(uint8_t tid, uint8_t led, Store s) { return Request(tid, 0x0F, 0x82, 0x0C, {s, led}); }
Report SetBrightness(uint8_t tid, uint8_t led, Store s, uint8_t value) {
    return Request(tid, 0x0F, 0x04, 0x03, {s, led, value});
}
Report SetEffect(uint8_t tid, uint8_t led, Store s, Effect e, uint8_t r, uint8_t g, uint8_t b) {
    switch (e) {
    case EffectStatic: return Request(tid, 0x0F, 0x02, 0x09, {s, led, EffectStatic, 0x00, 0x00, 0x01, r, g, b});
    case EffectBreathing:  // one colour
        return Request(tid, 0x0F, 0x02, 0x09, {s, led, EffectBreathing, 0x01, 0x00, 0x01, r, g, b});
    case EffectSpectrum: return Request(tid, 0x0F, 0x02, 0x06, {s, led, EffectSpectrum, 0x00, 0x00, 0x00});
    default: return Request(tid, 0x0F, 0x02, 0x06, {s, led, EffectOff, 0x00, 0x00, 0x00});
    }
}
}  // namespace cmd

std::string FirmwareText(const Reply& r) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.%02d", r.args[0], r.args[1]);
    return buf;
}

std::optional<Stages> ParseStages(const Reply& r) {
    // storage, active stage, count, then 7 bytes a stage:
    // number, x (big-endian), y (big-endian), two zeros.
    int count = r.args[2];
    if (count < 1 || count > 10) return std::nullopt;
    Stages s;
    s.active = r.args[1];
    for (int i = 0; i < count; i++) {
        const uint8_t* p = &r.args[3 + i * 7];
        s.dpi.push_back({(p[1] << 8) | p[2], (p[3] << 8) | p[4]});
    }
    return s;
}

std::pair<int, int> ParseDpi(const Reply& r) {
    return {(r.args[1] << 8) | r.args[2], (r.args[3] << 8) | r.args[4]};
}

int BatteryPercent(const Reply& r) { return (r.args[1] * 100 + 127) / 255; }

int PollingHz(const Reply& r) {
    switch (r.args[0]) {
    case 0x01: return 1000;
    case 0x02: return 500;
    case 0x08: return 125;
    default: return 0;
    }
}

int HyperPollingHz(const Reply& r) {
    switch (r.args[1]) {
    case 0x01: return 8000;
    case 0x02: return 4000;
    case 0x04: return 2000;
    case 0x08: return 1000;
    case 0x10: return 500;
    case 0x20: return 250;
    case 0x40: return 125;
    default: return 0;
    }
}

Light ParseEffect(const Reply& r) {
    // storage, led, effect, param, speed, colour count, colours...
    Light l;
    switch (r.args[2]) {
    case EffectOff: l.effect = EffectOff; break;
    case EffectStatic: l.effect = EffectStatic; break;
    case EffectBreathing: l.effect = EffectBreathing; break;
    case EffectSpectrum: l.effect = EffectSpectrum; break;
    default: l.effect = EffectOther; break;
    }
    if (r.args[5] >= 1) {
        l.r = r.args[6];
        l.g = r.args[7];
        l.b = r.args[8];
    }
    return l;
}

const std::vector<uint8_t>& ZoneIds() {
    static const std::vector<uint8_t> ids = {0x01, 0x03, 0x04, 0x05, 0x0A, 0x10, 0x11};
    return ids;
}

std::string ZoneName(uint8_t led) {
    switch (led) {
    case 0x01: return "Scroll wheel";
    case 0x03: return "Battery";
    case 0x04: return "Logo";
    case 0x05: return "Backlight";
    case 0x0A: return "Underglow";
    case 0x10: return "Right side";
    case 0x11: return "Left side";
    default: {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "Zone %02X", led);
        return buf;
    }
    }
}

}  // namespace sq
