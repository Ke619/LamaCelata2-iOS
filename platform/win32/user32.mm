// USER32: the game's one window as a Cocoa window, its message queue, the
// cursor and keyboard state, and message boxes. The game's PeekMessageA pumps
// Cocoa's events and turns them into messages. AppKit runs on the host main
// thread (on_main): AC1 calls USER32 from the guest main thread, which is the
// host main thread; Revelations creates and pumps its window from an engine
// thread, and its guest main thread runs on a host thread of its own while
// the host main thread serves the main queue (games/acr/main.cpp).
//
// SCIMITAR_HIDDEN=1 keeps the window on screen but transparent and
// click-through, with no Dock icon and no focus taken, as the Mac ports do.
//
// Quitting (Cmd-Q, the window's close button) sends the game WM_CLOSE, as
// Alt-F4 does on Windows: the game ends itself (WM_QUIT, ExitProcess). If it
// has not after two seconds the process ends at once; never through exit(),
// whose static destructors would run under the game's threads.
//
// As in the Mac ports: Cmd-Return or Ctrl-Cmd-F toggles full screen (the
// picture keeps the game's aspect, letterboxed), and F6 shows or hides a
// statistics overlay (frame rate, CPU, memory); SCIMITAR_STATS=1 starts with
// it shown. Neither F6 nor keys held with Cmd reach the game.
// SCIMITAR_FULL_SCREEN=1 (the app's launcher, for Full screen) puts the
// window in full screen when it first shows; without it the game starts in a
// window.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <mach/mach.h>
#include <unistd.h>
#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

#include "win32.h"
#include "macho.h"
#include "win32_window.h"

// The window's content: the game's Metal view, as large as fits at the game's
// aspect, centred on black, with the F6 overlay above it.
// Cmd-Q and the close button, to the game (request_quit).
@interface ScimitarAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@interface ScimitarGameView : NSView
@property(nonatomic) NSSize aspect;
@property(nonatomic, strong) NSView* game;
@property(nonatomic, strong) NSTextField* stats;
- (void)layoutGame;
@end

@implementation ScimitarGameView
- (void)layoutGame {
    const NSRect b = self.bounds;
    NSSize a = self.aspect;
    if (a.width <= 0 || a.height <= 0) a = b.size;
    const double s = std::min(b.size.width / a.width, b.size.height / a.height);
    const double w = std::floor(a.width * s), h = std::floor(a.height * s);
    self.game.frame = NSMakeRect(std::floor((b.size.width - w) / 2), std::floor((b.size.height - h) / 2), w, h);
    if (self.stats) {
        const NSRect g = self.game.frame;
        [self.stats setFrameOrigin:NSMakePoint(g.origin.x + 8, g.origin.y + g.size.height - self.stats.frame.size.height - 8)];
    }
}
- (void)resizeSubviewsWithOldSize:(NSSize)old {
    (void)old;
    [self layoutGame];
}
@end

namespace scimitar::win32 {

namespace {

constexpr uint32_t kDesktop = 0x10010, kFirstWindow = 0x20000;
const bool g_hidden = std::getenv("SCIMITAR_HIDDEN") != nullptr;
bool g_full_screen = std::getenv("SCIMITAR_FULL_SCREEN") != nullptr;  // until the window first shows

struct WindowClass {
    uint32_t wndproc = 0, style = 0, extra = 0;
    std::string name;
};

struct Window {
    uint32_t hwnd = 0, wndproc = 0, style = 0, exstyle = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool visible = false;
    std::string title;
    std::map<int32_t, uint32_t> longs;  // SetWindowLongA's other indices (GWL_USERDATA...)
    NSWindow* ns = nil;
    NSView* view = nil;
};

struct Msg {
    uint32_t hwnd, message, wparam, lparam;
};

std::mutex g_lock;
std::map<std::string, WindowClass> g_classes;  // by lower-case name
std::map<uint32_t, Window> g_windows;
uint32_t g_next_window = kFirstWindow;
std::deque<Msg> g_queue;
uint8_t g_keys[256];  // GetKeyboardState: bit 7 down, bit 0 toggled
int g_cursor_count = 0;
bool g_app_ready = false;

// Runs `block` on the host main thread (at once when already on it).
void on_main(void (^block)(void)) {
    if ([NSThread isMainThread]) block();
    else dispatch_sync(dispatch_get_main_queue(), block);
}

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

void post(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l) {
    std::lock_guard<std::mutex> lk(g_lock);
    // Windows keeps one WM_MOUSEMOVE pending: a newer one replaces it.
    if (msg == 0x0200 && !g_queue.empty() && g_queue.back().message == 0x0200 && g_queue.back().hwnd == hwnd) {
        g_queue.back() = {hwnd, msg, w, l};
        return;
    }
    g_queue.push_back({hwnd, msg, w, l});
}

ScimitarAppDelegate* app_delegate() {
    static ScimitarAppDelegate* d = [ScimitarAppDelegate new];
    return d;
}

uint32_t send(Context& c, uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l) {
    uint32_t proc;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end()) return 0;
        proc = it->second.wndproc;
    }
    trace("message %04x to %x (%08x, %08x)", msg, hwnd, w, l);
    return call_guest(c, proc, {hwnd, msg, w, l});
}

