// IOKit imports. The game reads the keyboard through HID (io_kit::Device:
// a manager matching DeviceUsagePage/DeviceUsage, each device opened, its
// elements put in a queue, the queue drained every frame into
// onUpdateElement(usage page, usage, value)). Here there is one virtual HID
// keyboard, fed by the shell's key events (hid_keyboard_event) instead of the
// real devices, which would need the Input Monitoring permission, and one
// virtual HID mouse that never reports anything: the game takes mouse input
// from MacGetMouseState, but uses keyboard and mouse only when both devices
// exist.
//
// Game pads: PadManagerNative looks once, at startup, for IOHIDDevice services
// with PrimaryUsagePage 1 / PrimaryUsage 5 and makes an io_kit::Joystick of
// each; the joystick's manager matches DeviceUsagePage 1 / DeviceUsage 0 (the
// Mac code passes 0 where a usage belongs) and reads X, Y, Z, Rx, Ry, Rz
// (signed, +-32768 full scale) and buttons 1-128; no hat. There is one virtual
// pad (scimitar/gamepad.h), found when the shell's probe reports a controller
// or SCIMITAR_PAD is set. Its layout is the Xbox 360 controller's as the game's
// pad type 6 reads it (PadIoKit::Update):
//   buttons 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 left stick, 8 right stick,
//           9 Start, 10 Back, 11 Guide, 12 d-pad up, 13 down, 14 left, 15 right
//   axes    X/Y left stick, Rx/Ry right stick (+y down), Z/Rz triggers (0-255)
// That pad type builds a fresh state from the queue every frame, so each time
// the game drains the pad's queue it gets every element's current value.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "bridge.h"
#include "scimitar/gamepad.h"
#include "scimitar/imports.h"
#include "scimitar/runtime.h"

namespace scimitar::imports {

namespace {

constexpr uint32_t kGenericDesktop = 1, kMouseUsage = 2, kJoystickUsage = 4, kGamePadUsage = 5, kKeyboardUsage = 6,
                   kKeyboardPage = 7, kButtonPage = 9;
constexpr uint32_t kHIDDeviceTypeID = 60;  // cf.cpp type_id(Kind::HIDDevice)
constexpr uint32_t kNoMatch = ~0u;

// Handles: managers 0x01d00000+n, queues 0x01d30000+n, values
// 0x01d40000+slot, the pad's service iterator and service 0x01d60001 and
// 0x01d70001, elements 0x01d80000+(usage page << 8)+usage. None is a bridge
// object except the devices (CFGetTypeID is asked about devices).
constexpr uint32_t kManagerBase = 0x01d00000, kQueueBase = 0x01d30000, kValueBase = 0x01d40000,
                   kPadIterator = 0x01d60001, kPadService = 0x01d70001, kElementBase = 0x01d80000;
uint32_t element(uint32_t page, uint32_t usage) { return kElementBase + (page << 8) + usage; }
constexpr uint32_t kValueSlots = 4096;
constexpr uint32_t kPadButtons = 15, kPadAxes = 6;  // buttons 1-15; X, Y, Z, Rx, Ry, Rz (usages 0x30-0x35)

struct Value {
    uint32_t page, usage;
    int32_t value;
};

struct Queue {
    bool pad = false;         // else the keyboard's
    bool started = false;
    std::deque<Value> batch;  // pad: the rest of the current snapshot
    bool draining = false;
};

const bool g_trace = std::getenv("SCIMITAR_TRACE_INPUT") != nullptr;  // HID setup, key and pad events
std::mutex g_lock;
std::map<uint32_t, uint32_t> g_manager_usage;  // manager -> matched DeviceUsage (page 1), or kNoMatch
uint32_t g_next_manager = kManagerBase + 1, g_next_queue = kQueueBase + 1;
uint32_t g_keyboard = 0, g_mouse = 0, g_pad = 0;  // the virtual devices
std::map<uint32_t, Queue> g_queues;               // keyboard and pad queues
std::deque<Value> g_pending;                      // key events not yet read by every queue
Value g_values[kValueSlots];
uint32_t g_next_value = 0;

GamepadProbe g_probe = nullptr;
int g_pad_found = -1;  // -1: not looked for yet
std::string g_pad_name;
bool g_pad_served = false;  // the iterator handed out its one service
GamepadState g_pad_state;
int32_t g_traced[kPadAxes + 1] = {};  // last traced axes and buttons

uint32_t number_value(Context& c, uint32_t dict, const char* key) {
    const bridge::Object* d = bridge::get(c, dict);
    if (!d || d->kind != bridge::Kind::Dictionary) return kNoMatch;
    for (const auto& [k, v] : d->pairs)
        if (bridge::text(c, k) == key)
            if (const bridge::Object* n = bridge::get(c, v)) return (uint32_t)n->number;
    return kNoMatch;
}

// Whether there is a pad, decided the first time the game asks.
bool pad_found() {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_pad_found >= 0) return g_pad_found;
    }
    std::string name = g_probe ? g_probe() : std::string();
    if (name.empty() && std::getenv("SCIMITAR_PAD")) name = "Xbox 360 Controller";
    std::lock_guard<std::mutex> l(g_lock);
    g_pad_found = !name.empty();
    g_pad_name = name;
    if (g_trace) std::fprintf(stderr, "hid: game pad: %s\n", name.empty() ? "none" : name.c_str());
    return g_pad_found;
}

