// The port's Mac shell, natively: replaces the game's Mac* functions
// (scimitar-recomp --override), which in the original drive AppKit through an
// ACApplicationDelegate, a CustomOpenGLView and an NSTimer. Here the window,
// the OpenGL context and the event loop are host Cocoa objects, and events go
// to the game's own input functions the way its delegate passed them:
//
//   key down:  MacHandleChar(first UTF-16 unit), MacUpdateKeyModifiersState(ctrl,
//              alt, shift), MacHandleKeyDown(keyCode)
//   key up:    MacUpdateKeyModifiersState(...), MacHandleKeyUp(keyCode)
//   key down/up also go to the virtual HID keyboard (platform/src/iokit.cpp),
//              as HID usages: gameplay reads keys there, the calls above only
//              feed text entry and the debug console
//   mouse moved/dragged: deltaX/deltaY/deltaZ accumulate (floats)
//   mouse button down/up: queued as (buttonNumber, type) events
//
// and MacGetMouseState hands both out exactly as the original did. All of this
// runs on the guest main thread, which is the host main thread. Game pads,
// scripted input and what else the iOS shell shares: shell_common.mm.
//
// For unattended tests, SCIMITAR_HIDDEN=1 keeps the window on screen but fully
// transparent and click-through, with no Dock icon, no menu bar and no focus
// taken (it never goes full screen; the mouse is not captured).
//
// F6 shows or hides a statistics overlay (frame rate, CPU, memory, guest heap);
// SCIMITAR_STATS=1 starts with it shown. F6 is not passed to the game.
//
// Display modes are not really switched. The game renders into a GL surface of
// the mode's size (CGLSetParameter kCGLCPSurfaceBackingSize), which the system
// scales to the view, letterboxed to the mode's aspect inside the window. Full
// screen is macOS's own (a Space of its own: Cmd-Tab and Mission Control keep
// working); Cmd-Return or Ctrl-Cmd-F toggles it, and the window is resizable.
// SCIMITAR_WINDOWED=1 starts full-screen modes in a window.
#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include <mach/mach.h>
#include <pthread.h>
#include <sys/resource.h>

#include "game.h"
#include "graphics_menu.h"
#include "shell_common.h"
#include "scimitar/guest_heap.h"
#include "scimitar/metal_gl.h"
#include "scimitar/runtime.h"

namespace scimitar {
void set_display_mode(int width, int height);  // platform/src/coregraphics.cpp
void set_display_safe_top(int points);         // platform/src/coregraphics.cpp
void hid_keyboard_event(uint32_t usage, bool down);  // platform/src/iokit.cpp
}

// A borderless (full-screen) window must still take the keyboard.
@interface ScimitarWindow : NSWindow
@end
@implementation ScimitarWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

void scimitar_layout_gl_view();  // below

// The window's content view: keeps the GL view letterboxed when it resizes.
@interface ScimitarContentView : NSView
@end
@implementation ScimitarContentView
- (void)resizeSubviewsWithOldSize:(NSSize)old { scimitar_layout_gl_view(); }
@end

// Cmd-Q, the Quit menu item and the window's close button end the game the way
// the original's applicationWillTerminate: did, not through AppKit's exit().
@interface ScimitarDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end
@implementation ScimitarDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    shell::quit();
    return NSTerminateCancel;
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
    shell::quit();
    return NO;
}
@end

