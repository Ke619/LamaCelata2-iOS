// The port's iOS shell (iPhone, iPad), natively: replaces the game's Mac*
// functions as shell.mm does on the Mac (scimitar-recomp --override), with
// UIKit and Metal (the only renderer there, platform/src/metal).
//
// UIKit owns the main thread, so the game's own main (games/common/main.cpp,
// game::game_main) runs on a thread of its own, started once the scene has its view.
// That thread is the game's main thread: its event pump (MacApplicationLoop)
// takes the input UIKit queued for it, as the Mac shell takes AppKit's events:
//
//   hardware keyboard: UIKey presses, whose HID usages become the Mac virtual
//                      key codes the game's key functions take
//   mouse (iPad, GCMouse): deltas and buttons for MacGetMouseState, the
//                      pointer locked to the game while it is active
//   game pads: shell_common.mm, through the GameController framework
//   with none connected, the on-screen pad (touchpad_ios.mm): sticks, d-pad and every button,
//                      the right half of the screen being the right stick; MOUSE and EDIT at the top
//   touch: one finger is the mouse on the menus, placing their cursor (hidden
//                      unless a mouse is connected) and holding the left button
//   a tap in the screen's top left corner: the statistics overlay, as F6;
//                      in the top right: a frame dump (Metal's, as F13), in the
//                      app's temporary folder
//
// The view fills the screen; the game's picture is letterboxed in it at the
// mode's aspect. The modes offered are the screen's own size and the common
// ones below it (platform/src/coregraphics.cpp, set_host_display).
//
// In the background (iOS allows no GPU work there) the game is told it is
// inactive, and its render thread waits at the next frame boundary until the
// app is in front again.
//
// The game's files: its .app (the Mac game, as installed; "Assassin's Creed
// 2.app" for AC2) in the app's Documents folder, or the game's app_env (AC2_APP).
// The Simulator can read the Mac's own copy: xcrun simctl launch with
// SIMCTL_CHILD_AC2_APP=<path>.
#import <AVFAudio/AVFAudio.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

#include "game.h"
#include "graphics_menu.h"
#include "shell_common.h"
#include "touchpad_ios.h"
#ifdef SCIMITAR_LAUNCHER
#include "launcher/launcher.h"
#endif
#include "scimitar/metal_gl.h"
#include "scimitar/runtime.h"


namespace scimitar {
void set_display_mode(int width, int height);                       // platform/src/coregraphics.cpp
void set_host_display(int width, int height, double refresh_hz);
}

namespace {

using namespace shell;

// What UIKit's main thread hands the game's event pump.
struct InputEvent {
    enum Kind { KeyDown, KeyUp, Special } kind;
    uint16_t key = 0, ch = 0;
    bool ctrl = false, alt = false, shift = false;
};

struct Shell {
    UIWindow* window = nil;
    UIView* view = nil;           // the game's picture, a CAMetalLayer
    CAMetalLayer* layer = nil;
    UILabel* stats = nil;         // the F6 overlay
    UILabel* perf = nil;          // the one line in the top left corner: frame rate, heat, memory
    UILabel* notice = nil;        // a short message, bottom left
    std::chrono::steady_clock::time_point notice_until;
    int aspect_w = 800, aspect_h = 600;  // main thread

