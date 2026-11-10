// The control protocol Razer mice and keyboards speak over 90-byte HID
// feature reports, as documented by OpenRazer. Pure functions only: the
// HID side is in hid.h, so this part tests anywhere.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sq {

// A report as sent with HidD_SetFeature: report ID 0, then the 90 bytes.
using Report = std::array<uint8_t, 91>;

// Status byte of a reply.
enum Status : uint8_t {
    New = 0x00,
    Busy = 0x01,
    Ok = 0x02,
    Failed = 0x03,
    Timeout = 0x04,  // a wireless device that's asleep or out of range
    Unsupported = 0x05,
};

// Where a setting goes: the live one only, or the one the device keeps
// through power cycles.
enum Store : uint8_t { Live = 0x00, Saved = 0x01 };

// Lighting effects (the "extended matrix" set newer devices use).
enum Effect : uint8_t {
    EffectOff = 0x00,
    EffectStatic = 0x01,
    EffectBreathing = 0x02,
    EffectSpectrum = 0x03,
    EffectOther = 0xFF,  // wave, reactive, custom...: shown, not offered
};

// Request builds a report. args are the command's arguments; size is the
// data size the device expects (often larger than args, for replies).
Report Request(uint8_t tid, uint8_t cls, uint8_t id, uint8_t size, const std::vector<uint8_t>& args = {});

// Checksum: XOR of the 90-byte message's bytes 2 to 87.
uint8_t Crc(const Report& r);

// Reply is a device's answer, without the report ID.
struct Reply {
    uint8_t status = 0;
    uint8_t cls = 0, id = 0;
    std::array<uint8_t, 80> args{};
};
Reply ParseReply(const Report& r);

// Commands.
namespace cmd {
Report Firmware(uint8_t tid);
Report DpiStages(uint8_t tid);
Report Dpi(uint8_t tid);
Report Battery(uint8_t tid);
Report Charging(uint8_t tid);
Report Polling(uint8_t tid);       // up to 1000 Hz
Report HyperPolling(uint8_t tid);  // the 8000 Hz kind; unsupported on some
Report GetBrightness(uint8_t tid, uint8_t led, Store s);
Report GetEffect(uint8_t tid, uint8_t led, Store s);
Report SetBrightness(uint8_t tid, uint8_t led, Store s, uint8_t value);
Report SetEffect(uint8_t tid, uint8_t led, Store s, Effect e, uint8_t r, uint8_t g, uint8_t b);
}  // namespace cmd

// Readings.
std::string FirmwareText(const Reply& r);  // e.g. "2.50"
struct Stages {
    int active = 0;  // 1-based, as the device counts
    std::vector<std::pair<int, int>> dpi;  // x, y per stage
};
std::optional<Stages> ParseStages(const Reply& r);
std::pair<int, int> ParseDpi(const Reply& r);
int BatteryPercent(const Reply& r);
int PollingHz(const Reply& r);
int HyperPollingHz(const Reply& r);
struct Light {
    Effect effect = EffectOff;
    uint8_t r = 0, g = 255, b = 0;
};
Light ParseEffect(const Reply& r);

// Lighting zones: the LED IDs worth asking a device about, and their names.
const std::vector<uint8_t>& ZoneIds();
std::string ZoneName(uint8_t led);

}  // namespace sq