namespace {

using namespace shell;

struct Shell {
    NSWindow* window = nil;
    NSView* view = nil;  // the GL view, a subview of the window's content view
    NSOpenGLContext* gl = nil;
    NSTextField* stats = nil;  // the F6 overlay, above the GL view
    NSTextField* menu = nil;   // the F4 graphics menu, above the GL view
    NSTextField* notice = nil;  // a short message (frame dumps), bottom left
    std::chrono::steady_clock::time_point notice_until;
    // Mode switch: set by the render thread, applied to the window by the main thread.
    std::mutex mode_lock;
    bool window_pending = false, window_fullscreen = false;
    int window_w = 800, window_h = 600;
    int aspect_w = 800, aspect_h = 600;  // the mode the view is letterboxed to (main thread)
};
Shell g;
// SCIMITAR_RENDERER=metal (g_metal): the view shows a CAMetalLayer that the
// Metal backend (platform/src/metal) presents into, instead of an NSOpenGLContext.

// The GL context belongs to the render thread from MacSetContextOpenGL on; the
// main thread's view updates ([NSOpenGLContext update] on resize, full screen
// and mode switches) wait for a frame boundary, where MacUpdateOpenGL hands the
// context over, instead of racing the render thread's drawing.
std::timed_mutex g_gl_frame;
std::atomic<bool> g_main_wants_gl{false};
bool g_layout_pending = false;  // main thread: an update that could not get the context yet
thread_local bool t_holds_gl = false;

bool ctrl(NSEvent* e) { return (e.modifierFlags & NSEventModifierFlagControl) != 0; }
bool alt(NSEvent* e) { return (e.modifierFlags & NSEventModifierFlagOption) != 0; }
bool shift(NSEvent* e) { return (e.modifierFlags & NSEventModifierFlagShift) != 0; }

// Applies a mode switch to the window (main thread): native full screen, or a
// window of the mode's size (smaller if the screen is).
void apply_window_mode() {
    bool fullscreen;
    int w, h;
    {
        std::lock_guard<std::mutex> l(g.mode_lock);
        if (!g.window_pending) return;
        g.window_pending = false;
        fullscreen = g.window_fullscreen, w = g.window_w, h = g.window_h;
    }
    g.aspect_w = w, g.aspect_h = h;
    const bool is_fullscreen = (g.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (!is_fullscreen) {
        // The windowed size, also what full screen returns to.
        const NSRect vis = (g.window.screen ?: NSScreen.mainScreen).visibleFrame;
        const double scale = std::min({1.0, vis.size.width * 0.9 / w, (vis.size.height - 40) * 0.9 / h});
        [g.window setContentSize:NSMakeSize(std::floor(w * scale), std::floor(h * scale))];
        [g.window center];
    }
    if (g_hidden) fullscreen = false;
    if (fullscreen != is_fullscreen) [g.window toggleFullScreen:nil];
    if (g_hidden) [g.window orderFrontRegardless];
    else [g.window makeKeyAndOrderFront:nil];
    scimitar_layout_gl_view();
}

// ---- the F6 statistics overlay ----

// Top left of the game picture.
void place_stats() {
    if (!g.stats || !g.view) return;
    const NSRect v = g.view.frame;
    const NSSize s = g.stats.frame.size;
    [g.stats setFrameOrigin:NSMakePoint(v.origin.x + 8, v.origin.y + v.size.height - s.height - 8)];
}

// Refreshed twice a second from the event pump (stats_text, shell_common.mm).
void update_stats(bool force = false) {
    if (!g.stats || g.stats.hidden) return;
    const std::string text = stats_text(force);
    if (text.empty()) return;
    g.stats.stringValue = [NSString stringWithUTF8String:text.c_str()];
    [g.stats sizeToFit];
    place_stats();
}

// ---- the F4 graphics menu (graphics_menu.cpp) ----

// Top right of the game picture.
void place_menu() {
    if (!g.menu || !g.view) return;
    const NSRect v = g.view.frame;
    const NSSize s = g.menu.frame.size;
    [g.menu setFrameOrigin:NSMakePoint(v.origin.x + v.size.width - s.width - 8, v.origin.y + v.size.height - s.height - 8)];
}

void update_menu() {
    if (!g.menu || g.menu.hidden) return;
    g.menu.stringValue = [NSString stringWithUTF8String:graphics_menu::text().c_str()];
    [g.menu sizeToFit];
    place_menu();
}

bool menu_open() { return g.menu && !g.menu.hidden; }

void toggle_menu(Context& c) {
    if (!g.window) return;
    if (!g.menu) {
        g.menu = [NSTextField labelWithString:@""];
        g.menu.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium];
        g.menu.textColor = NSColor.whiteColor;
        g.menu.drawsBackground = YES;
        g.menu.backgroundColor = [NSColor colorWithWhite:0 alpha:0.7];
        g.menu.usesSingleLineMode = NO;
        g.menu.maximumNumberOfLines = 0;
        g.menu.hidden = YES;
        [g.window.contentView addSubview:g.menu positioned:NSWindowAbove relativeTo:g.view];
    }
    g.menu.hidden = !g.menu.hidden;
    if (!g.menu.hidden) graphics_menu::refresh(c);
    update_menu();
}

// The arrow keys while the menu is open.
bool menu_key(Context& c, uint16_t key) {
    graphics_menu::Key k;
    switch (key) {
    case 126: k = graphics_menu::Key::Up; break;
    case 125: k = graphics_menu::Key::Down; break;
    case 123: k = graphics_menu::Key::Left; break;
    case 124: k = graphics_menu::Key::Right; break;
    default: return false;
    }
    graphics_menu::key(c, k);
    update_menu();
    return true;
}


// SCIMITAR_MENU_KEYS="FRAME:KEYS;FRAME:KEYS..." (unattended runs): at each
// frame, the keys to the menu: M opens or closes it, U D L R are the arrows,
// P logs the game's current values.
void scripted_menu_keys(Context& c) {
    static const char* script = std::getenv("SCIMITAR_MENU_KEYS");
    static const char* at = script;
    while (at && *at) {
        char* end = nullptr;
        const unsigned long long frame = std::strtoull(at, &end, 10);
        if (end == at || *end != ':' || g_frames.load() < frame) return;
        for (at = end + 1; *at && *at != ';'; at++) {
            static const char keys[] = "UDLR";
            static const uint16_t codes[] = {126, 125, 123, 124};
            if (*at == 'M') toggle_menu(c);
            else if (*at == 'P') graphics_menu::refresh(c), game::log("%s\n", graphics_menu::text().c_str());
            else if (const char* k = std::strchr(keys, *at)) menu_key(c, codes[k - keys]);
        }
        if (*at == ';') at++;
    }
}

}  // namespace