    std::mutex lock;  // the queue and the activity, between UIKit and the game
    std::condition_variable wake;
    std::deque<InputEvent> events;
    bool foreground = true;
    std::condition_variable foreground_cv;
};
Shell g;

int g_argc = 0;
char** g_argv = nullptr;

void post(InputEvent e) {
    {
        std::lock_guard<std::mutex> l(g.lock);
        g.events.push_back(e);
    }
    g.wake.notify_one();
}

void on_main(dispatch_block_t b) {
    if (pthread_main_np()) b();
    else dispatch_async(dispatch_get_main_queue(), b);
}

// Letterboxes the game's view to the mode's aspect in the window, and gives the
// backend the drawable's pixel size (main thread).
void layout() {
    if (!g.window || !g.view) return;
    const CGSize s = g.window.bounds.size;
    // A mode of the screen's own shape (the modes offered are, to within rounding) fills it.
    double vw = s.width, vh = s.height;
    if (std::fabs(((double)g.aspect_w / g.aspect_h) / (s.width / s.height) - 1) >= 0.015) {
        const double scale = std::min(s.width / g.aspect_w, s.height / g.aspect_h);
        vw = std::floor(g.aspect_w * scale), vh = std::floor(g.aspect_h * scale);
    }
    g.view.frame = CGRectMake(std::floor((s.width - vw) / 2), std::floor((s.height - vh) / 2), vw, vh);
    const CGFloat px = g.view.window.screen.nativeScale ?: UIScreen.mainScreen.nativeScale;
    g.view.contentScaleFactor = px;
    scimitar::metal::set_drawable_size((int)std::lround(vw * px), (int)std::lround(vh * px));
    // The one line sits in the very top left corner, above the game's health bars and level with the HUD's left edge,
    // which is about 6.25% of the picture's width (measured on a 19.5:9 phone). The F6 overlay moves down below it.
    CGFloat below = 0;
    if (g.perf && !g.perf.hidden) {
        const CGFloat left = g.view.frame.origin.x + g.view.frame.size.width * 0.0625;
        g.perf.frame = CGRectMake(left, g.view.frame.origin.y + 3, g.perf.frame.size.width, g.perf.frame.size.height);
        below = g.perf.frame.size.height + 3;
    }
    if (g.stats) g.stats.frame = CGRectMake(g.view.frame.origin.x + 8, g.view.frame.origin.y + 8 + below,
                                            g.stats.frame.size.width, g.stats.frame.size.height);
}

UILabel* make_label(CGFloat size) {
    UILabel* l = [UILabel new];
    l.font = [UIFont monospacedSystemFontOfSize:size weight:UIFontWeightMedium];
    l.textColor = UIColor.whiteColor;
    l.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6];
    l.numberOfLines = 0;
    l.hidden = YES;
    [g.window addSubview:l];
    return l;
}

// The one line in the top left corner (the launcher's "Show FPS, heat and memory"): small, one row, so it stays
// clear of the HUD. The thermal word is coloured: green, yellow, orange, red.
void show_perf_line() {
    on_main(^{
        if (!g.window || g.perf) return;
        g.perf = make_label(10);
        g.perf.numberOfLines = 1;
        g.perf.backgroundColor = [UIColor colorWithWhite:0 alpha:0.4];
        g.perf.layer.cornerRadius = 4;
        g.perf.layer.masksToBounds = YES;
        g.perf.text = @" -- FPS  ·  Thermal: Nominal  ·  RAM: ---- MB ";
        g.perf.hidden = NO;
        [g.perf sizeToFit];
        layout();
    });
}