int32_t stick(float v) { return (int32_t)std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f); }
int32_t trigger(float v) { return v < 0.12f ? 0 : (int32_t)std::lround(std::fmin(1.f, v) * 255.f); }

// The pad's elements now, in the layout above (lock held).
std::deque<Value> pad_snapshot() {
    const GamepadState& s = g_pad_state;
    const int32_t axes[kPadAxes] = {stick(s.left_x),       stick(-s.left_y),  trigger(s.left_trigger),
                                    stick(s.right_x),      stick(-s.right_y), trigger(s.right_trigger)};
    using B = GamepadState;
    static const uint32_t kButtons[kPadButtons] = {B::A,     B::B,    B::X,     B::Y,     B::LeftShoulder,
                                                   B::RightShoulder,  B::LeftThumb, B::RightThumb, B::Start,
                                                   B::Back,  B::Guide, B::Up,   B::Down,  B::Left,  B::Right};
    std::deque<Value> out;
    for (uint32_t k = 0; k < kPadAxes; k++) out.push_back({kGenericDesktop, 0x30 + k, axes[k]});
    int32_t bits = 0;
    for (uint32_t k = 0; k < kPadButtons; k++) {
        const bool down = (s.buttons & kButtons[k]) != 0;
        out.push_back({kButtonPage, k + 1, down ? 1 : 0});
        bits |= down ? 1 << k : 0;
    }
    if (g_trace && (!std::equal(axes, axes + kPadAxes, g_traced) || bits != g_traced[kPadAxes])) {
        std::fprintf(stderr, "hid: pad read: buttons %04x (1-15) X %d Y %d Z %d Rx %d Ry %d Rz %d\n", bits, axes[0],
                     axes[1], axes[2], axes[3], axes[4], axes[5]);
        std::copy(axes, axes + kPadAxes, g_traced);
        g_traced[kPadAxes] = bits;
    }
    return out;
}

uint32_t store(const Value& v) {
    const uint32_t slot = g_next_value++ % kValueSlots;
    g_values[slot] = v;
    return kValueBase + slot;
}

}  // namespace

// IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef, IOOptionBits)
void imp_IOHIDManagerCreate(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    const uint32_t m = g_next_manager++;
    g_manager_usage[m] = kNoMatch;
    shim_return(c, m);
}
// void IOHIDManagerSetDeviceMatching(IOHIDManagerRef, CFDictionaryRef)
void imp_IOHIDManagerSetDeviceMatching(Context& c) {
    const uint32_t page = number_value(c, arg32(c, 1), "DeviceUsagePage");
    const uint32_t usage = number_value(c, arg32(c, 1), "DeviceUsage");
    std::lock_guard<std::mutex> l(g_lock);
    g_manager_usage[arg32(c, 0)] = page == kGenericDesktop ? usage : kNoMatch;
    if (g_trace) std::fprintf(stderr, "hid: manager %08x matches page %d usage %d\n", arg32(c, 0), (int)page, (int)usage);
    shim_return(c, 0);
}
// CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef): the keyboard, the mouse or
// the pad when matched, else an empty set (sets are arrays in the bridge).
void imp_IOHIDManagerCopyDevices(Context& c) {
    uint32_t usage;
    {
        std::lock_guard<std::mutex> l(g_lock);
        usage = g_manager_usage[arg32(c, 0)];
    }
    const bool pad = (usage == 0 || usage == kJoystickUsage || usage == kGamePadUsage) && pad_found();
    uint32_t* device = usage == kKeyboardUsage ? &g_keyboard : usage == kMouseUsage ? &g_mouse : pad ? &g_pad : nullptr;
    if (!device) return shim_return(c, bridge::make_array(c, {}));
    if (!*device) *device = bridge::make(c, bridge::Kind::HIDDevice, "IOHIDDevice");
    shim_return(c, bridge::make_array(c, {*device}));
}
void imp_IOHIDDeviceGetTypeID(Context& c) { shim_return(c, kHIDDeviceTypeID); }
// IOReturn IOHIDDeviceOpen(IOHIDDeviceRef, IOOptionBits), IOHIDDeviceClose
void imp_IOHIDDeviceOpen(Context& c) { shim_return(c, 0); }
void imp_IOHIDDeviceClose(Context& c) { shim_return(c, 0); }
// CFArrayRef IOHIDDeviceCopyMatchingElements(IOHIDDeviceRef, CFDictionaryRef, IOOptionBits):
// the keyboard's keys (usages 0x04-0xe7); the mouse's buttons, X, Y and wheel;
// the pad's axes and buttons.
void imp_IOHIDDeviceCopyMatchingElements(Context& c) {
    std::vector<uint32_t> elements;
    if (arg32(c, 0) == g_keyboard)
        for (uint32_t u = 0x04; u <= 0xe7; u++) elements.push_back(element(kKeyboardPage, u));
    if (arg32(c, 0) == g_mouse) {
        for (uint32_t u = 1; u <= 5; u++) elements.push_back(element(kButtonPage, u));
        for (uint32_t u : {0x30u, 0x31u, 0x38u}) elements.push_back(element(kGenericDesktop, u));
    }
    if (arg32(c, 0) == g_pad) {
        for (uint32_t u = 0; u < kPadAxes; u++) elements.push_back(element(kGenericDesktop, 0x30 + u));
        for (uint32_t u = 1; u <= kPadButtons; u++) elements.push_back(element(kButtonPage, u));
    }
    shim_return(c, bridge::make_array(c, std::move(elements)));
}
void imp_IOHIDElementGetUsagePage(Context& c) { shim_return(c, (arg32(c, 0) - kElementBase) >> 8); }
void imp_IOHIDElementGetUsage(Context& c) { shim_return(c, (arg32(c, 0) - kElementBase) & 0xff); }
// IOHIDQueueRef IOHIDQueueCreate(CFAllocatorRef, IOHIDDeviceRef, CFIndex depth, IOOptionBits)
void imp_IOHIDQueueCreate(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    const uint32_t q = g_next_queue++;
    if (arg32(c, 1) == g_keyboard || (g_pad && arg32(c, 1) == g_pad)) g_queues[q].pad = arg32(c, 1) == g_pad;
    shim_return(c, q);
}
void imp_IOHIDQueueAddElement(Context& c) { shim_return(c, 0); }
void imp_IOHIDQueueStart(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    auto q = g_queues.find(arg32(c, 0));
    if (q != g_queues.end()) q->second.started = true;
    if (g_trace) std::fprintf(stderr, "hid: queue %08x started%s\n", arg32(c, 0), q != g_queues.end() && q->second.pad ? " (pad)" : "");
    shim_return(c, 0);
}
void imp_IOHIDQueueStop(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    auto q = g_queues.find(arg32(c, 0));
    if (q != g_queues.end()) q->second.started = false;
    shim_return(c, 0);
}
// IOHIDValueRef IOHIDQueueCopyNextValueWithTimeout(IOHIDQueueRef, CFTimeInterval): the
// next key event, or the next element of the pad's snapshot, or null (never waits).
void imp_IOHIDQueueCopyNextValueWithTimeout(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    static uint32_t polls = 0;
    const uint32_t n = ++polls;
    if (g_trace && (n & (n - 1)) == 0) std::fprintf(stderr, "hid: %u polls (queue %08x)\n", polls, arg32(c, 0));
    auto it = g_queues.find(arg32(c, 0));
    if (it == g_queues.end() || !it->second.started) return shim_return(c, 0);
    Queue& q = it->second;
    if (q.pad) {
        if (!q.draining) q.batch = pad_snapshot(), q.draining = true;
        if (q.batch.empty()) {
            q.draining = false;  // this drain is over; the next starts a new snapshot
            return shim_return(c, 0);
        }
        const uint32_t v = store(q.batch.front());
        q.batch.pop_front();
        return shim_return(c, v);
    }
    if (g_pending.empty()) return shim_return(c, 0);
    const uint32_t v = store(g_pending.front());
    g_pending.pop_front();
    if (g_trace) std::fprintf(stderr, "hid: key %#x %s read\n", g_values[v - kValueBase].usage, g_values[v - kValueBase].value ? "down" : "up");
    shim_return(c, v);
}
void imp_IOHIDValueGetElement(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    const Value& v = g_values[(arg32(c, 0) - kValueBase) % kValueSlots];
    shim_return(c, element(v.page, v.usage));
}
// CFIndex IOHIDValueGetIntegerValue(IOHIDValueRef)
void imp_IOHIDValueGetIntegerValue(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    shim_return(c, (uint32_t)g_values[(arg32(c, 0) - kValueBase) % kValueSlots].value);
}