// A message at the bottom left of the game picture for a few seconds (main thread).
void shell::show_notice(const char* text, double seconds) {
    if (!g.window || !g.view) return;
    if (!g.notice) {
        g.notice = [NSTextField labelWithString:@""];
        g.notice.font = [NSFont monospacedSystemFontOfSize:13 weight:NSFontWeightMedium];
        g.notice.textColor = NSColor.whiteColor;
        g.notice.drawsBackground = YES;
        g.notice.backgroundColor = [NSColor colorWithWhite:0 alpha:0.65];
        [g.window.contentView addSubview:g.notice positioned:NSWindowAbove relativeTo:g.view];
    }
    g.notice.stringValue = [NSString stringWithUTF8String:text];
    [g.notice sizeToFit];
    const NSRect v = g.view.frame;
    [g.notice setFrameOrigin:NSMakePoint(v.origin.x + 8, v.origin.y + 8)];
    g.notice.hidden = NO;
    g.notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
}

namespace {

void update_notice() {
    if (g_metal) {
        const std::string dumped = scimitar::metal::take_dump_result();
        if (!dumped.empty()) show_notice(("Frame saved to " + dumped).c_str(), 5);
    }
    if (g.notice && !g.notice.hidden && std::chrono::steady_clock::now() > g.notice_until) g.notice.hidden = YES;
}

}  // namespace