// The mouse belongs to the game while it is active, as on Windows in full
// screen: the cursor is hidden and stays put, and DirectInput reads the
// motion. Switching away (Cmd-Tab) gives it back; coming back takes it again.
// SCIMITAR_NO_MOUSE_CAPTURE=1 leaves the mouse alone.
std::atomic<bool> g_captured{false};  // the cursor is hidden and moved by the mouse's motion

void capture_mouse() {
    static const bool off = std::getenv("SCIMITAR_NO_MOUSE_CAPTURE") != nullptr;
    if (off || g_hidden) return;
    static bool observing = false;
    if (!observing) {
        observing = true;
        NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil
                    usingBlock:^(NSNotification*) {
                        g_captured = false;
                        CGAssociateMouseAndMouseCursorPosition(true);
                        [NSCursor unhide];
                    }];
        [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil
                    usingBlock:^(NSNotification*) {
                        g_captured = true;
                        CGAssociateMouseAndMouseCursorPosition(false);
                        [NSCursor hide];
                    }];
    }
    g_captured = true;
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
}

// The game's cursor (GetCursorPos, WM_MOUSEMOVE), in its client pixels and
// inside the client area: moved by the mouse's motion while captured (the
// window's width in points spans the game's width), else where the pointer
// is over the picture. Mouse moves and buttons become window messages as on
// Windows: AC1's menus follow WM_MOUSEMOVE.
void mouse_message(uint32_t hwnd, NSEvent* e) {
    int w, h;
    window_size(hwnd, &w, &h);
    NSWindow* ns = nil;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end()) ns = it->second.ns;
    }
    NSView* game = [ns.contentView isKindOfClass:ScimitarGameView.class] ? ((ScimitarGameView*)ns.contentView).game : ns.contentView;
    if (!game || w <= 0 || h <= 0) return;
    const NSSize vs = game.bounds.size;
    if (vs.width <= 0 || vs.height <= 0) return;
    int x, y;
    if (g_captured) {
        static double fx = -1, fy = -1;  // the position with its fraction
        cursor_position(&x, &y);
        if ((int)fx != x || (int)fy != y) fx = x, fy = y;
        fx = std::clamp(fx + e.deltaX * w / vs.width, 0.0, (double)(w - 1));
        fy = std::clamp(fy + e.deltaY * h / vs.height, 0.0, (double)(h - 1));
        x = (int)fx, y = (int)fy;
    } else {
        if (e.window != ns) return;
        const NSPoint p = [game convertPoint:e.locationInWindow fromView:nil];
        x = std::clamp((int)(p.x / vs.width * w), 0, w - 1);
        y = std::clamp((int)((vs.height - p.y) / vs.height * h), 0, h - 1);
    }
    set_cursor_position(x, y);
    const NSUInteger pressed = NSEvent.pressedMouseButtons;
    const uint32_t keys = (pressed & 1 ? 1u : 0) | (pressed & 2 ? 2u : 0);  // MK_LBUTTON, MK_RBUTTON
    uint32_t msg = 0x0200;  // WM_MOUSEMOVE
    switch (e.type) {
    case NSEventTypeLeftMouseDown: msg = 0x0201; break;
    case NSEventTypeLeftMouseUp: msg = 0x0202; break;
    case NSEventTypeRightMouseDown: msg = 0x0204; break;
    case NSEventTypeRightMouseUp: msg = 0x0205; break;
    default: break;
    }
    post(hwnd, msg, keys, (uint32_t)y << 16 | ((uint32_t)x & 0xffff));
}

void app_init() {
    if (g_app_ready) return;
    g_app_ready = true;
    on_main(^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:g_hidden ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
        static id<NSObject> activity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                              reason:@"game"];
        (void)activity;
        NSMenu* bar = [[NSMenu alloc] init];
        NSMenuItem* app = [[NSMenuItem alloc] init];
        [bar addItem:app];
        NSMenu* menu = [[NSMenu alloc] init];
        [menu addItemWithTitle:@"Quit Assassin's Creed" action:@selector(terminate:) keyEquivalent:@"q"];
        app.submenu = menu;
        NSApp.delegate = app_delegate();
        NSMenuItem* view = [[NSMenuItem alloc] init];
        [bar addItem:view];
        NSMenu* view_menu = [[NSMenu alloc] initWithTitle:@"View"];
        [view_menu addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"\r"];
        NSMenuItem* fs = [view_menu addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:)
                                       keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        view.submenu = view_menu;
        NSApp.mainMenu = bar;
        [NSApp finishLaunching];
    });
}

