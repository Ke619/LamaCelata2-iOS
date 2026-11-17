// DirectInput 8 (dinput8!DirectInput8Create): the system keyboard and mouse,
// read from the window's Cocoa events (input.cpp). No joysticks are
// enumerated: game pads go through XInput.
#include <deque>
#include <mutex>

#include "com.h"
#include "win32.h"
#include "win32_window.h"

namespace scimitar::win32::dinput {

using namespace scimitar::win32::com;

constexpr uint32_t DI_OK = 0, DIERR_INVALIDPARAM = 0x80070057u, DIERR_DEVICENOTREG = 0x80040154u,
                   DIERR_UNSUPPORTED = 0x80004001u, DI_BUFFEROVERFLOW = 1;

struct Device : Native {
    bool keyboard = false;
    bool joystick = false;  // the game pad, as Windows lists an Xbox 360 controller
    uint32_t data_size = 0;     // the data format's size (DIMOUSESTATE 16, DIMOUSESTATE2 20)
    uint32_t buffer_size = 0;   // DIPROP_BUFFERSIZE
    bool acquired = false, read = false;  // (traced once)
    uint8_t last_keys[256] = {};
    struct Event {
        uint32_t ofs, data, time, seq;
    };
    std::deque<Event> events;
    uint32_t seq = 0;
};

struct Input : Native {};

uint32_t g_vt_input = 0, g_vt_device = 0;

bool guid_is(uint32_t g, uint32_t first) {
    // GUID_SysKeyboard 6F1D2B61-D5A0-11CF-BFC7-444553540000, GUID_SysMouse ...2B60...
    return rd(g) == first && rd(g + 4) == 0x11cfd5a0u && rd(g + 8) == 0x4544c7bfu && rd(g + 12) == 0x00005453u;
}

// Keyboard changes since the last read become buffered events.
void update_events(Device* d) {
    if (!d->keyboard || !d->buffer_size) return;
    uint8_t keys[256];
    keyboard_state(keys);
    for (uint32_t k = 0; k < 256; k++)
        if (keys[k] != d->last_keys[k]) {
            d->events.push_back({k, keys[k], 0, d->seq++});
            if (d->events.size() > d->buffer_size) d->events.pop_front();
        }
    std::memcpy(d->last_keys, keys, 256);
}

// The game pad as DirectInput lists it: "Controller (XBOX 360 For Windows)",
// vendor 0x045e, product 0x028e (the game then reads it through XInput).
constexpr uint32_t kPadVidPid = 0x028e045e;

bool is_pad_guid(uint32_t g) {
    // Its instance GUID, or its product GUID {028E045E-0000-0000-0000-504944564944} ("PIDVID").
    return (rd(g) == 0x5ca1ab1e && rd(g + 4) == 0) ||
           (rd(g) == kPadVidPid && rd(g + 4) == 0 && rd(g + 8) == 0x49500000u && rd(g + 12) == 0x44495644u);
}

// The DirectInput the game made: IDirectInput8A or IDirectInput8W, whose
// DIDEVICEINSTANCE holds its names as char or WCHAR (580 or 1100 bytes).
bool g_wide = false;
uint32_t instance_size() { return g_wide ? 1100 : 580; }

void put_pad_instance(uint32_t di) {
    const uint32_t size = instance_size(), chars = g_wide ? 520 : 260;
    std::memset(gptr(di), 0, size);
    wr(di, size);
    wr(di + 4, 0x5ca1ab1e);                                       // guidInstance
    wr(di + 20, kPadVidPid), wr(di + 28, 0x49500000u), wr(di + 32, 0x44495644u);  // guidProduct
    wr(di + 36, 0x10215);  // DI8DEVTYPE_GAMEPAD, standard, HID
    for (uint32_t name : {di + 40, di + 40 + chars}) {  // tszInstanceName, tszProductName
        if (g_wide) put_wstr(name, 260, u"Controller (XBOX 360 For Windows)");
        else put_str(name, 260, "Controller (XBOX 360 For Windows)");
    }
    wr16(di + 40 + 2 * chars + 16, 1), wr16(di + 40 + 2 * chars + 18, 5);  // HID usage page 1, usage 5 (game pad)
}

void in_CreateDevice(Context& c) {
    const uint32_t guid = arg(c, 1), out = arg(c, 2);
    auto* d = new Device;
    if (guid_is(guid, 0x6f1d2b61u)) d->keyboard = true;
    else if (is_pad_guid(guid)) d->joystick = true;
    else if (!guid_is(guid, 0x6f1d2b60u)) {
        delete d;
        trace("DirectInput CreateDevice: not the keyboard, mouse or pad");
        return ret(c, DIERR_DEVICENOTREG, 4);
    }
    trace("DirectInput CreateDevice(%s)", d->keyboard ? "keyboard" : d->joystick ? "pad" : "mouse");
    wr(out, create(g_vt_device, d));
    ret(c, DI_OK, 4);
}

// The system mouse or keyboard as DirectInput lists them: GUID_SysMouse or
// GUID_SysKeyboard as both instance and product.
void put_system_instance(uint32_t di, bool keyboard) {
    const uint32_t size = instance_size(), chars = g_wide ? 520 : 260;
    std::memset(gptr(di), 0, size);
    wr(di, size);
    for (uint32_t g : {di + 4, di + 20}) {
        wr(g, keyboard ? 0x6f1d2b61u : 0x6f1d2b60u), wr16(g + 4, 0xd5a0), wr16(g + 6, 0x11cf);
        wr(g + 8, 0x4544c7bfu), wr(g + 12, 0x00005453u);  // BF C7 44 45 53 54 00 00
    }
    wr(di + 36, keyboard ? 0x413 : 0x112);  // DI8DEVTYPE_KEYBOARD (PC enhanced) / DI8DEVTYPE_MOUSE
    const char* name = keyboard ? "Keyboard" : "Mouse";
    for (uint32_t n : {di + 40, di + 40 + chars}) {
        if (g_wide) put_wstr(n, 260, utf16(name));
        else put_str(n, 260, name);
    }
}

void in_EnumDevices(Context& c) {
    // (type, callback, ref, flags): all devices (0) are the mouse, the
    // keyboard and the pad, as Windows lists them; pointers (2), keyboards (3)
    // and game controllers (4) one of them. The callback returns 0
    // (DIENUM_STOP) to end the list.
    const uint32_t type = arg(c, 1), cb = arg(c, 2), ref = arg(c, 3);
    trace("DirectInput EnumDevices(type %u, flags %x)", type, arg(c, 4));
    if (!cb) return ret(c, DI_OK, 5);
    const uint32_t di = galloc(instance_size());
    bool go = true;
    if (type == 0 || type == 2) {
        put_system_instance(di, false);
        go = call_guest(c, cb, {di, ref}) != 0;
    }
    if (go && (type == 0 || type == 3)) {
        put_system_instance(di, true);
        go = call_guest(c, cb, {di, ref}) != 0;
    }
    PadState p;
    if (go && (type == 0 || type == 4) && pad_state(&p)) {
        put_pad_instance(di);
        call_guest(c, cb, {di, ref});
    }
    gfree(di);
    ret(c, DI_OK, 5);
}

void in_GetDeviceStatus(Context& c) { ret(c, DI_OK, 2); }

void dv_GetCapabilities(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    const uint32_t caps = arg(c, 1);
    const uint32_t size = rd(caps);
    std::memset(gptr(caps + 4), 0, size - 4);
    wr(caps + 4, 5);  // DIDC_ATTACHED | DIDC_POLLEDDEVICE?  attached, emulated
    wr(caps + 8, d->joystick ? 0x10215 : d->keyboard ? 0x13 : 0x12);  // gamepad / keyboard / mouse
    wr(caps + 12, d->joystick ? 5 : d->keyboard ? 0 : 3);              // axes
    wr(caps + 16, d->joystick ? 10 : d->keyboard ? 128 : 4);           // buttons
    if (d->joystick) wr(caps + 20, 1);                                  // POVs
    ret(c, DI_OK, 2);
}

// No force feedback: the game asks for rumble effects whether or not the
// capabilities offer them, and goes on without them.
void dv_CreateEffect(Context& c) {
    if (arg(c, 3)) wr(arg(c, 3), 0);
    ret(c, 0x80004001u, 5);  // DIERR_UNSUPPORTED
}
void dv_EnumEffects(Context& c) { ret(c, DI_OK, 4); }

void dv_SetProperty(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    const uint32_t prop = arg(c, 1), header = arg(c, 2);
    if (prop == 1) d->buffer_size = rd(header + 16);  // DIPROP_BUFFERSIZE: DIPROPDWORD.dwData
    ret(c, DI_OK, 3);
}

void dv_GetProperty(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    const uint32_t prop = arg(c, 1), header = arg(c, 2);
    trace("DirectInput GetProperty(%s, %u)", d->joystick ? "pad" : d->keyboard ? "keyboard" : "mouse", prop);
    if (prop == 1) wr(header + 16, d->buffer_size);              // DIPROP_BUFFERSIZE
    if (prop == 24) wr(header + 16, d->joystick ? kPadVidPid : 0);  // DIPROP_VIDPID
    if (prop == 4) wr(header + 16, 0), wr(header + 20, 65535);   // DIPROP_RANGE
    if (prop == 25 && d->joystick) {                             // DIPROP_PRODUCTNAME (wide)
        put_wstr(header + 16, 260, u"Controller (XBOX 360 For Windows)");
    }
    ret(c, DI_OK, 3);
}

const char* kind(const Device* d) { return d->joystick ? "pad" : d->keyboard ? "keyboard" : "mouse"; }

void dv_Acquire(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    if (!d->acquired) trace("DirectInput Acquire(%s)", kind(d));
    d->acquired = true;
    ret(c, DI_OK, 1);
}
void dv_Unacquire(Context& c) { ret(c, DI_OK, 1); }

void dv_GetDeviceState(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    const uint32_t size = arg(c, 1), data = arg(c, 2);
    if (!d->read) trace("DirectInput GetDeviceState(%s, %u bytes): first read", kind(d), size), d->read = true;
    if (d->joystick) {
        // DIJOYSTATE(2): X/Y left stick, Z triggers, Rx/Ry right stick
        // (0..65535, down positive), the d-pad as POV 0, buttons A B X Y LB
        // RB Back Start LS RS.
        PadState p;
        pad_state(&p);
        std::memset(gptr(data), 0, size);
        auto ax = [](int16_t v, bool flip) { return (uint32_t)((flip ? -(int32_t)v : (int32_t)v) + 32768); };
        wr(data, ax(p.lx, false)), wr(data + 4, ax(p.ly, true));
        wr(data + 8, (uint32_t)(32768 + 128 * ((int32_t)p.lt - (int32_t)p.rt)));
        wr(data + 12, ax(p.rx, false)), wr(data + 16, ax(p.ry, true));
        wr(data + 20, 32768);
        const uint16_t b = p.buttons;
        const bool up = b & 1, down = b & 2, left = b & 4, right = b & 8;
        int pov = -1;
        if (up) pov = right ? 4500 : left ? 31500 : 0;
        else if (down) pov = right ? 13500 : left ? 22500 : 18000;
        else if (right) pov = 9000;
        else if (left) pov = 27000;
        wr(data + 32, pov < 0 ? 0xffffffffu : (uint32_t)pov);
        wr(data + 36, 0xffffffffu), wr(data + 40, 0xffffffffu), wr(data + 44, 0xffffffffu);
        const uint16_t bits[10] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0x20, 0x10, 0x40, 0x80};
        for (int k = 0; k < 10 && 48 + k < (int)size; k++) gptr(data + 48 + k)[0] = (b & bits[k]) ? 0x80 : 0;
    } else if (d->keyboard) {
        uint8_t keys[256];
        keyboard_state(keys);
        std::memcpy(gptr(data), keys, std::min<uint32_t>(size, 256));
    } else {
        const MouseState m = take_mouse_state();
        std::memset(gptr(data), 0, size);
        wr(data, (uint32_t)m.dx), wr(data + 4, (uint32_t)m.dy), wr(data + 8, (uint32_t)m.dz);
        for (uint32_t k = 0; k < 4 && 12 + k < size; k++) gptr(data)[12 + k] = m.buttons[k];
    }
    ret(c, DI_OK, 3);
}