void update_overlays() {
    on_main(^{
        if (g.perf) {
            const PerfInfo p = perf_info();
            if (p.fresh) {
                static NSString* const names[] = {@"Nominal", @"Fair", @"Serious", @"Critical"};
                UIColor* colors[] = {[UIColor colorWithRed:0.45 green:0.90 blue:0.50 alpha:1],
                                           [UIColor colorWithRed:1.00 green:0.85 blue:0.35 alpha:1],
                                           [UIColor colorWithRed:1.00 green:0.60 blue:0.25 alpha:1],
                                           [UIColor colorWithRed:1.00 green:0.35 blue:0.35 alpha:1]};
                const int t = std::min(std::max(p.thermal, 0), 3);
                NSString* head = [NSString stringWithFormat:@" %.0f FPS  ·  Thermal: ", p.fps];
                NSString* tail = [NSString stringWithFormat:@"  ·  RAM: %.0f MB ", p.ram_mb];
                NSMutableAttributedString* s = [[NSMutableAttributedString alloc]
                    initWithString:[NSString stringWithFormat:@"%@%@%@", head, names[t], tail]];
                [s addAttributes:@{NSFontAttributeName : g.perf.font, NSForegroundColorAttributeName : UIColor.whiteColor}
                           range:NSMakeRange(0, s.length)];
                [s addAttribute:NSForegroundColorAttributeName value:colors[t] range:NSMakeRange(head.length, names[t].length)];
                g.perf.attributedText = s;
                [g.perf sizeToFit];
                layout();
            }
        }
        if (g.stats && !g.stats.hidden) {
            const std::string text = stats_text(false);
            if (!text.empty()) {
                g.stats.text = [NSString stringWithUTF8String:text.c_str()];
                [g.stats sizeToFit];
                layout();
            }
        }
        if (g.notice && !g.notice.hidden && std::chrono::steady_clock::now() > g.notice_until) g.notice.hidden = YES;
    });
    const std::string dumped = scimitar::metal::take_dump_result();
    if (!dumped.empty()) show_notice(("Frame saved to " + dumped).c_str(), 5);
}

// The game is active while the app is in front, as the Mac delegate's
// DidBecomeActive/WillResignActive told it; scripted input keeps it active.
void track_activation(Context& c) {
    static int last = -1;
    static const bool always = std::getenv("SCIMITAR_KEYS") != nullptr || std::getenv("SCIMITAR_PAD") != nullptr;
    bool fg;
    {
        std::lock_guard<std::mutex> l(g.lock);
        fg = g.foreground;
    }
    const int active = always || fg ? 1 : 0;
    if (active == last) return;
    const bool was_active = last == 1;
    last = active;
    if (std::getenv("SCIMITAR_TRACE_INPUT")) game::log("application %s\n", active ? "active" : "inactive");
    scimitar::call_guest(c, game_functions().set_active, {(uint32_t)active});
    // iOS may end an app in the background without notice: keep its settings.
    if (was_active) {
        scimitar::call_guest(c, game_functions().write_bindings, {});
        scimitar::call_guest(c, game_functions().write_config, {});
    }
}

void pump(Context& c) {
    g_pump_context = &c;
    static bool stats_at_start = std::getenv("SCIMITAR_STATS") != nullptr;
    if (stats_at_start) stats_at_start = false, show_perf_line();  // F6 and the corner tap still open the full overlay
    update_overlays();
    graphics_menu::enforce(c);  // the launcher's Graphics choices (no F4 menu here)
    track_activation(c);
    pump_input(c);
    // As on the Mac, wait up to 2 ms for the first event instead of spinning
    // (SCIMITAR_PUMP_WAIT_US; 0 polls).
    static const auto wait = std::chrono::microseconds([] {
        const char* v = std::getenv("SCIMITAR_PUMP_WAIT_US");
        return v ? std::atoll(v) : 2000;
    }());
    std::deque<InputEvent> events;
    {
        std::unique_lock<std::mutex> l(g.lock);
        if (g.events.empty() && wait.count() > 0) g.wake.wait_for(l, wait);
        events.swap(g.events);
    }
    for (const InputEvent& e : events) switch (e.kind) {
        case InputEvent::KeyDown: key_down(c, e.key, e.ch, e.ctrl, e.alt, e.shift, false); break;
        case InputEvent::KeyUp: key_up(c, e.key, e.ctrl, e.alt, e.shift); break;
        case InputEvent::Special:
            if (e.key == kKeyF6) toggle_stats();
            else if (e.key == kKeyF13) request_frame_dump();
            break;
        }
}

// The render thread, at a frame boundary: no GPU work while in the background.
void wait_for_foreground() {
    std::unique_lock<std::mutex> l(g.lock);
    g.foreground_cv.wait(l, [] { return g.foreground; });
}