// ---- the F6 statistics overlay (main thread) ----

constexpr unsigned short kKeyF6 = 97;

ScimitarGameView* game_view_of(NSWindow* w) {
    return [w.contentView isKindOfClass:ScimitarGameView.class] ? (ScimitarGameView*)w.contentView : nil;
}

// Frame rate and time, process CPU (100% = one core) and memory, at most twice a second.
void update_stats(ScimitarGameView* v, bool force) {
    if (!v.stats || v.stats.hidden) return;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static uint64_t last_frames = presented_frames();
    static double last_cpu = 0;
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    if (!force && dt < 0.5) return;
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
    const uint64_t frames = presented_frames();
    const double fps = dt > 0 ? (frames - last_frames) / dt : 0;
    const double cpu_pct = dt > 0 && last_cpu > 0 ? (cpu - last_cpu) / dt * 100 : 0;
    last = now, last_frames = frames, last_cpu = cpu;
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
    const double MB = 1024.0 * 1024.0;
    char text[256];
    std::snprintf(text, sizeof text,
                  "FPS  %5.1f   %5.2f ms\n"
                  "CPU  %5.0f%%   (%d cores)\n"
                  "RAM  %5.0f MB footprint, %.0f MB resident",
                  fps, fps > 0 ? 1000 / fps : 0.0, cpu_pct, (int)NSProcessInfo.processInfo.activeProcessorCount,
                  vm.phys_footprint / MB, vm.resident_size / MB);
    v.stats.stringValue = @(text);
    [v.stats sizeToFit];
    [v layoutGame];
}

void toggle_stats(ScimitarGameView* v) {
    if (!v) return;
    if (!v.stats) {
        NSTextField* t = [NSTextField labelWithString:@""];
        t.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium];
        t.textColor = NSColor.whiteColor;
        t.drawsBackground = YES;
        t.backgroundColor = [NSColor colorWithWhite:0 alpha:0.55];
        t.usesSingleLineMode = NO;
        t.maximumNumberOfLines = 0;
        t.hidden = YES;
        [v addSubview:t positioned:NSWindowAbove relativeTo:v.game];
        v.stats = t;
    }
    v.stats.hidden = !v.stats.hidden;
    update_stats(v, true);
}

// Mac virtual key codes to Windows virtual keys (0: none).
uint8_t vk_of(unsigned short k) {
    static const uint8_t kMap[128] = {
        'A', 'S', 'D', 'F', 'H', 'G', 'Z', 'X', 'C', 'V', 0, 'B', 'Q', 'W', 'E', 'R',          // 0x00
        'Y', 'T', '1', '2', '3', '4', '6', '5', 0xbb, '9', '7', 0xbd, '8', '0', 0xdd, 'O',     // 0x10
        'U', 0xdb, 'I', 'P', 0x0d, 'L', 'J', 0xde, 'K', 0xba, 0xdc, 0xbc, 0xbf, 'N', 'M', 0xbe, // 0x20
        0x09, 0x20, 0xc0, 0x08, 0, 0x1b, 0, 0x5b, 0x10, 0x14, 0x12, 0x11, 0x10, 0x12, 0x11, 0, // 0x30
        0, 0x6e, 0, 0x6a, 0, 0x6b, 0, 0x90, 0, 0, 0, 0x6f, 0x0d, 0, 0x6d, 0,                  // 0x40
        0, 0, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0, 0x68, 0x69, 0, 0, 0,         // 0x50
        0x74, 0x75, 0x76, 0x72, 0x77, 0x78, 0, 0x7a, 0, 0x7c, 0, 0x7d, 0, 0x79, 0, 0x7b,      // 0x60
        0, 0x7e, 0x2d, 0x24, 0x21, 0x2e, 0x73, 0x23, 0x71, 0x22, 0x70, 0x25, 0x27, 0x28, 0x26, 0, // 0x70
    };
    return k < 128 ? kMap[k] : 0;
}