void dv_GetDeviceData(Context& c) {
    // (cbObjectData, rgdod, pdwInOut, flags): DIDEVICEOBJECTDATA {ofs, data, timestamp, sequence, appdata}
    Device* d = as<Device>(arg(c, 0));
    const uint32_t each = arg(c, 1), out = arg(c, 2), inout = arg(c, 3), flags = arg(c, 4);
    if (!d->read) trace("DirectInput GetDeviceData(%s): first read", kind(d)), d->read = true;
    update_events(d);
    const uint32_t want = rd(inout);
    uint32_t n = 0;
    for (; n < want && n < d->events.size(); n++)
        if (out) {
            const auto& e = d->events[n];
            const uint32_t p = out + n * each;
            wr(p, e.ofs), wr(p + 4, e.data), wr(p + 8, e.time), wr(p + 12, e.seq);
            if (each >= 20) wr(p + 16, 0);
        }
    if (!(flags & 1)) d->events.erase(d->events.begin(), d->events.begin() + n);  // DIGDD_PEEK
    wr(inout, n);
    ret(c, DI_OK, 5);
}

void dv_SetDataFormat(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    d->data_size = rd(arg(c, 1) + 12);  // DIDATAFORMAT.dwDataSize
    ret(c, DI_OK, 2);
}