void set_foreground(bool fg) {
    {
        std::lock_guard<std::mutex> l(g.lock);
        g.foreground = fg;
    }
    g.foreground_cv.notify_all();
    g.wake.notify_one();
}

void* game_thread(void*) {
    pthread_setname_np((std::string(game::kInfo.id) + " main").c_str());
    const int r = game::game_main(g_argc, g_argv);
    std::fflush(stdout);
    std::_Exit(r);
}

void start_game() {
    static bool started = false;
    if (started) return;
    started = true;
    // Recompiled code nests host calls as deep as the game nests guest ones.
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t;
    if (pthread_create(&t, &a, game_thread, nullptr) != 0) scimitar::fatal("cannot start the game thread");
    pthread_attr_destroy(&a);
}

}  // namespace

void shell::show_notice(const char* text, double seconds) {
    NSString* s = [NSString stringWithUTF8String:text];
    on_main(^{
        if (!g.window || !g.view) return;
        if (!g.notice) g.notice = make_label(13);
        g.notice.text = s;
        [g.notice sizeToFit];
        const CGRect v = g.view.frame;
        g.notice.frame = CGRectMake(v.origin.x + 8, v.origin.y + v.size.height - g.notice.frame.size.height - 8,
                                    g.notice.frame.size.width, g.notice.frame.size.height);
        g.notice.hidden = NO;
        g.notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    });
}

void shell::toggle_stats() {
    on_main(^{
        if (!g.window) return;
        if (!g.stats) g.stats = make_label(12);
        g.stats.hidden = !g.stats.hidden;
        if (!g.stats.hidden) {
            g.stats.text = [NSString stringWithUTF8String:stats_text(true).c_str()];
            [g.stats sizeToFit];
            layout();
        }
    });
}

// ---- UIKit ----

// The game's view: its layer is the CAMetalLayer the backend presents into.
@interface ScimitarView : UIView
@end
@implementation ScimitarView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

// Full screen, landscape (the whole app is landscape-only), no status bar or home indicator; takes the hardware keyboard and
// locks the pointer. A tap in the top left corner (88 points
// square, of the screen, not of the letterboxed picture) shows or hides the
// statistics overlay, as F6 does. Other touches are the mouse on the UI: one
// finger at a time places the menus' cursor and holds the left button.
@interface ScimitarViewController : UIViewController
@end
@implementation ScimitarViewController {
    UITouch* _touch;  // the finger driving the cursor
}
- (void)loadView {
    UIView* root = [UIView new];
    root.backgroundColor = UIColor.blackColor;
    self.view = root;
    UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)];
    tap.cancelsTouchesInView = NO;
    [root addGestureRecognizer:tap];
}
static constexpr CGFloat kCorner = 88;
- (BOOL)inCorner:(CGPoint)p {
    return p.y < kCorner && (p.x < kCorner || p.x > self.view.bounds.size.width - kCorner);
}
- (void)tapped:(UITapGestureRecognizer*)tap {
    const CGPoint p = [tap locationInView:self.view];
    if (tap.state != UIGestureRecognizerStateEnded || ![self inCorner:p]) return;
    if (p.x < kCorner) toggle_stats();
    else post({InputEvent::Special, kKeyF13});  // frame dump, as F13
}
- (void)sendTouch:(UITouch*)t phase:(Touch)phase {
    const CGPoint p = [t locationInView:g.view];
    const CGSize s = g.view.bounds.size;
    if (s.width > 0 && s.height > 0) touch(phase, (float)(p.x / s.width), (float)(p.y / s.height));
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* t in touches) {
        if (_touch || t.type == UITouchTypeIndirectPointer || [self inCorner:[t locationInView:self.view]]) continue;
        _touch = t;
        [self sendTouch:t phase:Touch::Began];
    }
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_touch && [touches containsObject:_touch]) [self sendTouch:_touch phase:Touch::Moved];
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (!_touch || ![touches containsObject:_touch]) return;
    [self sendTouch:_touch phase:Touch::Ended];
    _touch = nil;
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self touchesEnded:touches withEvent:event];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (BOOL)prefersPointerLocked { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    layout();
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becomeFirstResponder];
}