void shell::toggle_stats() {
    if (!g.window) return;
    if (!g.stats) {
        g.stats = [NSTextField labelWithString:@""];
        g.stats.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium];
        g.stats.textColor = NSColor.whiteColor;
        g.stats.drawsBackground = YES;
        g.stats.backgroundColor = [NSColor colorWithWhite:0 alpha:0.55];
        g.stats.usesSingleLineMode = NO;
        g.stats.maximumNumberOfLines = 0;
        g.stats.hidden = YES;
        [g.window.contentView addSubview:g.stats positioned:NSWindowAbove relativeTo:g.view];
    }
    g.stats.hidden = !g.stats.hidden;
    update_stats(true);
}

namespace {

// The original delegate told the game when the application became active or
// inactive (applicationDidFinishLaunching/DidBecomeActive/WillResignActive):
// the pad manager, keyboard and mouse included, only runs while active. With
// scripted keys or pad input (SCIMITAR_KEYS, SCIMITAR_PAD) the game is always
// active, focus or not.
//
// While really active the mouse is captured, as the original did by hiding the
// cursor and recentring it: the cursor is hidden and decoupled from the mouse,
// whose deltas keep coming (the game draws its own cursor in menus).
// SCIMITAR_NO_MOUSE_CAPTURE=1 leaves the cursor alone.
void capture_mouse(bool on) {
    static bool captured = false;
    static const bool allowed = std::getenv("SCIMITAR_NO_MOUSE_CAPTURE") == nullptr && !g_hidden;
    on = on && allowed;
    if (on == captured) return;
    captured = on;
    // The frozen cursor stays where it is: at the top edge of a full screen
    // it would keep the menu bar and title bar shown, so move it to the
    // window's centre first.
    if (on && g.window) {
        const NSRect f = g.window.frame;
        const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);  // flipped: CG's origin is the top left
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(f), top - NSMidY(f)));
    }
    CGAssociateMouseAndMouseCursorPosition(on ? false : true);
    if (on) [NSCursor hide];
    else [NSCursor unhide];
}

void track_activation(Context& c) {
    static int last = -1;
    static const bool always = std::getenv("SCIMITAR_KEYS") != nullptr || std::getenv("SCIMITAR_PAD") != nullptr;
    // Launched from another app's process, macOS may keep that app frontmost
    // while our window has the keyboard: a key window counts as active too.
    const bool really = NSApp.isActive || g.window.isKeyWindow;
    capture_mouse(really && !always);
    const int active = always || really ? 1 : 0;
    if (active == last) return;
    last = active;
    if (std::getenv("SCIMITAR_TRACE_INPUT"))
        game::log("application %s\n", active ? "active" : "inactive");
    scimitar::call_guest(c, game_functions().set_active, {(uint32_t)active});
}

