#include "devices.h"

#include <algorithm>
#include <set>

#include "hid.h"

namespace sq {
namespace {

constexpr auto kSaveAfter = std::chrono::milliseconds(1500);

struct Known {
    uint16_t pid;
    uint8_t tid;
    const char* connection;
};
// Devices seen so far; anything else from Razer is tried with the usual
// transaction IDs.
const Known kKnown[] = {
    {0x0099, 0x1F, "Cable"},     // Basilisk V3
    {0x00AA, 0x1F, "Cable"},     // Basilisk V3 Pro
    {0x00AB, 0x1F, "Wireless"},  // Basilisk V3 Pro
    {0x00B2, 0x1F, "Cable"},     // DeathAdder V3
    {0x00B6, 0x1F, "Cable"},     // DeathAdder V3 Pro
    {0x00B7, 0x1F, "Wireless"},  // DeathAdder V3 Pro
};

bool ok(Hid& h, const Report& r, Reply& out) { return h.Send(r, out) && out.status == Ok; }

Device readDevice(const HidInfo& info, Hid& h) {
    Device d;
    d.path = info.path;
    d.pid = info.pid;
    d.name = info.product.empty() ? "Razer device" : info.product;
    std::vector<uint8_t> tids = {0x1F, 0x3F, 0xFF};
    for (const auto& k : kKnown)
        if (k.pid == info.pid) {
            tids = {k.tid};
            d.connection = k.connection;
        }
    Reply r;
    bool found = false;
    for (uint8_t tid : tids) {
        if (!h.Send(cmd::Firmware(tid), r)) break;
        if (r.status == Ok) {
            d.tid = tid;
            d.firmware = FirmwareText(r);
            found = true;
            break;
        }
        if (r.status == Timeout) {
            d.tid = tid;
            d.asleep = true;
            return d;
        }
    }
    if (!found) {
        d.asleep = true;
        return d;
    }
    const uint8_t t = d.tid;
    if (ok(h, cmd::Battery(t), r)) {
        d.battery = BatteryPercent(r);
        if (ok(h, cmd::Charging(t), r)) d.charging = r.args[1] != 0;
    }
    if (ok(h, cmd::HyperPolling(t), r) && HyperPollingHz(r)) d.pollHz = HyperPollingHz(r);
    else if (ok(h, cmd::Polling(t), r)) d.pollHz = PollingHz(r);
    if (ok(h, cmd::DpiStages(t), r)) d.stages = ParseStages(r);
    if (ok(h, cmd::Dpi(t), r)) d.dpi = ParseDpi(r);
    for (uint8_t led : ZoneIds()) {
        if (!ok(h, cmd::GetBrightness(t, led, Live), r)) continue;
        Zone z;
        z.id = led;
        z.name = ZoneName(led);
        z.brightness = r.args[2];
        if (ok(h, cmd::GetEffect(t, led, Live), r)) z.light = ParseEffect(r);
        d.zones.push_back(z);
    }
    return d;
}

}  // namespace

Devices::Devices(std::function<void()> wake) : wake_(std::move(wake)) {
    refresh_ = true;
    worker_ = std::thread([this] { loop(); });
}

Devices::~Devices() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void Devices::Refresh() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        refresh_ = true;
    }
    cv_.notify_all();
}

bool Devices::Changed(std::vector<Device>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!changed_) return false;
    changed_ = false;
    out = devices_;
    return true;
}

void Devices::SetLight(const Device& d, uint8_t zone, Light l) {
    Pending p;
    p.path = d.path;
    p.tid = d.tid;
    p.zone = zone;
    p.light = l;
    queue(p);
}

void Devices::SetBrightness(const Device& d, uint8_t zone, int value) {
    Pending p;
    p.path = d.path;
    p.tid = d.tid;
    p.zone = zone;
    p.brightness = true;
    p.value = std::clamp(value, 0, 255);
    queue(p);
}

void Devices::queue(Pending p) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        p.liveDue = true;
        p.savedAt = Clock::now() + kSaveAfter;
        pending_[{p.path, p.zone, p.brightness}] = p;
    }
    cv_.notify_all();
}

void Devices::loop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        // Wake for a refresh, a live write, or the next save falling due.
        Clock::time_point next = Clock::time_point::max();
        bool live = false;
        for (auto& [k, p] : pending_) {
            next = std::min(next, p.savedAt);
            live |= p.liveDue;
        }
        if (!stop_ && !refresh_ && !live) {
            if (next == Clock::time_point::max()) cv_.wait(lock);
            else cv_.wait_until(lock, next);
        }
        if (stop_) {
            lock.unlock();
            writeDue(true);
            return;
        }
        bool doRefresh = refresh_;
        refresh_ = false;
        lock.unlock();
        writeDue(false);
        if (doRefresh) read();
        lock.lock();
    }
}

void Devices::writeDue(bool flush) {
    std::vector<Pending> live, saved;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = Clock::now();
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.liveDue) {
                live.push_back(it->second);
                it->second.liveDue = false;
            }
            if (flush || it->second.savedAt <= now) {
                saved.push_back(it->second);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    auto send = [](const Pending& p, Store s) {
        Hid h(p.path);
        Reply r;
        if (p.brightness) h.Send(cmd::SetBrightness(p.tid, p.zone, s, uint8_t(p.value)), r);
        else h.Send(cmd::SetEffect(p.tid, p.zone, s, p.light.effect, p.light.r, p.light.g, p.light.b), r);
    };
    for (const auto& p : live) send(p, Live);
    for (const auto& p : saved) send(p, Saved);
}

void Devices::read() {
    busy_ = true;
    wake_();
    std::vector<Device> found;
    std::set<uint16_t> seen;  // one control interface per device
    for (const auto& info : FindControlInterfaces()) {
        if (seen.count(info.pid)) continue;
        Hid h(info.path);
        if (!h.Open()) continue;
        Device d = readDevice(info, h);
        // A collection that doesn't answer is some other part of a device,
        // unless it's a wireless one that's asleep.
        if (d.asleep && d.connection != "Wireless") continue;
        seen.insert(info.pid);
        found.push_back(std::move(d));
    }
    // By name, so the order doesn't jump about.
    std::stable_sort(found.begin(), found.end(), [](const Device& a, const Device& b) { return a.name < b.name; });
    {
        std::lock_guard<std::mutex> lock(mu_);
        devices_ = std::move(found);
        changed_ = true;
    }
    busy_ = false;
    wake_();
}

}  // namespace sq