void dv_SetEventNotification(Context& c) { ret(c, DI_OK, 2); }
void dv_SetCooperativeLevel(Context& c) { ret(c, DI_OK, 3); }
void dv_Poll(Context& c) { ret(c, DI_OK, 1); }
void dv_EnumObjects(Context& c) {
    trace("DirectInput EnumObjects(flags %x)", arg(c, 3));
    ret(c, DI_OK, 4);
}

void dv_GetDeviceInfo(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    const uint32_t info = arg(c, 1);
    const uint32_t size = rd(info);
    trace("DirectInput GetDeviceInfo(%s, size %u)", d->joystick ? "pad" : d->keyboard ? "keyboard" : "mouse", size);
    std::memset(gptr(info + 4), 0, size - 4);
    if (d->joystick) {
        const uint32_t n = std::min<uint32_t>(size, instance_size());
        const uint32_t tmp = galloc(instance_size());
        put_pad_instance(tmp);
        std::memcpy(gptr(info + 4), gptr(tmp + 4), n - 4);
        gfree(tmp);
        return ret(c, DI_OK, 2);
    }
    const uint32_t n = std::min<uint32_t>(size, instance_size());
    const uint32_t tmp = galloc(instance_size());
    put_system_instance(tmp, d->keyboard);
    std::memcpy(gptr(info + 4), gptr(tmp + 4), n - 4);
    gfree(tmp);
    ret(c, DI_OK, 2);
}