void pump(Context& c) {
    g_pump_context = &c;
    if (g_layout_pending) scimitar_layout_gl_view();
    static bool stats_at_start = std::getenv("SCIMITAR_STATS") != nullptr;
    if (stats_at_start && g.window) stats_at_start = false, toggle_stats();
    update_stats();
    update_notice();
    graphics_menu::enforce(c);
    scripted_menu_keys(c);
    track_activation(c);
    pump_input(c);
    @autoreleasepool {
        apply_window_mode();
        // The engine's main thread spins on this (Engine::WaitForLoopDone: a
        // mutex-guarded flag, then the event pump) for the whole game. Waiting up
        // to 2 ms for the first event instead of polling frees that core and the
        // mutex, at a cost of at most 2 ms of input latency.
        // SCIMITAR_PUMP_WAIT_US sets the wait (0 polls, as the original did).
        static const double wait = [] {
            const char* v = std::getenv("SCIMITAR_PUMP_WAIT_US");
            return (v ? std::atof(v) : 2000.0) / 1e6;
        }();
        for (bool first = true;; first = false) {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:first && wait > 0 ? [NSDate dateWithTimeIntervalSinceNow:wait]
                                                                        : [NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (!e) break;
            const NSEventType t = e.type;
            bool to_app = true;
            switch (t) {
            case NSEventTypeKeyDown:
                if (e.modifierFlags & NSEventModifierFlagCommand) break;  // menu shortcuts (Cmd-Q)
                if (e.keyCode == kKeyF6) {
                    if (!e.ARepeat) toggle_stats();
                    to_app = false;
                    break;
                }
                if (e.keyCode == kKeyF4) {
                    if (!e.ARepeat) toggle_menu(c);
                    to_app = false;
                    break;
                }
                if (menu_open() && (e.keyCode == 53 ? (toggle_menu(c), true) : menu_key(c, e.keyCode))) {
                    to_app = false;  // Escape closes it
                    break;
                }
                if (e.keyCode == kKeyF13 && g_metal) {
                    if (!e.ARepeat) request_frame_dump();
                    to_app = false;
                    break;
                }
                key_down(c, e.keyCode, e.characters.length ? [e.characters characterAtIndex:0] : 0, ctrl(e), alt(e),
                         shift(e), e.ARepeat);
                to_app = false;  // no system beep for keys the window does not take
                break;
            case NSEventTypeKeyUp:
                if (e.modifierFlags & NSEventModifierFlagCommand) break;
                if (e.keyCode == kKeyF6 || e.keyCode == kKeyF4 || (e.keyCode == kKeyF13 && g_metal)) {
                    to_app = false;
                    break;
                }
                key_up(c, e.keyCode, ctrl(e), alt(e), shift(e));
                to_app = false;
                break;
            case NSEventTypeFlagsChanged: {
                // A modifier key went down or up: down if its flag is now set.
                NSEventModifierFlags flag = 0;
                switch (e.keyCode) {
                case 56: case 60: flag = NSEventModifierFlagShift; break;
                case 59: case 62: flag = NSEventModifierFlagControl; break;
                case 58: case 61: flag = NSEventModifierFlagOption; break;
                case 55: case 54: flag = NSEventModifierFlagCommand; break;
                }
                if (flag) scimitar::hid_keyboard_event(hid_usage(e.keyCode), (e.modifierFlags & flag) != 0);
                scimitar::call_guest(c, game_functions().update_modifiers, {ctrl(e) ? 1u : 0u, alt(e) ? 1u : 0u, shift(e) ? 1u : 0u});
                break;
            }
            case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
            case NSEventTypeOtherMouseDragged:
                mouse_motion((float)e.deltaX, (float)e.deltaY, (float)e.deltaZ);
                break;
            case NSEventTypeLeftMouseDown:
            case NSEventTypeLeftMouseUp:
            case NSEventTypeRightMouseDown:
            case NSEventTypeRightMouseUp:
            case NSEventTypeOtherMouseDown:
            case NSEventTypeOtherMouseUp:
                mouse_button((uint32_t)e.buttonNumber, t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown ||
                                                           t == NSEventTypeOtherMouseDown);
                break;
            default:
                break;
            }
            if (to_app) [NSApp sendEvent:e];
        }
        [NSApp updateWindows];
    }
}

}  // namespace