void pump_cocoa(Context& c) {
    (void)c;
    if (!g_app_ready) return;
    on_main(^{
        for (;;) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!e) break;
            uint32_t hwnd = 0;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                for (auto& [h, w] : g_windows)
                    if (w.ns && w.ns == e.window) hwnd = h;
                if (!hwnd && !g_windows.empty()) hwnd = g_windows.begin()->first;
            }
            switch (e.type) {
            case NSEventTypeKeyDown:
            case NSEventTypeKeyUp: {
                const uint8_t vk = vk_of(e.keyCode);
                const bool down = e.type == NSEventTypeKeyDown;
                if (e.keyCode == kKeyF6) {
                    if (down && !e.isARepeat) toggle_stats(game_view_of(e.window ?: NSApp.keyWindow));
                    break;
                }
                if (down && (e.modifierFlags & NSEventModifierFlagCommand)) {
                    [NSApp sendEvent:e];  // the menus' shortcuts: Cmd-Q, Cmd-Return
                    break;
                }
                if (vk) {
                    g_keys[vk] = down ? 0x80 : 0;
                    key_event(e.keyCode, down);
                    if (hwnd) post(hwnd, down ? 0x100 : 0x101, vk, down ? (e.isARepeat ? 0x40000001u : 1u) : 0xc0000001u);
                }
                break;
            }
            case NSEventTypeFlagsChanged: {
                const NSEventModifierFlags f = e.modifierFlags;
                const struct { unsigned short code; NSEventModifierFlags flag; } mods[] = {
                    {0x38, NSEventModifierFlagShift}, {0x3c, NSEventModifierFlagShift},
                    {0x3b, NSEventModifierFlagControl}, {0x3e, NSEventModifierFlagControl},
                    {0x3a, NSEventModifierFlagOption}, {0x3d, NSEventModifierFlagOption}};
                for (const auto& m : mods)
                    if (m.code == e.keyCode) {
                        const bool down = (f & m.flag) != 0;
                        g_keys[vk_of(m.code)] = down ? 0x80 : 0;
                        key_event(m.code, down);
                        if (hwnd) post(hwnd, down ? 0x100 : 0x101, vk_of(m.code), down ? 1u : 0xc0000001u);
                    }
                break;
            }
            case NSEventTypeLeftMouseDown: case NSEventTypeLeftMouseUp:
            case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp:
            case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged:
                mouse_event((int)e.type, e.deltaX, e.deltaY);
                if (hwnd) mouse_message(hwnd, e);
                [NSApp sendEvent:e];
                break;
            case NSEventTypeScrollWheel:
                mouse_wheel(e.scrollingDeltaY);
                break;
            default:
                [NSApp sendEvent:e];
                break;
            }
        }
        if (NSWindow* w = NSApp.keyWindow ?: NSApp.windows.firstObject) update_stats(game_view_of(w), false);
    });
}

}  // namespace

NSView* window_view(uint32_t hwnd) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) {
        if (g_windows.empty()) return nil;
        it = g_windows.begin();
    }
    return it->second.view;
}

void window_size(uint32_t hwnd, int* w, int* h) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) {
        *w = 1280, *h = 720;
        return;
    }
    *w = it->second.w, *h = it->second.h;
}

void window_resized(Context& c, uint32_t hwnd, int w, int h) {
    NSWindow* ns = nil;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_windows.find(hwnd);
        if (it == g_windows.end() || (it->second.w == w && it->second.h == h)) return;
        it->second.w = w, it->second.h = h;
        ns = it->second.ns;
    }
    if (ns)
        on_main(^{
            ns.contentAspectRatio = NSMakeSize(w, h);
            if (ScimitarGameView* v = game_view_of(ns)) v.aspect = NSMakeSize(w, h), [v layoutGame];
        });
    trace("window %x resized to %dx%d", hwnd, w, h);
    send(c, hwnd, 0x0005, 0, (uint32_t)(h << 16 | (w & 0xffff)));  // WM_SIZE, SIZE_RESTORED
}

void window_key(unsigned short mac_keycode, bool down) {
    key_event(mac_keycode, down);
    const uint8_t vk = vk_of(mac_keycode);
    if (!vk) return;
    uint32_t hwnd = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_keys[vk] = down ? 0x80 : 0;
        if (!g_windows.empty()) hwnd = g_windows.begin()->first;
    }
    if (hwnd) post(hwnd, down ? 0x100 : 0x101, vk, down ? 1u : 0xc0000001u);
}

void request_quit() {
    uint32_t hwnd = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_windows.empty()) hwnd = g_windows.begin()->first;
    }
    if (hwnd) post(hwnd, 0x0010, 0, 0);  // WM_CLOSE
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * (int64_t)NSEC_PER_SEC),
                   dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                       std::fflush(nullptr);
                       _exit(0);
                   });
}

}  // namespace scimitar::win32

@implementation ScimitarAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    scimitar::win32::request_quit();
    return NSTerminateCancel;
}
- (BOOL)windowShouldClose:(NSWindow*)window {
    (void)window;
    scimitar::win32::request_quit();
    return NO;
}
@end