// Hardware keyboard presses, as Mac key codes (shell_common.mm).
- (void)handlePresses:(NSSet<UIPress*>*)presses down:(BOOL)down {
    for (UIPress* p in presses) {
        UIKey* k = p.key;
        if (!k) continue;
        const uint16_t mac = mac_key((uint32_t)k.keyCode);
        if (mac == 0xffff) continue;
        if (mac == kKeyF6 || mac == kKeyF13) {
            if (down) post({InputEvent::Special, mac});
            continue;
        }
        InputEvent e{down ? InputEvent::KeyDown : InputEvent::KeyUp, mac};
        if (down && k.characters.length) e.ch = [k.characters characterAtIndex:0];
        e.ctrl = (k.modifierFlags & UIKeyModifierControl) != 0;
        e.alt = (k.modifierFlags & UIKeyModifierAlternate) != 0;
        e.shift = (k.modifierFlags & UIKeyModifierShift) != 0;
        post(e);
    }
}
- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self handlePresses:presses down:YES];
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self handlePresses:presses down:NO];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self handlePresses:presses down:NO];
}
@end

// A mouse or trackpad (iPad): raw deltas and buttons, for MacGetMouseState.
static std::atomic<bool> g_mouse_connected{false};  // the menus show their cursor
static void watch_mouse(GCMouse* m) {
    GCMouseInput* in = m.mouseInput;
    // The game's y grows downwards, as AppKit's deltaY.
    in.mouseMovedHandler = ^(GCMouseInput*, float dx, float dy) { mouse_motion(dx, -dy, 0); };
    in.leftButton.pressedChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { mouse_button(0, p); };
    in.rightButton.pressedChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { mouse_button(1, p); };
    in.middleButton.pressedChangedHandler = ^(GCControllerButtonInput*, float, BOOL p) { mouse_button(2, p); };
}

// The frame-rate limit the launcher chose (SCIMITAR_FPS: 24, 30, 45 or 60; none: the display's own rate,
// up to 120). The game is told that rate as the display's refresh, and the backend presents no faster.
static void apply_frame_rate() {
    UIScreen* screen = g.window.windowScene.screen;
    const int display = (int)screen.maximumFramesPerSecond;
    const char* v = std::getenv("SCIMITAR_FPS");
    const int cap = v ? std::clamp(std::atoi(v), 0, 240) : 0;
    const int fps = cap > 0 ? std::min(cap, display) : 0;
    const CGSize n = screen.nativeBounds.size;
    scimitar::set_host_display((int)std::max(n.width, n.height), (int)std::min(n.width, n.height),
                               fps > 0 ? (double)fps : (double)display);
    scimitar::metal::set_frame_cap(fps);
}

// ---- the on-screen pad: touchpad_ios.mm ----
// What the scene calls where it used to call start_game(): the pad shows (when the setting and the
// connected controllers say so) and follows controllers coming and going.
static void start_game_with_pad() {
    static bool watching = false;
    if (!watching) {
        watching = true;
        for (NSNotificationName n in @[ GCControllerDidConnectNotification, GCControllerDidDisconnectNotification ])
            [NSNotificationCenter.defaultCenter addObserverForName:n
                                                            object:nil
                                                             queue:NSOperationQueue.mainQueue
                                                        usingBlock:^(NSNotification*) { touchpad_refresh(); }];
    }
    touchpad_refresh();
    apply_frame_rate();
    start_game();
}

