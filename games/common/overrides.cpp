// Hand-written replacements for game functions, linked in place of their
// translations (scimitar-recomp --override SYMBOL forwards the function to
// scimitar::overrides::ov_SYMBOL). Each keeps the function's guest calling
// convention. Shared by the games; each game's CMake list says which it uses.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "scimitar/gamepad.h"
#include "scimitar/image_loader.h"
#include "scimitar/orbit.h"
#include "scimitar/runtime.h"

namespace scimitar::overrides {

// mg::orbitclient::LibraryLoader::PrivLoadLibrary() (AC2, which links the Orbit
// client statically; Brotherhood has it in libubiorbitapi_r2_loader.dylib,
// replaced by the import shims in platform/src/orbit.cpp), run once
// by LibraryLoader::GetInstance on a zeroed 0x60-byte loader: +0 library
// handle, +4 error code (0 = loaded), +8..+0x5c the 22 MgOrbitdll* functions in
// dlsym order. Orbit is not ported (see docs/PLAN.md): instead of finding the
// launcher and dlopen'ing ubiorbitapi_r2.dylib, install the local replacement.
void ov__ZN2mg11orbitclient13LibraryLoader15PrivLoadLibraryEv(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0);
    c.mem->write<uint32_t>(self + 0, 0x0b17a000);  // any non-null handle
    c.mem->write<uint32_t>(self + 4, 0);
    const uint32_t* fns = scimitar::orbit::functions();
    for (int k = 0; k < scimitar::orbit::kFunctionCount; k++)
        c.mem->write<uint32_t>(self + 8 + 4 * (uint32_t)k, fns[k]);
    scimitar::shim_return(c, 0);
}

// io_kit::Device (a joystick's): +0x20 vendor ID, +0x24 product ID, +0x28
// product name (260 wchar_t). The Mac builds never fill in those three (the
// constructor leaves -1, -1 and whatever memory held), so every pad would get
// the generic type, whose bindings no shipped profile provides. For the
// virtual pad (scimitar/gamepad.h) these report the Xbox 360 controller,
// which PadIoKit's constructor turns into its built-in layout (pad type 6),
// and the pad's name, which the options menu lists. Where the device is:
// AC2 opens every matching device (Device::initDevices; +0x18 an array of
// 12-byte entries, the IOHIDDeviceRef first, +0x1c their count), Brotherhood
// the first one (Device::initDevice; +0x14 the IOHIDDeviceRef).
namespace {
bool is_virtual_pad(scimitar::Context& c, uint32_t device) {
    static const bool single = scimitar::guest_symbol_or_0("__ZN6io_kit6Device10initDeviceEv") != 0;
    const uint32_t pad = scimitar::hid_gamepad_device();
    if (!pad) return false;
    if (single) return c.mem->read<uint32_t>(device + 0x14) == pad;
    const uint32_t devices = c.mem->read<uint32_t>(device + 0x18);
    return devices && c.mem->read<uint16_t>(device + 0x1c) && c.mem->read<uint32_t>(devices) == pad;
}
}  // namespace

// unsigned long io_kit::Device::vendorId() const
void ov__ZNK6io_kit6Device8vendorIdEv(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0);
    scimitar::shim_return(c, is_virtual_pad(c, self) ? scimitar::kGamepadVendor : c.mem->read<uint32_t>(self + 0x20));
}

// unsigned long io_kit::Device::productId() const
void ov__ZNK6io_kit6Device9productIdEv(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0);
    scimitar::shim_return(c, is_virtual_pad(c, self) ? scimitar::kGamepadProduct : c.mem->read<uint32_t>(self + 0x24));
}

// const wchar_t* io_kit::Device::product() const (UTF-32 on the Mac)
void ov__ZNK6io_kit6Device7productEv(scimitar::Context& c) {
    const uint32_t self = scimitar::arg32(c, 0);
    if (is_virtual_pad(c, self)) {
        const std::string name = scimitar::hid_gamepad_name();
        uint32_t n = 0;
        for (size_t i = 0; i < name.size() && n < 259;) {  // UTF-8 -> UTF-32
            const unsigned char b = (unsigned char)name[i];
            const int len = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
            uint32_t cp = len == 1 ? b : b & (0x3f >> (len - 1));
            for (int k = 1; k < len && i + k < name.size(); k++) cp = cp << 6 | ((unsigned char)name[i + k] & 0x3f);
            i += len;
            c.mem->write<uint32_t>(self + 0x28 + 4 * n++, cp);
        }
        c.mem->write<uint32_t>(self + 0x28 + 4 * n, 0);
    }
    scimitar::shim_return(c, self + 0x28);
}

