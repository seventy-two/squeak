# Squeak

A small Windows window for Razer mice and keyboards, so Razer Synapse
doesn't have to run all the time. Set macros and button maps once in
Synapse (they're kept on the mouse), then close it: Squeak shows each
device's battery, DPI stages, polling rate and firmware, and sets its
lighting.

It talks to the devices directly with Windows' HID API, using the control
protocol OpenRazer documents (90-byte feature reports). No Razer driver or
service is needed.

- **Lighting**: per zone (found by asking the device, so keyboards get
  theirs too) Off, Static, Breathing or Spectrum, a colour and brightness.
  Changes show at once and are saved to the device 1.5s after the last
  one, so it keeps them with Squeak closed and dragging a slider doesn't
  wear its memory.
- **Refreshes** every minute while open, and when something is plugged
  in or out. F5 reads again.
- If Synapse is running it says so, as Synapse puts its own lighting back.

## Building

`build.bat` (Visual Studio 2022 C++ tools) builds `build\squeak.exe` and runs
the protocol tests. The tests also build anywhere:

    g++ -std=c++17 -Isrc tests/tests.cpp src/protocol.cpp -o tests && ./tests

`squeak.exe -selftest <file>` sets each lit device's first zone to static
red through the app's own worker, reads it back, puts it back, and writes
the results to the file.

`squeak.exe -battery <file>` reads every device once, opens no window,
and writes those with a battery to the file as JSON
(`{"devices":[{"name":"…","battery":87,"charging":true}]}`). kitchensink's
Windows client runs it every two minutes to say when a mouse has finished
charging.

## Adding a device

Unknown Razer devices are tried with the usual transaction IDs, so most
work as they are. Known ones (and whether they're on a cable or wireless)
are in `kKnown` in `src/devices.cpp`.