// CFMutableDictionaryRef IOServiceMatching(const char* name)
void imp_IOServiceMatching(Context& c) {
    bridge::Object d;
    d.mutable_ = true;
    shim_return(c, bridge::make(c, bridge::Kind::Dictionary, "NSMutableDictionary", std::move(d)));
}
// kern_return_t IOServiceGetMatchingServices(mach_port_t, CFDictionaryRef, io_iterator_t* out):
// the pad's service for game pads and joysticks (PrimaryUsagePage 1, PrimaryUsage 4 or 5).
void imp_IOServiceGetMatchingServices(Context& c) {
    const uint32_t page = number_value(c, arg32(c, 1), "PrimaryUsagePage");
    const uint32_t usage = number_value(c, arg32(c, 1), "PrimaryUsage");
    const bool pad = page == kGenericDesktop && (usage == kJoystickUsage || usage == kGamePadUsage) && pad_found();
    if (g_trace) std::fprintf(stderr, "hid: services with page %d usage %d: %s\n", (int)page, (int)usage, pad ? "the pad" : "none");
    if (pad) {
        std::lock_guard<std::mutex> l(g_lock);
        g_pad_served = false;
    }
    c.mem->write<uint32_t>(arg32(c, 2), pad ? kPadIterator : 0);
    shim_return(c, 0);
}
void imp_IOServiceGetMatchingService(Context& c) { shim_return(c, 0); }
// io_object_t IOIteratorNext(io_iterator_t)
void imp_IOIteratorNext(Context& c) {
    std::lock_guard<std::mutex> l(g_lock);
    const bool next = arg32(c, 0) == kPadIterator && !g_pad_served;
    if (next) g_pad_served = true;
    shim_return(c, next ? kPadService : 0);
}
void imp_IOObjectRelease(Context& c) { shim_return(c, 0); }
void imp_IORegistryEntryCreateCFProperty(Context& c) { shim_return(c, 0); }
// kern_return_t IORegistryEntryGetParentEntry(io_registry_entry_t, const io_name_t plane, io_registry_entry_t* parent)
void imp_IORegistryEntryGetParentEntry(Context& c) {
    c.mem->write<uint32_t>(arg32(c, 2), 0);
    shim_return(c, 0xe00002c0);  // kIOReturnNoDevice
}

}  // namespace scimitar::imports

namespace scimitar {

// A key of the virtual HID keyboard went down or up (HID usage, page 7).
// Events wait until the game's keyboard queue has been started.
void hid_keyboard_event(uint32_t usage, bool down) {
    using namespace imports;
    if (usage < 0x04 || usage > 0xe7) return;
    std::lock_guard<std::mutex> l(g_lock);
    if (g_pending.size() < 256) g_pending.push_back({kKeyboardPage, usage, down ? 1 : 0});
}

void set_gamepad_probe(GamepadProbe probe) {
    std::lock_guard<std::mutex> l(imports::g_lock);
    imports::g_probe = probe;
}

void hid_gamepad_state(const GamepadState& s) {
    std::lock_guard<std::mutex> l(imports::g_lock);
    imports::g_pad_state = s;
}

namespace {
std::atomic<GamepadKind> g_kind{GamepadKind::Xbox};
std::atomic<bool> g_selected{false};
}  // namespace
void set_gamepad_kind(GamepadKind kind) { g_kind = kind; }
GamepadKind gamepad_kind() { return g_kind; }
void set_gamepad_selected(bool selected) { g_selected = selected; }
bool gamepad_selected() { return g_selected; }

uint32_t hid_gamepad_device() {
    std::lock_guard<std::mutex> l(imports::g_lock);
    return imports::g_pad;
}

std::string hid_gamepad_name() {
    std::lock_guard<std::mutex> l(imports::g_lock);
    return imports::g_pad_name;
}

namespace {
using namespace imports;
#define S(N) {"_" #N, imp_##N},
const ImportShim kIOKit[] = {
    S(IOHIDManagerCreate) S(IOHIDManagerSetDeviceMatching) S(IOHIDManagerCopyDevices) S(IOHIDDeviceGetTypeID)
    S(IOHIDDeviceOpen) S(IOHIDDeviceClose) S(IOHIDDeviceCopyMatchingElements) S(IOHIDElementGetUsagePage)
    S(IOHIDElementGetUsage) S(IOHIDQueueCreate) S(IOHIDQueueAddElement) S(IOHIDQueueStart) S(IOHIDQueueStop)
    S(IOHIDQueueCopyNextValueWithTimeout) S(IOHIDValueGetElement) S(IOHIDValueGetIntegerValue)
    S(IOServiceMatching) S(IOServiceGetMatchingServices) S(IOServiceGetMatchingService) S(IOIteratorNext)
    S(IOObjectRelease) S(IORegistryEntryCreateCFProperty) S(IORegistryEntryGetParentEntry)
};
#undef S
SCIMITAR_REGISTER_SHIMS(kIOKit);
}  // namespace
}  // namespace scimitar