// The keys of a US keyboard by DIK scan code, named as Windows names them
// (GetObjectInfo); codes without a name are no key.
const char* key_name(uint32_t dik) {
    static const char* const low[0x59] = {
        nullptr, "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace", "Tab",
        "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "Enter", "Ctrl", "A", "S",
        "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", "Shift", "\\", "Z", "X", "C", "V",
        "B", "N", "M", ",", ".", "/", "Right Shift", "Num *", "Alt", "Space", "Caps Lock", "F1", "F2", "F3",
        "F4", "F5", "F6", "F7", "F8", "F9", "F10", "Num Lock", "Scroll Lock", "Num 7", "Num 8", "Num 9", "Num -",
        "Num 4", "Num 5", "Num 6", "Num +", "Num 1", "Num 2", "Num 3", "Num 0", "Num Del", nullptr, nullptr,
        nullptr, "F11", "F12"};
    if (dik < 0x59) return low[dik];
    switch (dik) {
    case 0x9c: return "Num Enter";
    case 0x9d: return "Right Ctrl";
    case 0xb5: return "Num /";
    case 0xb7: return "Prnt Scrn";
    case 0xb8: return "Right Alt";
    case 0xc5: return "Pause";
    case 0xc7: return "Home";
    case 0xc8: return "Up";
    case 0xc9: return "Page Up";
    case 0xcb: return "Left";
    case 0xcd: return "Right";
    case 0xcf: return "End";
    case 0xd0: return "Down";
    case 0xd1: return "Page Down";
    case 0xd2: return "Insert";
    case 0xd3: return "Delete";
    case 0xdb: return "Left Windows";
    case 0xdc: return "Right Windows";
    case 0xdd: return "Application";
    default: return nullptr;
    }
}