// Letterboxes the GL view to the mode's aspect in the content view, and tells
// the context (AppKit wants that on the main thread; the lock keeps the render
// thread's present out meanwhile).
void scimitar_layout_gl_view() {
    if (!g.window || !g.view) return;
    g_layout_pending = false;
    const NSSize s = g.window.contentView.bounds.size;
    const double scale = std::min(s.width / g.aspect_w, s.height / g.aspect_h);
    double vw = std::floor(g.aspect_w * scale), vh = std::floor(g.aspect_h * scale);
    // A mode within 1% of the window's shape fills it (a full screen below a
    // camera housing is a point or so off any mode): no slivers of black.
    if (std::abs((s.width / s.height) / ((double)g.aspect_w / g.aspect_h) - 1) < 0.01) vw = s.width, vh = s.height;
    g.view.frame = NSMakeRect(std::floor((s.width - vw) / 2), std::floor((s.height - vh) / 2), vw, vh);
    place_stats();
    place_menu();
    if (g_metal) {
        const NSSize px = [g.view convertSizeToBacking:g.view.bounds.size];
        scimitar::metal::set_drawable_size((int)px.width, (int)px.height);
    }
    if (g.gl) {
        g_main_wants_gl = true;
        if (g_gl_frame.try_lock_for(std::chrono::milliseconds(100))) {
            CGLLockContext(g.gl.CGLContextObj);
            [g.gl update];
            CGLUnlockContext(g.gl.CGLContextObj);
            g_gl_frame.unlock();
        } else {
            g_layout_pending = true;  // the render thread is busy (loading): next pump
        }
        g_main_wants_gl = false;
    }
}

namespace {
void switch_mode_if_needed();  // below
}

namespace scimitar::overrides {

using scimitar::arg32;
using scimitar::shim_return;
using namespace shell;

// void MacApplicationInit(void): the application, its menu, launching.
void ov_MacApplicationInit(Context& c) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        // Full screen sits below a camera housing: the display's modes include
        // that area, which full screen fills without bars.
        for (NSScreen* s in NSScreen.screens)
            if ([s.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == CGMainDisplayID())
                scimitar::set_display_safe_top((int)std::lround(s.safeAreaInsets.top));
        [NSApp setActivationPolicy:g_hidden ? NSApplicationActivationPolicyAccessory : NSApplicationActivationPolicyRegular];
        // A game: no App Nap or timer coalescing, also while in the background.
        static id<NSObject> activity = [NSProcessInfo.processInfo
            beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical
                              reason:@"game"];
        (void)activity;
        NSMenu* bar = [[NSMenu alloc] init];
        NSMenuItem* app = [[NSMenuItem alloc] init];
        [bar addItem:app];
        NSMenu* menu = [[NSMenu alloc] init];
        [menu addItemWithTitle:[@"Quit " stringByAppendingString:@(game::kInfo.title)] action:@selector(terminate:)
                 keyEquivalent:@"q"];
        app.submenu = menu;
        NSMenuItem* view = [[NSMenuItem alloc] init];
        [bar addItem:view];
        NSMenu* view_menu = [[NSMenu alloc] initWithTitle:@"View"];
        [view_menu addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"\r"];
        NSMenuItem* fs = [view_menu addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:)
                                       keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        view.submenu = view_menu;
        NSApp.mainMenu = bar;
        static ScimitarDelegate* delegate = [ScimitarDelegate new];
        NSApp.delegate = delegate;
        [NSApp finishLaunching];
        // Scripted runs leave the keyboard focus where it is: typing elsewhere
        // must not land in the game.
        if (!std::getenv("SCIMITAR_KEYS") && !std::getenv("SCIMITAR_PAD") && !g_hidden) [NSApp activateIgnoringOtherApps:YES];
    }
    shim_return(c, 0);
}