namespace scimitar::imports {
using namespace scimitar::win32;

W32(user32, RegisterClassExA) {
    const uint32_t wc = arg(c, 0);
    WindowClass k;
    k.style = rd(wc + 4);
    k.wndproc = rd(wc + 8);
    const uint32_t name = rd(wc + 40);
    k.name = name < 0x10000 ? "#" + std::to_string(name) : gstr(name);
    trace("RegisterClassExA(%s, wndproc %08x)", k.name.c_str(), k.wndproc);
    std::lock_guard<std::mutex> lk(g_lock);
    g_classes[lower(k.name)] = k;
    ret(c, 0xc000 + (uint32_t)g_classes.size(), 1);
}

W32(user32, CreateWindowExA) {
    const uint32_t exstyle = arg(c, 0), cls = arg(c, 1), title = arg(c, 2), style = arg(c, 3);
    const int x = (int)arg(c, 4), y = (int)arg(c, 5);
    int w = (int)arg(c, 6), h = (int)arg(c, 7);
    const uint32_t param = arg(c, 11);
    const std::string name = cls < 0x10000 ? "#" + std::to_string(cls) : gstr(cls);
    Window win;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_classes.find(lower(name));
        if (it == g_classes.end()) {
            set_last_error(1407);  // ERROR_CANNOT_FIND_WND_CLASS
            return ret(c, 0, 12);
        }
        win.wndproc = it->second.wndproc;
        win.hwnd = g_next_window;
        g_next_window += 0x10;
    }
    if (w <= 0 || w == (int)0x80000000) w = 1280;
    if (h <= 0 || h == (int)0x80000000) h = 720;
    win.style = style, win.exstyle = exstyle, win.x = x, win.y = y, win.w = w, win.h = h;
    win.title = gstr(title);
    trace("CreateWindowExA(%s, \"%s\", %08x, %dx%d)", name.c_str(), win.title.c_str(), style, w, h);
    app_init();
    Window* wp = &win;
    on_main(^{
        // The window fits the screen (a full-screen game asks for the
        // desktop's size); the game keeps drawing at the size it asked for,
        // which the layer scales to the window.
        const NSRect vis = NSScreen.mainScreen ? NSScreen.mainScreen.visibleFrame : NSMakeRect(0, 0, w, h);
        const double scale = std::min({1.0, vis.size.width * 0.95 / w, (vis.size.height - 30) * 0.95 / h});
        NSWindow* ns = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, std::floor(w * scale), std::floor(h * scale))
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        ns.contentAspectRatio = NSMakeSize(w, h);
        ns.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        ns.delegate = app_delegate();
        ns.title = [NSString stringWithUTF8String:wp->title.c_str()] ?: @"Assassin's Creed";
        ns.acceptsMouseMovedEvents = YES;
        ns.backgroundColor = NSColor.blackColor;
        ns.releasedWhenClosed = NO;
        ScimitarGameView* content = [[ScimitarGameView alloc] initWithFrame:ns.contentView.bounds];
        content.wantsLayer = YES;
        content.layer.backgroundColor = NSColor.blackColor.CGColor;
        content.aspect = NSMakeSize(w, h);
        NSView* view = [[NSView alloc] initWithFrame:content.bounds];
        view.wantsLayer = YES;
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.backgroundColor = NSColor.blackColor.CGColor;
        view.layer = layer;
        [content addSubview:view];
        content.game = view;
        ns.contentView = content;
        [content layoutGame];
        if (std::getenv("SCIMITAR_STATS")) toggle_stats(content);
        [ns center];
        wp->ns = ns;
        wp->view = view;
    });
    const uint32_t hwnd = win.hwnd;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_windows[hwnd] = win;
    }
    set_cursor_position(w / 2, h / 2);
    // CREATESTRUCTA for WM_NCCREATE and WM_CREATE.
    const uint32_t cs = galloc(48);
    wr(cs, param), wr(cs + 4, process().image->image_base()), wr(cs + 8, arg(c, 9)), wr(cs + 12, arg(c, 8));
    wr(cs + 16, (uint32_t)h), wr(cs + 20, (uint32_t)w), wr(cs + 24, (uint32_t)y), wr(cs + 28, (uint32_t)x);
    wr(cs + 32, style), wr(cs + 36, title), wr(cs + 40, cls), wr(cs + 44, exstyle);
    if (!send(c, hwnd, 0x0081, 0, cs)) trace("WM_NCCREATE returned 0");  // WM_NCCREATE
    send(c, hwnd, 0x0001, 0, cs);                                          // WM_CREATE
    send(c, hwnd, 0x0005, 0, (uint32_t)(h << 16 | (w & 0xffff)));          // WM_SIZE
    if (style & 0x10000000u) {  // WS_VISIBLE
        std::lock_guard<std::mutex> lk(g_lock);
        g_windows[hwnd].visible = true;
    }
    ret(c, hwnd, 12);
}