// const char* GetPlatformName(Platform): an entry of the 8-byte name table
// g_PlatformNames (0 "360", 1 "PC", 2 "PC_DX10", 3 "PS3", 4 "LRB", 5 "MAC").
// FireItem::GetVariable answers the UI's PLATFORM variable with
// GetPlatformName(1), "PC". The UI movies are the consoles' too: on "360" and
// "PS3" they draw the controller's buttons, but the menus also drop what
// consoles lack (Exit to Desktop, ...). So while the pad is the selected input
// the in-game HUD (MultiComponentShowingFireItem, which asks once, when a game
// loads) and the memory start screens (MissionActivatorMenu) get "360", or
// "PS3" for a PlayStation controller; TitleMenu, PauseMenu and the material
// cache descriptor, the other caller, keep the real answer.
namespace {
struct PlatformNameCallers {
    uint32_t names = 0, fire_return = 0, hud_vtable = 0, mission_vtable = 0;
};
// Where FireItem::GetVariable calls GetPlatformName: the return address after
// its `call rel32` to it (0 if there is none).
uint32_t return_after_call(uint32_t caller, uint32_t callee) {
    const recomp::Image* img = scimitar::loaded_image();
    const recomp::Function* f = img ? img->function_at(caller) : nullptr;
    if (!f) return 0;
    const uint8_t* p = img->at(f->addr, f->end - f->addr);
    for (uint32_t k = 0; p && k + 5 <= f->end - f->addr; k++) {
        if (p[k] != 0xe8) continue;
        int32_t rel;
        std::memcpy(&rel, p + k + 1, 4);
        if (f->addr + k + 5 + (uint32_t)rel == callee) return f->addr + k + 5;
    }
    return 0;
}
const PlatformNameCallers& platform_name_callers() {
    static const PlatformNameCallers p = [] {
        PlatformNameCallers r;
        using scimitar::guest_symbol_or_0;
        // The table's symbol has internal linkage in Brotherhood (an "L" in the name).
        r.names = guest_symbol_or_0("__ZN8scimitar15g_PlatformNamesE");
        if (!r.names) r.names = scimitar::guest_symbol("__ZN8scimitarL15g_PlatformNamesE");
        r.fire_return = return_after_call(
            guest_symbol_or_0("__ZN8scimitar8FireItem11GetVariableEPN4fire6PlayerEtPKPKwRNS1_20BasicStringInterfaceIwEE"),
            guest_symbol_or_0("__ZN8scimitar15GetPlatformNameENS_8PlatformE"));
        const auto vtable = [](const char* name) { const uint32_t a = guest_symbol_or_0(name); return a ? a + 8 : 0; };
        r.hud_vtable = vtable("__ZTVN8scimitar3ac229MultiComponentShowingFireItemE");
        r.mission_vtable = vtable("__ZTVN8scimitar3ac220MissionActivatorMenuE");
        return r;
    }();
    return p;
}
}  // namespace

void ov__ZN8scimitar15GetPlatformNameENS_8PlatformE(scimitar::Context& c) {
    const PlatformNameCallers& k = platform_name_callers();
    uint32_t platform = scimitar::arg32(c, 0);
    if (k.fire_return && c.mem->read<uint32_t>(c.esp) == k.fire_return) {
        // FireItem::GetVariable(this, ...) keeps a frame: its `this` is at [ebp + 8].
        const uint32_t vtable = c.mem->read<uint32_t>(c.mem->read<uint32_t>(c.ebp + 8));
        // SCIMITAR_BUTTON_PROMPTS=keyboard: the keyboard's prompts with a controller too;
        // =xbox or =playstation: the controller's, with the keyboard too (shell_common.mm sets the kind).
        static const char* const prompts = std::getenv("SCIMITAR_BUTTON_PROMPTS");
        static const bool keyboard_prompts = prompts && std::strcmp(prompts, "keyboard") == 0,
                          pad_prompts = prompts && (std::strcmp(prompts, "xbox") == 0 || std::strcmp(prompts, "playstation") == 0);
        const bool hud = vtable && (vtable == k.hud_vtable || vtable == k.mission_vtable);
        if ((hud && scimitar::gamepad_selected() && !keyboard_prompts) || (hud && pad_prompts))
            platform = scimitar::gamepad_kind() == scimitar::GamepadKind::PlayStation ? 3 : 0;
        // SCIMITAR_UI_PLATFORM=ps3 or xbox (for tests): every movie, menus too, gets that
        // console's answer, with what the menus drop on consoles (Exit to Desktop...).
        static const char* const ui = std::getenv("SCIMITAR_UI_PLATFORM");
        if (ui && std::strcmp(ui, "ps3") == 0) platform = 3;
        if (ui && std::strcmp(ui, "xbox") == 0) platform = 0;
        if (std::getenv("SCIMITAR_TRACE_INPUT"))
            std::fprintf(stderr, "ui: PLATFORM for the FireItem with vtable %08x: %s\n", vtable,
                         (const char*)c.mem->ptr(k.names + 8 * platform));
    }
    scimitar::shim_return(c, k.names + 8 * platform);
}