// GetObjectInfo(DIDEVICEOBJECTINSTANCE*, object, how): the mouse's axes
// (offsets 0, 4, 8) and buttons (12-15), the keyboard's keys (offset = scan
// code), the pad's as DIJOYSTATE lays them out; by offset (DIPH_BYOFFSET) or
// by the type-and-instance ID GetObjectInfo returns (DIPH_BYID).
void dv_GetObjectInfo(Context& c) {
    Device* d = as<Device>(arg(c, 0));
    const uint32_t info = arg(c, 1), obj = arg(c, 2), how = arg(c, 3);
    const uint32_t size = rd(info);
    uint32_t ofs = obj;
    if (how == 2) {  // DIPH_BYID: instance << 8 | type
        const uint32_t inst = (obj >> 8) & 0xffff, type = obj & 0xff;
        if (d->keyboard) ofs = inst;
        else if (type & 3) ofs = 4 * inst;  // axes
        else ofs = (d->joystick ? 48 : 12) + inst;
    } else if (how != 1) {
        return ret(c, 0x80070002u, 4);  // DIERR_OBJECTNOTFOUND
    }
    const bool axis = !d->keyboard && ofs < (d->joystick ? 24u : 12u);
    const bool exists = d->keyboard ? key_name(ofs) != nullptr : d->joystick ? ofs < 58 : ofs < 16;
    if (!exists) return ret(c, 0x80070002u, 4);
    std::memset(gptr(info + 4), 0, size - 4);
    // guidType: GUID_XAxis.. {A36D02E0+n-C9F3-11CF-BFC7-444553540000}, GUID_Button
    // (A36D02F0), GUID_Key {55728220-D33C-11CF-...}.
    const uint32_t g = info + 4;
    if (d->keyboard) wr(g, 0x55728220u), wr16(g + 4, 0xd33c);
    else wr(g, axis ? 0xa36d02e0u + ofs / 4 : 0xa36d02f0u), wr16(g + 4, 0xc9f3);
    wr16(g + 6, 0x11cf), wr(g + 8, 0x4544c7bfu), wr(g + 12, 0x00005453u);
    const uint32_t inst = d->keyboard ? ofs : axis ? ofs / 4 : ofs - (d->joystick ? 48 : 12);
    wr(info + 20, ofs);
    wr(info + 24, inst << 8 | (axis ? (d->joystick ? 2u : 1u) : 0xcu));  // ABSAXIS/RELAXIS or PSHBUTTON|TGLBUTTON
    char name[32];
    static const char* const axes[] = {"X Axis", "Y Axis", "Wheel", "X Rotation", "Y Rotation", "Z Rotation"};
    if (axis) std::snprintf(name, sizeof name, "%s", axes[ofs / 4]);
    else if (d->keyboard) std::snprintf(name, sizeof name, "%s", key_name(ofs));
    else std::snprintf(name, sizeof name, "Button %u", inst);
    if (g_wide) put_wstr(info + 32, 260, utf16(name));
    else put_str(info + 32, 260, name);
    ret(c, DI_OK, 4);
}

void make_vtables() {
    if (g_vt_input) return;
    g_vt_input = make_vtable("IDirectInput8A", {
        {"QueryInterface", QueryInterface}, {"AddRef", AddRef}, {"Release", Release},
        {"CreateDevice", in_CreateDevice}, {"EnumDevices", in_EnumDevices},
        {"GetDeviceStatus", in_GetDeviceStatus}, {"RunControlPanel", nullptr}, {"Initialize", nullptr},
        {"FindDevice", nullptr}, {"EnumDevicesBySemantics", nullptr}, {"ConfigureDevices", nullptr}});
    g_vt_device = make_vtable("IDirectInputDevice8A", {
        {"QueryInterface", QueryInterface}, {"AddRef", AddRef}, {"Release", Release},
        {"GetCapabilities", dv_GetCapabilities}, {"EnumObjects", dv_EnumObjects},
        {"GetProperty", dv_GetProperty}, {"SetProperty", dv_SetProperty}, {"Acquire", dv_Acquire},
        {"Unacquire", dv_Unacquire}, {"GetDeviceState", dv_GetDeviceState},
        {"GetDeviceData", dv_GetDeviceData}, {"SetDataFormat", dv_SetDataFormat},
        {"SetEventNotification", dv_SetEventNotification}, {"SetCooperativeLevel", dv_SetCooperativeLevel},
        {"GetObjectInfo", dv_GetObjectInfo}, {"GetDeviceInfo", dv_GetDeviceInfo}, {"RunControlPanel", nullptr},
        {"Initialize", nullptr}, {"CreateEffect", dv_CreateEffect}, {"EnumEffects", dv_EnumEffects},
        {"GetEffectInfo", nullptr}, {"GetForceFeedbackState", nullptr}, {"SendForceFeedbackCommand", nullptr},
        {"EnumCreatedEffectObjects", nullptr}, {"Escape", nullptr}, {"Poll", dv_Poll},
        {"SendDeviceData", nullptr}, {"EnumEffectsInFile", nullptr}, {"WriteEffectToFile", nullptr},
        {"BuildActionMap", nullptr}, {"SetActionMap", nullptr}, {"GetImageInfo", nullptr}});
}

}  // namespace scimitar::win32::dinput

namespace scimitar::imports {
using namespace scimitar::win32;

W32(dinput8, DirectInput8Create) {
    // (hinst, version, riid, out, outer)
    dinput::make_vtables();
    dinput::g_wide = rd(arg(c, 2)) == 0xbf798031u;  // IID_IDirectInput8W (A: ...30)
    trace("DirectInput8Create(version %x, %s)", arg(c, 1), dinput::g_wide ? "wide" : "ANSI");
    wr(arg(c, 3), com::create(dinput::g_vt_input, new dinput::Input));
    ret(c, 0, 5);
}

}  // namespace scimitar::imports