W32(user32, ShowWindow) {
    const uint32_t hwnd = arg(c, 0), cmd = arg(c, 1);
    NSWindow* ns = nil;
    bool was = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end()) {
            ns = it->second.ns;
            was = it->second.visible;
            it->second.visible = cmd != 0;
        }
    }
    trace("ShowWindow(%x, %u)", hwnd, cmd);
    on_main(^{
        if (ns && cmd) {
            if (g_hidden) {
                ns.alphaValue = 0;
                ns.ignoresMouseEvents = YES;
                ns.hasShadow = NO;
                [ns orderFrontRegardless];
            } else {
                [ns makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
                if (g_full_screen) {
                    g_full_screen = false;
                    if (!(ns.styleMask & NSWindowStyleMaskFullScreen)) [ns toggleFullScreen:nil];
                }
                capture_mouse();
            }
        } else if (ns) {
            [ns orderOut:nil];
        }
    });
    // Showing a top-level window activates it, as on Windows: the game takes
    // the focus (Prince of Persia acquires its DirectInput devices on
    // WM_SETFOCUS). The game keeps it while it runs, hidden or not.
    if (ns && cmd && !was) {
        send(c, hwnd, 0x001c, 1, 0);  // WM_ACTIVATEAPP
        send(c, hwnd, 0x0006, 1, 0);  // WM_ACTIVATE, WA_ACTIVE
        send(c, hwnd, 0x0007, 0, 0);  // WM_SETFOCUS
    }
    ret(c, was, 2);
}

W32(user32, UpdateWindow) { ret(c, 1, 1); }

W32(user32, DestroyWindow) {
    const uint32_t hwnd = arg(c, 0);
    send(c, hwnd, 0x0002, 0, 0);  // WM_DESTROY
    NSWindow* ns = nil;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_windows.find(hwnd);
        if (it != g_windows.end()) {
            ns = it->second.ns;
            g_windows.erase(it);
        }
    }
    if (ns) on_main(^{ [ns orderOut:nil]; });
    ret(c, 1, 1);
}

W32(user32, IsWindow) {
    std::lock_guard<std::mutex> lk(g_lock);
    ret(c, g_windows.count(arg(c, 0)) ? 1 : 0, 1);
}

W32(user32, IsWindowVisible) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_windows.find(arg(c, 0));
    ret(c, it != g_windows.end() && it->second.visible, 1);
}

W32(user32, IsIconic) { ret(c, 0, 1); }

W32(user32, GetDesktopWindow) { ret(c, kDesktop, 0); }

W32(user32, DefWindowProcA) {
    const uint32_t msg = arg(c, 1);
    uint32_t r = 0;
    if (msg == 0x0081) r = 1;  // WM_NCCREATE
    if (msg == 0x0010) {       // WM_CLOSE
        post(arg(c, 0), 0x0012, 0, 0);
    }
    ret(c, r, 4);
}

W32(user32, PeekMessageA) {
    const uint32_t msg = arg(c, 0), remove = arg(c, 4);
    pump_cocoa(c);
    Msg m{};
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_queue.empty()) return ret(c, 0, 5);
        m = g_queue.front();
        if (remove & 1) g_queue.pop_front();  // PM_REMOVE
    }
    wr(msg, m.hwnd), wr(msg + 4, m.message), wr(msg + 8, m.wparam), wr(msg + 12, m.lparam);
    wr(msg + 16, (uint32_t)(clock() / (CLOCKS_PER_SEC / 1000)));
    wr(msg + 20, 0), wr(msg + 24, 0);
    ret(c, 1, 5);
}

W32(user32, TranslateMessage) {
    const uint32_t msg = arg(c, 0);
    const uint32_t m = rd(msg + 4), vk = rd(msg + 8);
    if (m == 0x100) {
        uint32_t ch = 0;
        const bool shift = g_keys[0x10] & 0x80;
        if (vk >= 'A' && vk <= 'Z') ch = shift ? vk : vk + 32;
        else if (vk >= '0' && vk <= '9' && !shift) ch = vk;
        else if (vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x1b || vk == 0x09) ch = vk;
        if (ch) post(rd(msg), 0x102, ch, rd(msg + 12));  // WM_CHAR
    }
    ret(c, 1, 1);
}

W32(user32, DispatchMessageA) {
    const uint32_t msg = arg(c, 0);
    const uint32_t r = send(c, rd(msg), rd(msg + 4), rd(msg + 8), rd(msg + 12));
    ret(c, r, 1);
}

W32(user32, PostQuitMessage) {
    post(0, 0x0012, arg(c, 0), 0);
    ret(c, 0, 1);
}

W32(user32, GetClientRect) {
    int w, h;
    window_size(arg(c, 0), &w, &h);
    const uint32_t r = arg(c, 1);
    wr(r, 0), wr(r + 4, 0), wr(r + 8, (uint32_t)w), wr(r + 12, (uint32_t)h);
    ret(c, 1, 2);
}

W32(user32, GetWindowRect) {
    int w, h;
    window_size(arg(c, 0), &w, &h);
    const uint32_t r = arg(c, 1);
    wr(r, 0), wr(r + 4, 0), wr(r + 8, (uint32_t)w), wr(r + 12, (uint32_t)h);
    ret(c, 1, 2);
}

W32(user32, AdjustWindowRect) { ret(c, 1, 3); }  // no borders: the client area is the window

W32(user32, ScreenToClient) { ret(c, 1, 2); }

W32(user32, GetCursorPos) {
    int x, y;
    cursor_position(&x, &y);
    wr(arg(c, 0), (uint32_t)x), wr(arg(c, 0) + 4, (uint32_t)y);
    ret(c, 1, 1);
}

