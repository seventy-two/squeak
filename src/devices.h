// The devices found and what they report, kept up to date on a worker
// thread so the window never waits on a mouse.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "protocol.h"

namespace sq {

struct Zone {
    uint8_t id = 0;
    std::string name;
    Light light;
    int brightness = 255;  // 0-255
};

struct Device {
    std::wstring path;
    uint16_t pid = 0;
    uint8_t tid = 0x1F;
    std::string name;
    std::string connection;  // "Cable", "Wireless" or ""
    bool asleep = false;
    std::string firmware;
    int battery = -1;  // percent; -1 if it has none
    bool charging = false;
    int pollHz = 0;
    std::optional<Stages> stages;
    std::pair<int, int> dpi{0, 0};
    std::vector<Zone> zones;
};

class Devices {
public:
    explicit Devices(std::function<void()> wake);
    ~Devices();  // writes any lighting still waiting to be saved

    // Refresh reads every device again, in the background.
    void Refresh();
    bool Busy() const { return busy_; }
    // Changed returns the latest devices if they've changed since last asked.
    bool Changed(std::vector<Device>& out);

    // Lighting changes show at once and are saved to the device a moment
    // after the last one, so dragging a slider doesn't wear its memory.
    void SetLight(const Device& d, uint8_t zone, Light l);
    void SetBrightness(const Device& d, uint8_t zone, int value);

private:
    using Clock = std::chrono::steady_clock;
    struct Pending {
        std::wstring path;
        uint8_t tid = 0, zone = 0;
        bool brightness = false;
        Light light;
        int value = 0;
        bool liveDue = false;
        Clock::time_point savedAt;
    };
    void loop();
    void read();
    void writeDue(bool flush);
    void queue(Pending p);

    std::function<void()> wake_;
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false, refresh_ = false, changed_ = false;
    std::atomic<bool> busy_{false};
    std::vector<Device> devices_;
    // Keyed by path, zone, and brightness-or-effect.
    std::map<std::tuple<std::wstring, uint8_t, bool>, Pending> pending_;
};

}  // namespace sq