@interface ScimitarSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end
@implementation ScimitarSceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UIWindowScene* ws = (UIWindowScene*)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:ws];
    ScimitarViewController* vc = [ScimitarViewController new];
    self.window.rootViewController = vc;
    g.window = self.window;
    g.view = [[ScimitarView alloc] initWithFrame:self.window.bounds];
    g.view.userInteractionEnabled = NO;
    g.layer = (CAMetalLayer*)g.view.layer;
    [vc.view addSubview:g.view];
    UIView* pad = make_touchpad();  // above the game, hidden until the game starts
    pad.frame = vc.view.bounds;
    pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [vc.view addSubview:pad];
    [self.window makeKeyAndVisible];

    // The screen as the game's display: its pixels (landscape) and refresh rate.
    UIScreen* screen = ws.screen;
    const CGSize n = screen.nativeBounds.size;
    scimitar::set_host_display((int)std::max(n.width, n.height), (int)std::min(n.width, n.height),
                               (double)screen.maximumFramesPerSecond);

    for (GCMouse* m in GCMouse.mice) watch_mouse(m);
    g_mouse_connected = GCMouse.mice.count > 0;
    [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification* note) {
                                                    watch_mouse(note.object);
                                                    g_mouse_connected = true;
                                                }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidDisconnectNotification object:nil
                                                     queue:NSOperationQueue.mainQueue
                                                usingBlock:^(NSNotification*) {
                                                    g_mouse_connected = GCMouse.mice.count > 0;
                                                }];
    layout();
#ifdef SCIMITAR_LAUNCHER
    // The launcher first, over the game's view, unless the run is scripted.
    bool scripted = false;
    for (const char* v : {"SCIMITAR_NO_LAUNCHER", "SCIMITAR_KEYS", "SCIMITAR_PAD", "SCIMITAR_EXIT_FRAME"})
        scripted = scripted || std::getenv(v);
    if (!scripted) {
        const std::string documents = std::string(std::getenv("HOME")) + "/Documents/" + game::kInfo.app_name;
        UIViewController* launcher = (__bridge_transfer UIViewController*)scimitar_launcher_make(
            game::kInfo.id, game::kInfo.title, game::kInfo.app_env, game::kInfo.app_name, documents.c_str(), [] {
                // The app is landscape-only; this just makes sure the game comes back in landscape.
                UIViewController* root = g.window.rootViewController;
                [root dismissViewControllerAnimated:YES completion:^{
                    [root setNeedsUpdateOfSupportedInterfaceOrientations];
                    UIWindowSceneGeometryPreferencesIOS* landscape = [[UIWindowSceneGeometryPreferencesIOS alloc]
                        initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
                    [g.window.windowScene requestGeometryUpdateWithPreferences:landscape errorHandler:nil];
                }];
                start_game_with_pad();
            });
        launcher.modalPresentationStyle = UIModalPresentationFullScreen;
        dispatch_async(dispatch_get_main_queue(), ^{
            [vc presentViewController:launcher animated:NO completion:nil];
        });
        return;
    }
#endif
    start_game_with_pad();
}
- (void)sceneDidBecomeActive:(UIScene*)scene { set_foreground(true); }
- (void)sceneWillResignActive:(UIScene*)scene {}
- (void)sceneDidEnterBackground:(UIScene*)scene { set_foreground(false); }
- (void)sceneWillEnterForeground:(UIScene*)scene { set_foreground(true); }
@end

@interface ScimitarAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation ScimitarAppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    // Game audio plays with the silent switch off, alone.
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategorySoloAmbient error:nil];
    [AVAudioSession.sharedInstance setActive:YES error:nil];
    return YES;
}
- (UISceneConfiguration*)application:(UIApplication*)app
    configurationForConnectingSceneSession:(UISceneSession*)session
                                   options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* c = [[UISceneConfiguration alloc] initWithName:@"Game" sessionRole:session.role];
    c.delegateClass = ScimitarSceneDelegate.class;
    return c;
}
@end

int main(int argc, char** argv) {
    g_argc = argc, g_argv = argv;
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(ScimitarAppDelegate.class));
    }
}