W32(user32, ClipCursor) { ret(c, 1, 1); }

W32(user32, ShowCursor) {
    g_cursor_count += arg(c, 0) ? 1 : -1;
    ret(c, (uint32_t)g_cursor_count, 1);
}

W32(user32, LoadCursorA) { ret(c, 0x10020, 2); }

namespace {
void screen_size(int* w, int* h) {
    on_main(^{
        NSScreen* s = NSScreen.mainScreen;
        *w = s ? (int)s.frame.size.width : 1920;
        *h = s ? (int)s.frame.size.height : 1080;
    });
}
}  // namespace

W32(user32, GetSystemMetrics) {
    int w, h;
    screen_size(&w, &h);
    uint32_t r = 0;
    switch (arg(c, 0)) {
    case 0: case 16: case 78: r = (uint32_t)w; break;  // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 17: case 79: r = (uint32_t)h; break;
    case 80: r = 1; break;                             // SM_CMONITORS
    case 43: r = 3; break;                             // SM_CMOUSEBUTTONS
    case 19: r = 1; break;                             // SM_MOUSEPRESENT
    default: break;
    }
    ret(c, r, 1);
}

W32(user32, SystemParametersInfoA) {
    const uint32_t action = arg(c, 0), p = arg(c, 2);
    if (action == 0x30 && p) {  // SPI_GETWORKAREA
        int w, h;
        screen_size(&w, &h);
        wr(p, 0), wr(p + 4, 0), wr(p + 8, (uint32_t)w), wr(p + 12, (uint32_t)h);
    } else if (p && (action == 0x3a || action == 0x34 || action == 0x32)) {
        // SPI_GETSTICKYKEYS / TOGGLEKEYS / FILTERKEYS: structures with a size first.
        const uint32_t size = rd(p);
        std::memset(gptr(p + 4), 0, size > 4 ? size - 4 : 0);
    }
    ret(c, 1, 4);
}

W32(user32, EnumDisplayMonitors) {
    // One monitor: callback(hmonitor, hdc, rect, data).
    const uint32_t proc = arg(c, 2), data = arg(c, 3);
    int w, h;
    screen_size(&w, &h);
    const uint32_t r = galloc(16);
    wr(r, 0), wr(r + 4, 0), wr(r + 8, (uint32_t)w), wr(r + 12, (uint32_t)h);
    if (proc) call_guest(c, proc, {0x10030, 0, r, data});
    ret(c, 1, 4);
}

W32(user32, GetKeyboardState) {
    std::memcpy(gptr(arg(c, 0)), g_keys, 256);
    ret(c, 1, 1);
}

W32(user32, ToUnicode) {
    // (vk, scan, state, buf, cap, flags): letters, digits and space.
    const uint32_t vk = arg(c, 0), state = arg(c, 2), buf = arg(c, 3), cap = arg(c, 4);
    const bool shift = state && (gptr(state)[0x10] & 0x80);
    uint32_t ch = 0;
    if (vk >= 'A' && vk <= 'Z') ch = shift ? vk : vk + 32;
    else if (vk >= '0' && vk <= '9') ch = vk;
    else if (vk == 0x20) ch = ' ';
    if (!ch || !cap) return ret(c, 0, 6);
    wr16(buf, (uint16_t)ch);
    ret(c, 1, 6);
}

W32(user32, SetWindowsHookExA) {
    trace("SetWindowsHookExA(%d): not installed", (int)arg(c, 0));
    ret(c, 0x10040, 4);
}
W32(user32, UnhookWindowsHookEx) { ret(c, 1, 1); }
W32(user32, CallNextHookEx) { ret(c, 0, 4); }

W32(user32, MessageBoxA) {
    std::fprintf(stderr, "%s: message box \"%s\": %s\n", process().game_id.c_str(), gstr(arg(c, 2)).c_str(),
                 gstr(arg(c, 1)).c_str());
    const uint32_t type = arg(c, 3) & 0xf;
    ret(c, type == 4 || type == 3 ? 6 : 1, 4);  // IDYES for yes/no boxes, else IDOK
}

W32(user32, LoadStringA) {
    // The executable's string table (resources): not read yet.
    trace("LoadStringA(%u)", arg(c, 1));
    if (arg(c, 3)) put_str(arg(c, 2), arg(c, 3), "");
    ret(c, 0, 4);
}

// ---- Prince of Persia's additions: the older class registration, window
// longs, positions and the rest of what its window code asks ----

W32(user32, RegisterClassA) {
    // WNDCLASSA: style, lpfnWndProc, cbClsExtra, cbWndExtra, hInstance, hIcon,
    // hCursor, hbrBackground, lpszMenuName, lpszClassName.
    const uint32_t wc = arg(c, 0);
    WindowClass k;
    k.style = rd(wc);
    k.wndproc = rd(wc + 4);
    const uint32_t name = rd(wc + 36);
    k.name = name < 0x10000 ? "#" + std::to_string(name) : gstr(name);
    trace("RegisterClassA(%s, wndproc %08x)", k.name.c_str(), k.wndproc);
    std::lock_guard<std::mutex> lk(g_lock);
    g_classes[lower(k.name)] = k;
    ret(c, 0xc000 + (uint32_t)g_classes.size(), 1);
}