// NSWindow* MacCreateWindow(void): an 800x600 titled window, as the original.
void ov_MacCreateWindow(Context& c) {
    @autoreleasepool {
        g.window = [[ScimitarWindow alloc] initWithContentRect:NSMakeRect(150, 100, 800, 600)
                                               styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        g.window.title = @(game::kInfo.title);
        g.window.acceptsMouseMovedEvents = YES;
        g.window.backgroundColor = NSColor.blackColor;
        g.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        g.window.contentView = [[ScimitarContentView alloc] initWithFrame:NSMakeRect(0, 0, 800, 600)];
        g.window.delegate = (ScimitarDelegate*)NSApp.delegate;
        g.view = [[NSView alloc] initWithFrame:g.window.contentView.bounds];
        [g.window.contentView addSubview:g.view];
        // A full screen of an inactive application keeps its title bar and the
        // menu bar shown (the activation at start can lose to the app in front
        // under cooperative activation): take it when the full screen comes up,
        // or the user switches to its Space. Scripted runs leave the focus be.
        if (!std::getenv("SCIMITAR_KEYS") && !std::getenv("SCIMITAR_PAD") && !g_hidden) {
            void (^activate)(NSNotification*) = ^(NSNotification*) {
                if (!NSApp.isActive && g.window.isOnActiveSpace && (g.window.styleMask & NSWindowStyleMaskFullScreen))
                    [NSApp activate];
            };
            [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification object:g.window
                                                             queue:nil usingBlock:activate];
            [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification
                                                                        object:nil queue:nil usingBlock:activate];
        }
        if (std::getenv("SCIMITAR_VERBOSE")) {
            NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
            [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:g.window queue:nil
                        usingBlock:^(NSNotification*) {
                            const NSSize v = g.window.contentView.bounds.size, p = g.view.frame.size;
                            game::log("entered full screen: %gx%g, picture %gx%g at %g,%g\n", v.width, v.height, p.width,
                                      p.height, g.view.frame.origin.x, g.view.frame.origin.y);
                        }];
            [nc addObserverForName:NSWindowDidExitFullScreenNotification object:g.window queue:nil
                        usingBlock:^(NSNotification*) { game::log("left full screen\n"); }];
        }
        [g.window center];
        if (g_hidden) {
            g.window.alphaValue = 0;
            g.window.ignoresMouseEvents = YES;
            g.window.hasShadow = NO;
            [g.window orderFrontRegardless];
        } else {
            [g.window makeKeyAndOrderFront:nil];
            [NSApp activate];
        }
    }
    shim_return(c, kWindowHandle);
}

// void MacInitializeOpenGL(NSWindow*): the game's pixel format (double
// buffered, 32-bit colour, 24-bit depth, 8-bit stencil) on the legacy
// profile, whose GL 2.1 runs the game's ARB programs.
void ov_MacInitializeOpenGL(Context& c) {
    if (g_metal) {
        @autoreleasepool {
            CAMetalLayer* layer = [CAMetalLayer layer];
            g.view.layer = layer;
            g.view.wantsLayer = YES;
            scimitar::metal::attach_layer((__bridge void*)layer, c.mem, g_mode.have_w, g_mode.have_h);
            scimitar_layout_gl_view();
        }
        return shim_return(c, 0);
    }
    @autoreleasepool {
        const NSOpenGLPixelFormatAttribute attrs[] = {
            NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 32, NSOpenGLPFADepthSize, 24,
            NSOpenGLPFAStencilSize, 8, NSOpenGLPFAAccelerated, NSOpenGLPFAOpenGLProfile,
            NSOpenGLProfileVersionLegacy, 0};
        NSOpenGLPixelFormat* pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
        if (!pf) scimitar::fatal("MacInitializeOpenGL: no OpenGL pixel format");
        g.gl = [[NSOpenGLContext alloc] initWithFormat:pf shareContext:nil];
        g.view.wantsBestResolutionOpenGLSurface = NO;
        g.gl.view = g.view;
        [g.gl makeCurrentContext];
    }
    shim_return(c, 0);
}

// void MacSetContextOpenGL(void): makes the context current on this thread.
void ov_MacSetContextOpenGL(Context& c) {
    if (g_metal) return shim_return(c, 0);
    if (!t_holds_gl && !NSThread.isMainThread) g_gl_frame.lock(), t_holds_gl = true;
    [g.gl makeCurrentContext];
    shim_return(c, 0);
}

// void MacUpdateOpenGL(void): presents the frame (and counts it, shell_common.mm).
void ov_MacUpdateOpenGL(Context& c) {
    count_frame([](int* w, int* h, std::vector<uint8_t>* px) {
        if (g_metal) return scimitar::metal::read_default(w, h, px);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, *w, *h, GL_RGB, GL_UNSIGNED_BYTE, px->data());
        return true;
    });
    if (g_metal) {
        scimitar::metal::present();
        return shim_return(c, 0);
    }
    CGLLockContext(g.gl.CGLContextObj);
    [g.gl flushBuffer];
    CGLUnlockContext(g.gl.CGLContextObj);
    // Frame boundary: let a waiting main-thread view update in.
    if (!t_holds_gl && !NSThread.isMainThread) g_gl_frame.lock(), t_holds_gl = true;
    if (t_holds_gl && g_main_wants_gl.load()) {
        g_gl_frame.unlock();
        while (g_main_wants_gl.load()) std::this_thread::yield();
        g_gl_frame.lock();
    }
    shim_return(c, 0);
}

