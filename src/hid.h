// Finding Razer devices' control interfaces and talking to them, through
// Windows' own HID API (no Razer driver or software needed).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.h"

namespace sq {

struct HidInfo {
    std::wstring path;
    uint16_t pid = 0;
    std::string product;
};

// FindControlInterfaces lists every Razer (VID 1532) HID collection that
// takes 90-byte feature reports.
std::vector<HidInfo> FindControlInterfaces();

class Hid {
public:
    explicit Hid(const std::wstring& path);
    ~Hid();
    Hid(const Hid&) = delete;
    Hid& operator=(const Hid&) = delete;
    bool Open() const;

    // Send sends a request and waits for its reply (up to about half a
    // second, while the device says it's busy). False if the write or
    // read failed outright; otherwise check reply.status.
    bool Send(const Report& request, Reply& reply);

private:
    void* handle_;
};

// SynapseRunning reports whether Razer's own app is running, as it would
// set its lighting over ours.
bool SynapseRunning();

}  // namespace sq
