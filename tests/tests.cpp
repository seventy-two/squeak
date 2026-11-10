// Protocol tests, using replies captured from a Basilisk V3 Pro and a
// DeathAdder V3 Pro. Builds on Windows (build.bat) or Linux:
//   g++ -std=c++17 -Isrc tests/tests.cpp src/protocol.cpp -o tests && ./tests
#include <cstdio>
#include <cstdlib>

#include "protocol.h"

using namespace sq;

static int failures = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                             \
        }                                                           \
    } while (0)

// reply makes a Report from a captured reply's status and args.
static Report reply(uint8_t status, std::initializer_list<uint8_t> args) {
    Report r{};
    r[1] = status;
    int i = 9;
    for (uint8_t a : args) r[i++] = a;
    return r;
}

int main() {
    {
        Report r = cmd::Firmware(0x1F);
        CHECK(r[0] == 0 && r[2] == 0x1F && r[6] == 0x02 && r[7] == 0x00 && r[8] == 0x81);
        CHECK(r[89] == (0x02 ^ 0x00 ^ 0x81));
        Report s = cmd::SetEffect(0x1F, 0x04, Saved, EffectStatic, 0x10, 0x20, 0x30);
        CHECK(s[6] == 0x09 && s[7] == 0x0F && s[8] == 0x02);
        CHECK(s[9] == Saved && s[10] == 0x04 && s[11] == EffectStatic && s[14] == 1 && s[15] == 0x10 && s[17] == 0x30);
        CHECK(s[89] == Crc(s));
    }
    {
        // Basilisk V3 Pro: 3 stages, 400/800/1200, stage 2 active.
        auto st = ParseStages(ParseReply(reply(Ok, {0x01, 0x02, 0x03, 0x01, 0x01, 0x90, 0x01, 0x90, 0, 0, 0x02, 0x03,
                                                    0x20, 0x03, 0x20, 0, 0, 0x03, 0x04, 0xB0, 0x04, 0xB0, 0, 0})));
        CHECK(st && st->active == 2 && st->dpi.size() == 3);
        CHECK(st && st->dpi[0].first == 400 && st->dpi[1].first == 800 && st->dpi[2].first == 1200 && st->dpi[2].second == 1200);
        CHECK(!ParseStages(ParseReply(reply(Ok, {0x01, 0x00, 0x00}))));
    }
    {
        auto d = ParseDpi(ParseReply(reply(Ok, {0x01, 0x03, 0x20, 0x03, 0x20})));
        CHECK(d.first == 800 && d.second == 800);
        CHECK(BatteryPercent(ParseReply(reply(Ok, {0x00, 0xFF}))) == 100);
        CHECK(BatteryPercent(ParseReply(reply(Ok, {0x00, 0x29}))) == 16);
        CHECK(FirmwareText(ParseReply(reply(Ok, {0x02, 0x32}))) == "2.50");
        CHECK(PollingHz(ParseReply(reply(Ok, {0x01}))) == 1000);
        CHECK(HyperPollingHz(ParseReply(reply(Ok, {0x00, 0x08}))) == 1000);
    }
    {
        Light l = ParseEffect(ParseReply(reply(Ok, {0x00, 0x04, 0x03})));
        CHECK(l.effect == EffectSpectrum);
        l = ParseEffect(ParseReply(reply(Ok, {0x01, 0x04, 0x01, 0x00, 0x00, 0x01, 0xFF, 0x80, 0x00})));
        CHECK(l.effect == EffectStatic && l.r == 0xFF && l.g == 0x80 && l.b == 0);
        CHECK(ParseEffect(ParseReply(reply(Ok, {0x00, 0x04, 0x04}))).effect == EffectOther);
    }
    CHECK(ZoneName(0x0A) == "Underglow");
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("tests passed\n");
    return 0;
}