W32(user32, GetWindowLongA) {
    const uint32_t hwnd = arg(c, 0);
    const int32_t index = (int32_t)arg(c, 1);
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) {
        set_last_error(1400);  // ERROR_INVALID_WINDOW_HANDLE
        return ret(c, 0, 2);
    }
    const Window& w = it->second;
    uint32_t r = 0;
    switch (index) {
    case -4: r = w.wndproc; break;                                // GWL_WNDPROC
    case -6: r = process().image->image_base(); break;            // GWL_HINSTANCE
    case -16: r = w.style | (w.visible ? 0x10000000u : 0); break;  // GWL_STYLE
    case -20: r = w.exstyle; break;                               // GWL_EXSTYLE
    default: {
        auto l = w.longs.find(index);
        r = l == w.longs.end() ? 0 : l->second;
    }
    }
    ret(c, r, 2);
}

W32(user32, SetWindowLongA) {
    const uint32_t hwnd = arg(c, 0), value = arg(c, 2);
    const int32_t index = (int32_t)arg(c, 1);
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_windows.find(hwnd);
    if (it == g_windows.end()) {
        set_last_error(1400);
        return ret(c, 0, 3);
    }
    Window& w = it->second;
    uint32_t old = 0;
    switch (index) {
    case -4: old = w.wndproc, w.wndproc = value; break;
    case -16: old = w.style, w.style = value; break;
    case -20: old = w.exstyle, w.exstyle = value; break;
    default: old = w.longs[index], w.longs[index] = value;
    }
    trace("SetWindowLongA(%x, %d, %08x)", hwnd, index, value);
    ret(c, old, 3);
}

// SetWindowPos(hwnd, after, x, y, cx, cy, flags): a new size as Windows makes
// it (WM_SIZE) unless SWP_NOSIZE; positions are not kept (the window is the
// screen as the game sees it).
W32(user32, SetWindowPos) {
    const uint32_t hwnd = arg(c, 0), flags = arg(c, 6);
    const int cx = (int)arg(c, 4), cy = (int)arg(c, 5);
    trace("SetWindowPos(%x, %d,%d %dx%d, flags %x)", hwnd, (int)arg(c, 2), (int)arg(c, 3), cx, cy, flags);
    if (!(flags & 1) && cx > 0 && cy > 0) window_resized(c, hwnd, cx, cy);
    if (flags & 0x40) {  // SWP_SHOWWINDOW
        bool shown = false;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            auto it = g_windows.find(hwnd);
            shown = it != g_windows.end() && it->second.visible;
        }
        if (!shown) {
            const uint32_t esp = c.esp;
            c.esp = (c.esp - 16) & ~15u;
            wr(c.esp + 4, hwnd), wr(c.esp + 8, 5);  // SW_SHOW
            imp_user32_ShowWindow(c);
            c.esp = esp;
        }
    }
    ret(c, 1, 7);
}

W32(user32, GetActiveWindow) {
    std::lock_guard<std::mutex> lk(g_lock);
    ret(c, g_windows.empty() ? 0 : g_windows.begin()->first, 0);
}
W32(user32, GetLastActivePopup) { ret(c, arg(c, 0), 1); }
W32(user32, GetParent) { ret(c, 0, 1); }
W32(user32, BringWindowToTop) { ret(c, 1, 1); }
W32(user32, SetForegroundWindow) { ret(c, 1, 1); }
// No other instance of the game runs (the game looks for its own window).
W32(user32, FindWindowA) {
    trace("FindWindowA(%s, %s)", arg(c, 0) >= 0x10000 ? gstr(arg(c, 0)).c_str() : "#",
          arg(c, 1) ? gstr(arg(c, 1)).c_str() : "");
    ret(c, 0, 2);
}
W32(user32, FlashWindowEx) { ret(c, 0, 1); }
W32(user32, IsZoomed) { ret(c, 0, 1); }
W32(user32, ClientToScreen) { ret(c, 1, 2); }  // the client area is the screen
W32(user32, SetCursorPos) {
    set_cursor_position((int)arg(c, 0), (int)arg(c, 1));
    ret(c, 1, 2);
}
W32(user32, SetCursor) { ret(c, 0, 1); }
// Device contexts: the game only clears its window with GDI before Direct3D draws.
W32(user32, GetDC) { ret(c, 0x10050, 1); }
W32(user32, ReleaseDC) { ret(c, 1, 2); }
W32(user32, FillRect) { ret(c, 1, 3); }

}  // namespace scimitar::imports