// ---- the game's Mac* functions ----

namespace {
void switch_mode_if_needed();  // below
}

namespace scimitar::overrides {

using scimitar::arg32;
using scimitar::Context;
using scimitar::shim_return;
using namespace shell;

// void MacApplicationInit(void): UIKit is already running. Touch drives the
// menus, so their cursor is left out unless a mouse is connected.
void ov_MacApplicationInit(Context& c) {
    hide_ui_cursor([] { return !g_mouse_connected.load(); });
    shim_return(c, 0);
}

// NSWindow* MacCreateWindow(void): the scene's window, made before the game started.
void ov_MacCreateWindow(Context& c) { shim_return(c, kWindowHandle); }

// void MacInitializeOpenGL(NSWindow*): the backend takes the view's layer.
void ov_MacInitializeOpenGL(Context& c) {
    __block CAMetalLayer* layer = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{ layer = g.layer; });
    scimitar::metal::attach_layer((__bridge void*)layer, c.mem, g_mode.have_w, g_mode.have_h);
    dispatch_sync(dispatch_get_main_queue(), ^{ layout(); });
    shim_return(c, 0);
}

// void MacSetContextOpenGL(void): one Metal backend, nothing to make current.
void ov_MacSetContextOpenGL(Context& c) { shim_return(c, 0); }

// void MacUpdateOpenGL(void): presents the frame (and counts it, shell_common.mm).
void ov_MacUpdateOpenGL(Context& c) {
    count_frame([](int* w, int* h, std::vector<uint8_t>* px) { return scimitar::metal::read_default(w, h, px); });
    scimitar::metal::present();
    wait_for_foreground();
    shim_return(c, 0);
}

// void MacVSyncOpenGL(NSWindow*, bool on)
void ov_MacVSyncOpenGL(Context& c) {
    scimitar::metal::set_vsync((arg32(c, 1) & 0xff) != 0);
    shim_return(c, 0);
}

// void MacApplicationLoop(void): hands the game the input UIKit queued.
void ov_MacApplicationLoop(Context& c) {
    pump(c);
    shim_return(c, 0);
}

// void MacSwitchModeIfNeeded(void), on the render thread: the default
// framebuffer takes the mode's size at once, the view's letterbox follows.
void ov_MacSwitchModeIfNeeded(Context& c) {
    switch_mode_if_needed();
    shim_return(c, 0);
}

// void MacSwitchModeOpenGL(bool fullscreen, int width, int height, int refresh)
// (Brotherhood): requests and applies the mode at once.
void ov_MacSwitchModeOpenGL(Context& c) {
    request_mode(arg32(c, 0) & 0xff, (int)arg32(c, 1), (int)arg32(c, 2));
    switch_mode_if_needed();
    shim_return(c, 0);
}

// bool MacLockFocusIfCanDraw(void), void MacUnlockFocus(void) (Brotherhood).
void ov_MacLockFocusIfCanDraw(Context& c) { shim_return(c, 1); }
void ov_MacUnlockFocus(Context& c) { shim_return(c, 0); }

}  // namespace scimitar::overrides

namespace {

void switch_mode_if_needed() {
    Mode& m = g_mode;
    if (m.pending && m.want_w > 0 && m.want_h > 0) {
        scimitar::metal::set_default_size(m.want_w, m.want_h);
        scimitar::set_display_mode(m.want_w, m.want_h);
        const int w = m.want_w, h = m.want_h;
        on_main(^{
            g.aspect_w = w, g.aspect_h = h;
            layout();
        });
        m.have_w = m.want_w, m.have_h = m.want_h;
    }
    m.pending = false;
}

}  // namespace

namespace scimitar::overrides {

// CGDirectDisplayID MacGetDisplayID(void): the one display (coregraphics.cpp).
void ov_MacGetDisplayID(Context& c) { shim_return(c, 1); }

}  // namespace scimitar::overrides