// void MacVSyncOpenGL(NSWindow*, bool on)
void ov_MacVSyncOpenGL(Context& c) {
    const GLint interval = (arg32(c, 1) & 0xff) ? 1 : 0;
    if (g_metal) {
        scimitar::metal::set_vsync(interval != 0);
        return shim_return(c, 0);
    }
    [g.gl setValues:&interval forParameter:NSOpenGLContextParameterSwapInterval];
    shim_return(c, 0);
}

// void MacApplicationLoop(void): handles pending events, as the original's
// run loop pass with its heartbeat timer did.
void ov_MacApplicationLoop(Context& c) {
    pump(c);
    shim_return(c, 0);
}

// void MacSwitchModeIfNeeded(void), on the render thread: the GL surface takes
// the mode's size at once, the window follows on the main thread.
void ov_MacSwitchModeIfNeeded(Context& c) {
    switch_mode_if_needed();
    shim_return(c, 0);
}

// void MacSwitchModeOpenGL(bool fullscreen, int width, int height, int refresh)
// (Brotherhood, in place of AC2's MacScheduleSwitchMode and
// MacSwitchModeIfNeeded): requests and applies the mode at once.
void ov_MacSwitchModeOpenGL(Context& c) {
    request_mode(arg32(c, 0) & 0xff, (int)arg32(c, 1), (int)arg32(c, 2));
    switch_mode_if_needed();
    shim_return(c, 0);
}

// bool MacLockFocusIfCanDraw(void), void MacUnlockFocus(void) (Brotherhood):
// the original locked the view's focus around drawing; the context or the
// Metal layer is always ready here.
void ov_MacLockFocusIfCanDraw(Context& c) { shim_return(c, 1); }
void ov_MacUnlockFocus(Context& c) { shim_return(c, 0); }

}  // namespace scimitar::overrides

namespace {

void switch_mode_if_needed() {
    Mode& m = g_mode;
    if (m.pending && m.want_w > 0 && m.want_h > 0) {
        static const bool windowed = std::getenv("SCIMITAR_WINDOWED") != nullptr;
        if (g_metal) {
            scimitar::metal::set_default_size(m.want_w, m.want_h);
        } else {
            const GLint size[2] = {m.want_w, m.want_h};
            CGLContextObj cgl = g.gl.CGLContextObj;
            CGLSetParameter(cgl, kCGLCPSurfaceBackingSize, size);
            CGLEnable(cgl, kCGLCESurfaceBackingSize);
        }
        scimitar::set_display_mode(m.want_w, m.want_h);
        {
            std::lock_guard<std::mutex> l(g.mode_lock);
            g.window_pending = true;
            g.window_fullscreen = m.want_fullscreen && !windowed;
            g.window_w = m.want_w, g.window_h = m.want_h;
        }
        m.have_w = m.want_w, m.have_h = m.want_h;
    }
    m.pending = false;
}

}  // namespace

namespace scimitar::overrides {

// CGDirectDisplayID MacGetDisplayID(void)
void ov_MacGetDisplayID(Context& c) { shim_return(c, CGMainDisplayID()); }

}  // namespace scimitar::overrides