// int PadManagerNative::GetSelectedPadType() (Brotherhood): the selected
// pad's type, 0 for the keyboard and mouse. LocalizationManager::ReplaceTag
// draws a button tag ([A], [Back]...) in the menus' legends and help texts
// by it, through CanTagUseImage: the keyboard's key in a box (or the kbm_
// art for the mouse), and for a pad the art the button library exports under
// the type's prefix: 3 "360_" (the Xbox 360's), 4 "s2_" (none in the Mac
// library), any other type the unprefixed names. SCIMITAR_BUTTON_PROMPTS
// answers those two callers: =keyboard 0, =xbox or =playstation 3 (the Mac
// library has no PlayStation art); SCIMITAR_UI_PAD_TYPE=<n> (for tests) n.
void ov__ZN8scimitar16PadManagerNative18GetSelectedPadTypeEv(scimitar::Context& c) {
    const uint32_t manager = c.mem->read<uint32_t>(c.mem->read<uint32_t>(scimitar::arg32(c, 0) + 4));
    uint32_t type = c.mem->read<uint32_t>(manager + 0x58c + 0x420 * c.mem->read<uint32_t>(manager + 0x580));
    static const uint32_t tag_image = [] {
        const uint32_t a = guest_symbol_or_0("__ZN8scimitar19LocalizationManager14CanTagUseImageERKNS_16LineSupportedTagE");
        return a ? a + 0x16 : 0;
    }();
    static const uint32_t replace_tag = [] {
        const uint32_t a = guest_symbol_or_0(
            "__ZN8scimitar19LocalizationManager10ReplaceTagERN4Gear15GearBasicStringIwNS1_9TagMarkerILb0EEENS1_"
            "29GearDefaultContainerInterfaceEEERKS6_RKNS_16LineSupportedTagE");
        return a ? a + 0x18 : 0;
    }();
    const uint32_t from = c.mem->read<uint32_t>(c.esp);
    if (from && (from == tag_image || from == replace_tag)) {
        static const char* const prompts = std::getenv("SCIMITAR_BUTTON_PROMPTS");
        static const char* const test = std::getenv("SCIMITAR_UI_PAD_TYPE");
        if (prompts && std::strcmp(prompts, "keyboard") == 0) type = 0;
        if (prompts && (std::strcmp(prompts, "xbox") == 0 || std::strcmp(prompts, "playstation") == 0)) type = 3;
        if (test) type = (uint32_t)std::atoi(test);
    }
    scimitar::shim_return(c, type);
}

// bool MacLaunchExecutable(const char* path, const char* arguments)
// (Brotherhood): starts another program through NSTask; the game uses it for
// its multiplayer executable (acbmp_sf), whose servers are gone. Reports that
// nothing could be started, which the game handles as a failed launch.
void ov_MacLaunchExecutable(scimitar::Context& c) {
    const uint32_t path = scimitar::arg32(c, 0);
    std::fprintf(stderr, "shell: not launching %s (no online play)\n",
                 path ? (const char*)c.mem->ptr(path) : "(null)");
    scimitar::shim_return(c, 0);
}

// bool MacCheckUpdaterLaunch(void) (Brotherhood): looks for the Ubisoft
// launcher's updater and starts it (true: quit, the updater restarts the
// game). There is no launcher here: false, start the game.
void ov_MacCheckUpdaterLaunch(scimitar::Context& c) { scimitar::shim_return(c, 0); }

// bool MacLaunchBundle(...) (Brotherhood): opens another application bundle
// (the launcher, the Uplay browser). Nothing is started.
void ov_MacLaunchBundle(scimitar::Context& c) {
    std::fprintf(stderr, "shell: not launching another application\n");
    scimitar::shim_return(c, 0);
}

}  // namespace scimitar::overrides
