// The on-screen pad (iPhone, iPad), for when no controller is connected.
//
// A transparent view over the game with the whole controller.
//  - Left side, wherever there is no button: the left stick. It appears under the thumb (or, with
//    "Fixed left stick" on, stays in one place). Three quick taps lock a run: the stick stays pushed
//    forward until the next three taps.
//  - Right side, wherever there is no button: the right stick (the camera). It is drawn as a line and
//    a dot, which the menu can hide.
//  - Six buttons in the bottom-right: X and A along the bottom, LT Y B RT in a row above them. The very corner
//    is left empty, because the game's minimap is there. RT (the small one at the far right) toggles (sprint)
//    unless the launcher says otherwise.
//  - The D-pad is one button: hold it, a wheel opens under the finger, flick up, down, left or right.
//  - Select, Start and the menu button (the three dots) across the top.
//  - The menu: Edit, Mouse, L3 / R3 / LB / RB (press them there, or pin them onto the screen with the
//    plus), the fixed stick and camera trail switches, and Reset layout.
// Everything feeds shell_common.mm's virtual pad (g_virtual_buttons, g_virtual_axis), which the game
// reads as a controller.
//
// Mouse (in the menu) lets touches that are not on a button through to the game's own touch-as-mouse,
// for the menus. Edit (in the menu) opens the layout editor: drag a button to move it, tap one to resize it,
// and sliders for the overall size and opacity. Everything is kept in the user defaults (touchpad.*);
// the launcher has the same settings.
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "scimitar/gamepad.h"
#include "shell_common.h"
#include "touchpad_ios.h"

namespace {

using B = scimitar::GamepadState;

// ---- settings (user defaults; the launcher writes the same keys) ----

NSString* const kMode = @"touchpad.mode";        // "auto" (no controller connected), "on", "off"
NSString* const kScale = @"touchpad.scale";      // overall size, 0.6 ... 1.6
NSString* const kOpacity = @"touchpad.opacity";  // 0.15 ... 1
NSString* const kHaptics = @"touchpad.haptics";  // a tap on a button
NSString* const kToggleRT = @"touchpad.toggleRT";  // RT (sprint) stays down after a tap, until the next tap
NSString* const kFixed = @"touchpad.fixedStick";   // the left stick stays in one place instead of under the thumb
NSString* const kTrail = @"touchpad.trail";        // the line and dot that follow the finger on the camera side
// {control: {x, y (centre, fractions of the screen), s (its size), p (pinned: an optional button put on the screen)}}
NSString* const kLayout = @"touchpad.layout";
// Which default layout the saved settings were made for. When it is older than kLayoutVersion the saved layout, size
// and opacity are dropped once, so everyone starts from the current defaults (the same as "Reset layout").
NSString* const kLayoutVersion = @"touchpad.layoutVersion";
const NSInteger kCurrentLayoutVersion = 9;
const float kDefaultScale = 0.855f;
const float kDefaultOpacity = 0.68f;

void migrate_layout() {
    NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
    if ([d integerForKey:kLayoutVersion] >= kCurrentLayoutVersion) return;
    [d removeObjectForKey:kLayout];
    [d removeObjectForKey:kScale];
    [d removeObjectForKey:kOpacity];
    [d setBool:NO forKey:kFixed];
    [d setInteger:kCurrentLayoutVersion forKey:kLayoutVersion];
}

float pref(NSString* key, float fallback) {
    id v = [NSUserDefaults.standardUserDefaults objectForKey:key];
    return [v isKindOfClass:NSNumber.class] ? [v floatValue] : fallback;
}

NSString* mode_pref() {
    NSString* m = [NSUserDefaults.standardUserDefaults stringForKey:kMode];
    return m.length ? m : @"auto";
}

float clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

// ---- the controls ----

enum Kind { kStick, kDPad, kRound };
enum Special { kNone, kMenu };
enum Icon { kNoIcon, kPause, kDots, kArrows };
enum Place { kFrac, kCorner };

// Sizes are in "units": one unit is 1/100 of the screen's width (times the overall size setting).
struct Def {
    const char* name;   // the key in the saved layout
    Kind kind;
    const char* label;
    uint32_t bit;       // the pad button (B::...), 0 for none
    int trigger;        // shell::VLeftTrigger / VRightTrigger for a trigger, else -1
    Special special;
    bool optional;      // not on the screen until pinned in the menu
    Place place;        // kFrac: px, py are the centre as fractions of the screen
                        // kCorner: px, py are the centre's distance from the bottom-right corner, in units
    float px, py;
    float w;            // diameter in units at scale 1
    float fs;           // label size, percent of the diameter
    Icon icon;          // drawn instead of a label
    float r, g, b;      // colour
};

const std::vector<Def>& defs() {
    using namespace shell;
    static const std::vector<Def> d = {
        // name     kind    label     bit              trigger        special optional place    px     py     w     fs  icon     r    g    b
        {"lstick", kStick, "",       0,               -1,            kNone,  false, kFrac,   .198f, .713f, 16.5f,  0, kNoIcon, .75, .75, .75},
        {"dpad",   kDPad,  "",       0,               -1,            kNone,  false, kFrac,   .14f,  .296f, 8.0f,  0, kArrows, .75, .75, .75},
        // the brick: X A B along the bottom, LT Y RT in the gaps above
        {"x",      kRound, "X",      B::X,            -1,            kNone,  false, kCorner, 27.6f,  8.0f,  8.6f, 46, kNoIcon, .51, .71, 1.0},
        {"a",      kRound, "A",      B::A,            -1,            kNone,  false, kCorner, 17.8f,  8.0f,  9.6f, 50, kNoIcon, .49, .86, .55},
        {"b",      kRound, "B",      B::B,            -1,            kNone,  false, kCorner, 12.9f, 16.6f,  8.6f, 46, kNoIcon, 1.0, .54, .50},
        {"lt",     kRound, "LT",     0,               VLeftTrigger,  kNone,  false, kCorner, 32.5f, 16.6f,  8.6f, 34, kNoIcon, 1.0, 1.0, 1.0},
        {"y",      kRound, "Y",      B::Y,            -1,            kNone,  false, kCorner, 22.7f, 16.6f,  8.6f, 46, kNoIcon, 1.0, .83, .48},
        {"rt",     kRound, "RT",     0,               VRightTrigger, kNone,  false, kCorner,  4.2f, 17.1f,  7.6f, 34, kNoIcon, 1.0, 1.0, 1.0},
        // across the top
        {"back",   kRound, "Select", B::Back,         -1,            kNone,  false, kFrac,   .444f, .095f, 6.5f, 22, kNoIcon, 1.0, 1.0, 1.0},
        {"start",  kRound, "",       B::Start,        -1,            kNone,  false, kFrac,   .520f, .095f, 6.5f,  0, kPause,  1.0, 1.0, 1.0},
        {"menu",   kRound, "",       0,               -1,            kMenu,  false, kFrac,   .596f, .095f, 6.5f,  0, kDots,   1.0, 1.0, 1.0},
        // optional: pinned onto the screen from the menu
        {"lb",     kRound, "LB",     B::LeftShoulder,  -1,           kNone,  true,  kFrac,   .09f,  .13f,  7.0f, 34, kNoIcon, 1.0, 1.0, 1.0},
        {"rb",     kRound, "RB",     B::RightShoulder, -1,           kNone,  true,  kFrac,   .91f,  .13f,  7.0f, 34, kNoIcon, 1.0, 1.0, 1.0},
        {"l3",     kRound, "L3",     B::LeftThumb,     -1,           kNone,  true,  kFrac,   .28f,  .64f,  5.5f, 36, kNoIcon, 1.0, 1.0, 1.0},
        {"r3",     kRound, "R3",     B::RightThumb,    -1,           kNone,  true,  kFrac,   .66f,  .40f,  5.5f, 36, kNoIcon, 1.0, 1.0, 1.0},
    };
    return d;
}

CGPoint default_center(const Def& d, CGRect b, UIEdgeInsets inset, CGFloat unit, CGFloat scale) {
    if (d.place == kCorner)
        return CGPointMake(b.size.width - inset.right - d.px * scale * unit, b.size.height - inset.bottom - d.py * scale * unit);
    return CGPointMake(d.px * b.size.width, d.py * b.size.height);
}

CGPoint clamp_center(CGPoint c, CGSize size, CGRect b, UIEdgeInsets inset) {
    const CGFloat lo_x = inset.left + size.width / 2, hi_x = b.size.width - inset.right - size.width / 2;
    const CGFloat lo_y = inset.top + size.height / 2, hi_y = b.size.height - inset.bottom - size.height / 2;
    return CGPointMake(hi_x >= lo_x ? std::min(std::max(c.x, lo_x), hi_x) : b.size.width / 2,
                       hi_y >= lo_y ? std::min(std::max(c.y, lo_y), hi_y) : b.size.height / 2);
}

const uint32_t kDirBits[4] = {B::Up, B::Down, B::Left, B::Right};  // the D-pad wheel's arms

UIColor* white(CGFloat alpha) { return [UIColor colorWithWhite:1 alpha:alpha]; }
UIColor* amber(CGFloat alpha) { return [UIColor colorWithRed:1.0 green:0.83 blue:0.48 alpha:alpha]; }
UIColor* coral() { return [UIColor colorWithRed:1.0 green:0.54 blue:0.50 alpha:1]; }

bool physical_pad_present() {
    for (GCController* c in GCController.controllers)
        if (c.extendedGamepad) return true;
    return false;
}

// The three drawn labels, from a 100 x 100 box scaled to the button's diameter.
UIBezierPath* icon_path(Icon icon, CGFloat s) {
    UIBezierPath* p = [UIBezierPath bezierPath];
    const CGFloat k = s / 100;
    auto tri = [&](CGFloat ax, CGFloat ay, CGFloat bx, CGFloat by, CGFloat cx, CGFloat cy) {
        [p moveToPoint:CGPointMake(ax * k, ay * k)];
        [p addLineToPoint:CGPointMake(bx * k, by * k)];
        [p addLineToPoint:CGPointMake(cx * k, cy * k)];
        [p closePath];
    };
    switch (icon) {
        case kPause:
            [p appendPath:[UIBezierPath bezierPathWithRoundedRect:CGRectMake(30 * k, 27 * k, 13 * k, 46 * k) cornerRadius:4 * k]];
            [p appendPath:[UIBezierPath bezierPathWithRoundedRect:CGRectMake(57 * k, 27 * k, 13 * k, 46 * k) cornerRadius:4 * k]];
            break;
        case kDots:
            for (CGFloat cx : {24.0, 50.0, 76.0})
                [p appendPath:[UIBezierPath bezierPathWithArcCenter:CGPointMake(cx * k, 50 * k) radius:6.5 * k startAngle:0 endAngle:2 * M_PI clockwise:YES]];
            break;
        case kArrows:
            tri(50, 22, 58, 33, 42, 33);
            tri(50, 78, 58, 67, 42, 67);
            tri(22, 50, 33, 42, 33, 58);
            tri(78, 50, 67, 42, 67, 58);
            break;
        default:
            break;
    }
    return p;
}

// The menu's buttons: a pill (or a small round one for the pins), white outline, filled white when "on".
void style_button(UIButton* b, NSString* title, CGFloat size, BOOL on, UIColor* accent, BOOL round) {
    UIButtonConfiguration* cfg = [UIButtonConfiguration plainButtonConfiguration];
    cfg.contentInsets = round ? NSDirectionalEdgeInsetsZero : NSDirectionalEdgeInsetsMake(5, 11, 5, 11);
    UIColor* base = accent ?: UIColor.whiteColor;
    cfg.titleLineBreakMode = NSLineBreakByTruncatingTail;  // one line, never wrapped
    cfg.attributedTitle = [[NSAttributedString alloc]
        initWithString:title
            attributes:@{
                NSFontAttributeName : [UIFont systemFontOfSize:size weight:UIFontWeightBold],
                NSForegroundColorAttributeName : on ? UIColor.blackColor : base
            }];
    b.configuration = cfg;
    b.backgroundColor = on ? base : white(0.14);
    b.layer.cornerRadius = round ? 13 : 15;
    b.layer.borderWidth = 1.5;
    b.layer.borderColor = (accent ?: white(0.6)).CGColor;
}

}  // namespace

// A control on screen, with what its finger is doing.
@interface TPControl : NSObject
@property(nonatomic) NSInteger index;        // into defs()
@property(nonatomic) float scale;            // its own size, times the overall one
@property(nonatomic) CGPoint fraction;       // its centre as fractions of the screen; x < 0: the default place
@property(nonatomic) BOOL pinned;            // an optional button that is on the screen
@property(nonatomic) BOOL hasOverride;       // the floating left stick: centred where the finger went down
@property(nonatomic) CGPoint touchCentre;
@property(nonatomic, strong) UIView* view;
@property(nonatomic, strong) UIView* thumb;  // a stick's
@property(nonatomic, strong) UILabel* lockLabel;  // the left stick's "run locked"
@property(nonatomic, strong) UILabel* label;
@property(nonatomic, strong) CAShapeLayer* icon;
@property(nonatomic, strong) CAGradientLayer* sheen;  // the brick's soft highlight, top left
@property(nonatomic, strong) UITouch* touch;
@property(nonatomic) BOOL pressed;
@property(nonatomic) BOOL latched;         // a toggling button (RT) held down by a tap
@property(nonatomic) uint32_t dirs;          // D-pad directions held
@property(nonatomic) CGPoint anchor;         // where the finger went down (the D-pad's flick starts there)
@property(nonatomic) CGPoint vec;            // stick position, -1 ... 1, +y up
@end
@implementation TPControl
@end

@interface ScimitarTouchPad : UIView
- (void)updateVisibility;
@end

@implementation ScimitarTouchPad {
    NSArray<TPControl*>* _controls;
    TPControl* _lstick;
    // the camera: a line from where the finger went down to a dot under it
    UIView* _camTrail;
    UIView* _camDot;
    UITouch* _rightTouch;
    CGPoint _rightOrigin;
    CGPoint _rightVec;
    // the left stick
    UITouch* _leftTouch;
    CGPoint _leftBegin;
    NSTimeInterval _leftBeginTime;
    NSMutableArray<NSNumber*>* _taps;  // times of the last short taps, for the run lock
    BOOL _locked;
    // the D-pad's wheel
    UIView* _wheel;
    NSArray<UILabel*>* _wheelArrows;
    BOOL _editing, _mouseMode, _haptics, _toggleRT, _fixed, _trail;
    float _scale, _opacity;
    TPControl* _selected;
    TPControl* _dragging;
    UITouch* _dragTouch;
    CGPoint _dragOffset;
    // the menu (the three dots)
    UIView* _drawer;
    UIButton *_mouseButton, *_fixedButton, *_trailButton, *_resetButton;
    UIButton* _panelReset;  // the editor's Reset: only for the selected button
    NSArray<UIButton*>* _pinButtons;
    BOOL _resetArmed;
    NSUInteger _resetGeneration;
    // the editor
    UIView* _panel;
    UISlider *_sizeSlider, *_opacitySlider, *_selectedSlider;
    UILabel* _selectedLabel;
    UIImpactFeedbackGenerator* _haptic;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    migrate_layout();
    _scale = clamp(pref(kScale, kDefaultScale), 0.6f, 1.6f);
    _opacity = clamp(pref(kOpacity, kDefaultOpacity), 0.15f, 1.0f);
    _haptics = pref(kHaptics, 1) != 0;
    _toggleRT = pref(kToggleRT, 1) != 0;
    _fixed = pref(kFixed, 0) != 0;
    _trail = pref(kTrail, 1) != 0;
    _taps = [NSMutableArray array];
    _haptic = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];

    NSDictionary* saved = [NSUserDefaults.standardUserDefaults dictionaryForKey:kLayout];
    NSMutableArray<TPControl*>* all = [NSMutableArray array];
    for (size_t i = 0; i < defs().size(); i++) {
        TPControl* c = [TPControl new];
        c.index = (NSInteger)i;
        c.scale = 1;
        c.fraction = CGPointMake(-1, -1);
        NSDictionary* s = saved[@(defs()[i].name)];
        if ([s isKindOfClass:NSDictionary.class]) {
            if (s[@"x"] && s[@"y"]) c.fraction = CGPointMake(clamp([s[@"x"] floatValue], 0, 1), clamp([s[@"y"] floatValue], 0, 1));
            if (s[@"s"]) c.scale = clamp([s[@"s"] floatValue], 0.5f, 2.0f);
            if (s[@"p"]) c.pinned = [s[@"p"] boolValue];
        }
        [self buildViews:c];
        [all addObject:c];
        if (defs()[i].kind == kStick) _lstick = c;
    }
    _controls = all;

    _camTrail = [UIView new];
    _camTrail.userInteractionEnabled = NO;
    _camTrail.hidden = YES;
    _camTrail.backgroundColor = white(0.45);
    _camTrail.layer.anchorPoint = CGPointMake(0, 0.5);
    [self addSubview:_camTrail];
    _camDot = [UIView new];
    _camDot.userInteractionEnabled = NO;
    _camDot.hidden = YES;
    _camDot.backgroundColor = white(0.7);
    [self addSubview:_camDot];

    [self buildWheel];
    [self buildDrawer];
    [self buildPanel];
    return self;
}

- (CGFloat)unit {
    return self.bounds.size.width / 100;
}

- (TPControl*)controlNamed:(const char*)name {
    for (TPControl* c in _controls)
        if (!strcmp(defs()[c.index].name, name)) return c;
    return nil;
}

// ---- building ----

- (void)buildViews:(TPControl*)c {
    const Def& d = defs()[c.index];
    UIView* v = [UIView new];
    v.userInteractionEnabled = NO;
    c.view = v;
    [self addSubview:v];
    if (d.place == kCorner) {
        // a soft highlight at 34% / 28% that fades out by about 62% of the way to the far corner
        CAGradientLayer* g = [CAGradientLayer layer];
        g.type = kCAGradientLayerRadial;
        g.startPoint = CGPointMake(0.34, 0.28);
        g.endPoint = CGPointMake(1.32, 1.26);
        g.colors = @[ (id)white(0.30).CGColor, (id)white(0.07).CGColor ];
        g.locations = @[ @0, @0.62 ];
        g.masksToBounds = YES;
        g.actions = @{@"bounds" : NSNull.null, @"position" : NSNull.null, @"cornerRadius" : NSNull.null};
        [v.layer insertSublayer:g atIndex:0];
        c.sheen = g;
    }
    if (d.kind == kStick) {
        UIView* t = [UIView new];
        t.userInteractionEnabled = NO;
        [v addSubview:t];
        c.thumb = t;
        UILabel* l = [UILabel new];
        l.text = @"run locked";
        l.textAlignment = NSTextAlignmentCenter;
        l.textColor = amber(1);
        l.hidden = YES;
        [v addSubview:l];
        c.lockLabel = l;
    } else if (d.icon != kNoIcon) {
        CAShapeLayer* s = [CAShapeLayer layer];
        [v.layer addSublayer:s];
        c.icon = s;
    } else {
        UILabel* l = [UILabel new];
        l.text = @(d.label);
        l.textAlignment = NSTextAlignmentCenter;
        l.adjustsFontSizeToFitWidth = YES;
        l.minimumScaleFactor = 0.6;
        [v addSubview:l];
        c.label = l;
    }
}

- (void)buildWheel {
    _wheel = [UIView new];
    _wheel.userInteractionEnabled = NO;
    _wheel.hidden = YES;
    _wheel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.4];
    _wheel.layer.borderWidth = 1.5;
    _wheel.layer.borderColor = white(0.5).CGColor;
    NSMutableArray<UILabel*>* arrows = [NSMutableArray array];
    for (NSString* a in @[ @"▲", @"▼", @"◀", @"▶" ]) {  // the order of kDirBits
        UILabel* l = [UILabel new];
        l.text = a;
        l.textAlignment = NSTextAlignmentCenter;
        l.textColor = white(0.55);
        [_wheel addSubview:l];
        [arrows addObject:l];
    }
    _wheelArrows = arrows;
    [self addSubview:_wheel];
}

- (void)updateWheel:(uint32_t)dirs {
    for (int k = 0; k < 4; k++) _wheelArrows[k].textColor = (dirs & kDirBits[k]) ? amber(1) : white(0.55);
}

- (void)showWheelAround:(TPControl*)c {
    const CGFloat d = 21 * [self unit] * _scale, a = 4 * [self unit] * _scale;
    _wheel.bounds = CGRectMake(0, 0, d, d);
    _wheel.layer.cornerRadius = d / 2;
    _wheel.center = c.view.center;
    const CGPoint at[4] = {CGPointMake(d / 2, d * 0.07 + a / 2), CGPointMake(d / 2, d * 0.93 - a / 2),
                           CGPointMake(d * 0.09 + a / 2, d / 2), CGPointMake(d * 0.91 - a / 2, d / 2)};
    for (int k = 0; k < 4; k++) {
        _wheelArrows[k].bounds = CGRectMake(0, 0, a, a);
        _wheelArrows[k].center = at[k];
        _wheelArrows[k].font = [UIFont systemFontOfSize:3 * [self unit] * _scale];
    }
    [self updateWheel:0];
    _wheel.hidden = NO;
}

// The menu behind the three dots. It sits at the top centre and closes before the editor opens.
- (void)buildDrawer {
    _drawer = [UIView new];
    _drawer.backgroundColor = [UIColor colorWithWhite:0 alpha:0.62];
    _drawer.layer.cornerRadius = 14;
    _drawer.hidden = YES;

    UIButton* edit = [self menuButton:@"Edit" size:13 tag:-1];
    [edit addTarget:self action:@selector(editTapped) forControlEvents:UIControlEventTouchUpInside];
    _mouseButton = [self menuButton:@"Mouse" size:13 tag:-1];
    [_mouseButton addTarget:self action:@selector(mouseTapped) forControlEvents:UIControlEventTouchUpInside];

    // L3 R3 LB RB: press them here, or pin them onto the screen with the plus
    NSMutableArray<UIView*>* chips = [NSMutableArray array];
    NSMutableArray<UIButton*>* pins = [NSMutableArray array];
    for (const char* name : {"l3", "r3", "lb", "rb"}) {
        TPControl* c = [self controlNamed:name];
        const Def& d = defs()[c.index];
        UIButton* hold = [self menuButton:@(d.label) size:13 tag:c.index];
        [hold.widthAnchor constraintGreaterThanOrEqualToConstant:40].active = YES;
        [hold addTarget:self action:@selector(holdDown:) forControlEvents:UIControlEventTouchDown];
        [hold addTarget:self action:@selector(holdUp:)
            forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
        UIButton* pin = [UIButton buttonWithType:UIButtonTypeCustom];
        pin.tag = c.index;
        [pin.widthAnchor constraintEqualToConstant:26].active = YES;
        [pin.heightAnchor constraintEqualToConstant:26].active = YES;
        [pin addTarget:self action:@selector(pinTapped:) forControlEvents:UIControlEventTouchUpInside];
        style_button(pin, c.pinned ? @"✓" : @"+", 12, c.pinned, nil, YES);
        pin.accessibilityLabel = [NSString stringWithFormat:@"Put %s on the screen", d.label];
        [pins addObject:pin];
        UIStackView* chip = [self rowOf:@[ hold, pin ] spacing:4];
        [chips addObject:chip];
    }
    _pinButtons = pins;

    _fixedButton = [self menuButton:@"" size:12 tag:-1];
    [_fixedButton addTarget:self action:@selector(fixedTapped) forControlEvents:UIControlEventTouchUpInside];
    _trailButton = [self menuButton:@"" size:12 tag:-1];
    [_trailButton addTarget:self action:@selector(trailTapped) forControlEvents:UIControlEventTouchUpInside];
    _resetButton = [self menuButton:@"" size:12 tag:-1];
    [_resetButton addTarget:self action:@selector(resetMenuTapped) forControlEvents:UIControlEventTouchUpInside];
    [self syncDrawer];

    // Two wide rows keep the menu short: Edit, Mouse and the two switches along the top; L3 R3 LB RB and
    // Reset layout (which takes the room that is left) along the bottom.
    UIStackView* rowTop = [self rowOf:@[ edit, _mouseButton, _fixedButton, _trailButton ] spacing:8];
    NSMutableArray<UIView*>* bottom = [chips mutableCopy];
    [bottom addObject:_resetButton];
    for (UIView* chip in chips) [chip setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_resetButton setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* rowBottom = [self rowOf:bottom spacing:8];
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ rowTop, rowBottom ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.alignment = UIStackViewAlignmentFill;
    stack.spacing = 6;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_drawer addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:_drawer.topAnchor constant:8],
        [stack.bottomAnchor constraintEqualToAnchor:_drawer.bottomAnchor constant:-8],
        [stack.leadingAnchor constraintEqualToAnchor:_drawer.leadingAnchor constant:10],
        [stack.trailingAnchor constraintEqualToAnchor:_drawer.trailingAnchor constant:-10],
    ]];
    [self addSubview:_drawer];
}

- (UIButton*)menuButton:(NSString*)title size:(CGFloat)size tag:(NSInteger)tag {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeCustom];
    b.tag = tag;
    style_button(b, title, size, NO, nil, NO);
    [b.heightAnchor constraintGreaterThanOrEqualToConstant:30].active = YES;
    // The menu is sized to fit its widest row (the compressed size), which squeezed these down to their padding
    // and cut the labels off: a button now always keeps the width of its text.
    [b setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [b setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    return b;
}

- (UIStackView*)rowOf:(NSArray<UIView*>*)views spacing:(CGFloat)spacing {
    UIStackView* row = [[UIStackView alloc] initWithArrangedSubviews:views];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = spacing;
    return row;
}

// The menu's switches and pins, from the current state.
- (void)syncDrawer {
    style_button(_mouseButton, @"Mouse", 13, _mouseMode, nil, NO);
    style_button(_fixedButton, _fixed ? @"Fixed left stick: on" : @"Fixed left stick: off", 12, _fixed, nil, NO);
    style_button(_trailButton, _trail ? @"Camera trail: on" : @"Camera trail: off", 12, _trail, nil, NO);
    style_button(_resetButton, _resetArmed ? @"Tap again to reset" : @"Reset layout", 12, _resetArmed, coral(), NO);
    for (UIButton* pin in _pinButtons) {
        const BOOL on = _controls[pin.tag].pinned;
        style_button(pin, on ? @"✓" : @"+", 12, on, nil, YES);
    }
    [self setNeedsLayout];
}

- (UILabel*)panelLabel:(NSString*)text {
    UILabel* l = [UILabel new];
    l.text = text;
    l.textColor = UIColor.whiteColor;
    l.font = [UIFont systemFontOfSize:11];
    l.adjustsFontSizeToFitWidth = YES;
    l.minimumScaleFactor = 0.7;
    return l;
}

- (UIStackView*)panelColumn:(UILabel*)label slider:(UISlider*)s {
    UIStackView* col = [[UIStackView alloc] initWithArrangedSubviews:@[ label, s ]];
    col.axis = UILayoutConstraintAxisVertical;
    col.spacing = 2;
    return col;
}

// The editor's panel is small and sits at the top centre, under the top row of buttons, so the buttons being
// moved (the brick, the sticks, the D-pad) are all free to touch while it is open.
- (void)buildPanel {
    _panel = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 400, 88)];
    _panel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.62];  // the same as the menu's
    _panel.layer.cornerRadius = 14;
    _panel.hidden = YES;

    UILabel* title = [UILabel new];
    title.text = @"Edit controls";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont systemFontOfSize:13 weight:UIFontWeightBold];
    UILabel* hint = [UILabel new];
    hint.text = @"Drag a button to move it. Tap one to resize it.";
    hint.textColor = white(0.65);
    hint.font = [UIFont systemFontOfSize:10];
    hint.adjustsFontSizeToFitWidth = YES;
    hint.minimumScaleFactor = 0.7;
    UIStackView* titles = [[UIStackView alloc] initWithArrangedSubviews:@[ title, hint ]];
    titles.axis = UILayoutConstraintAxisVertical;
    titles.spacing = 1;

    UIButton* reset = [UIButton buttonWithType:UIButtonTypeCustom];
    style_button(reset, @"Reset button", 12, NO, coral(), NO);
    [reset addTarget:self action:@selector(resetTapped) forControlEvents:UIControlEventTouchUpInside];
    _panelReset = reset;
    UIButton* done = [UIButton buttonWithType:UIButtonTypeCustom];
    style_button(done, @"Done", 12, YES, nil, NO);  // filled white
    [done addTarget:self action:@selector(doneTapped) forControlEvents:UIControlEventTouchUpInside];
    for (UIButton* b in @[ reset, done ]) {
        [b.heightAnchor constraintEqualToConstant:26].active = YES;
        [b.widthAnchor constraintGreaterThanOrEqualToConstant:58].active = YES;
        [b setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    }
    UIStackView* buttons = [[UIStackView alloc] initWithArrangedSubviews:@[ reset, done ]];
    buttons.axis = UILayoutConstraintAxisHorizontal;
    buttons.spacing = 8;
    [buttons setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView* header = [[UIStackView alloc] initWithArrangedSubviews:@[ titles, buttons ]];
    header.axis = UILayoutConstraintAxisHorizontal;
    header.alignment = UIStackViewAlignmentCenter;
    header.spacing = 12;

    _sizeSlider = [UISlider new];
    _sizeSlider.minimumValue = 0.6;
    _sizeSlider.maximumValue = 1.6;
    [_sizeSlider addTarget:self action:@selector(sizeChanged:) forControlEvents:UIControlEventValueChanged];
    _opacitySlider = [UISlider new];
    _opacitySlider.minimumValue = 0.15;
    _opacitySlider.maximumValue = 1.0;
    [_opacitySlider addTarget:self action:@selector(opacityChanged:) forControlEvents:UIControlEventValueChanged];
    _selectedSlider = [UISlider new];
    _selectedSlider.minimumValue = 0.5;
    _selectedSlider.maximumValue = 2.0;
    [_selectedSlider addTarget:self action:@selector(selectedChanged:) forControlEvents:UIControlEventValueChanged];
    for (UISlider* sl in @[ _sizeSlider, _opacitySlider, _selectedSlider ]) {
        sl.minimumTrackTintColor = UIColor.whiteColor;
        sl.maximumTrackTintColor = white(0.3);
        sl.thumbTintColor = UIColor.whiteColor;
    }
    _selectedLabel = [self panelLabel:@"Selected"];

    UIStackView* sliders = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self panelColumn:[self panelLabel:@"Overall size"] slider:_sizeSlider],
        [self panelColumn:[self panelLabel:@"Opacity"] slider:_opacitySlider],
        [self panelColumn:_selectedLabel slider:_selectedSlider]
    ]];
    sliders.axis = UILayoutConstraintAxisHorizontal;
    sliders.distribution = UIStackViewDistributionFillEqually;
    sliders.spacing = 12;

    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[ header, sliders ]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 4;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_panel addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:_panel.topAnchor constant:8],
        [stack.leadingAnchor constraintEqualToAnchor:_panel.leadingAnchor constant:12],
        [stack.trailingAnchor constraintEqualToAnchor:_panel.trailingAnchor constant:-12],
        [stack.bottomAnchor constraintEqualToAnchor:_panel.bottomAnchor constant:-8],
    ]];
    [self addSubview:_panel];
}

// ---- layout and looks ----

- (void)layoutSubviews {
    [super layoutSubviews];
    for (TPControl* c in _controls) [self placeControl:c];
    // the editor's panel and the menu: top centre, just under the top row of buttons
    const CGFloat panelW = std::min<CGFloat>(400, self.bounds.size.width - 24);
    const CGSize panelFit = [_panel systemLayoutSizeFittingSize:CGSizeMake(panelW, UILayoutFittingCompressedSize.height)
                                  withHorizontalFittingPriority:UILayoutPriorityRequired
                                        verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
    _panel.frame = CGRectMake((self.bounds.size.width - panelW) / 2, self.bounds.size.height * 0.15, panelW, panelFit.height);
    const CGSize fit = [_drawer systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
    _drawer.frame = CGRectMake((self.bounds.size.width - std::min<CGFloat>(fit.width, self.bounds.size.width - 24)) / 2, self.bounds.size.height * 0.15, std::min<CGFloat>(fit.width, self.bounds.size.width - 24), fit.height);
}

- (CGSize)sizeOf:(TPControl*)c {
    const CGFloat side = defs()[c.index].w * [self unit] * _scale * c.scale;
    return CGSizeMake(side, side);
}

- (BOOL)shouldHide:(TPControl*)c {
    const Def& d = defs()[c.index];
    if (d.optional && !c.pinned) return YES;
    // the left stick is drawn only under a finger, when it is fixed, or while a run is locked
    if (d.kind == kStick) return !(c.pressed || _fixed || _locked);
    return NO;
}

- (void)placeControl:(TPControl*)c {
    const Def& d = defs()[c.index];
    const CGFloat unit = [self unit];
    const CGSize size = [self sizeOf:c];
    const CGRect b = self.bounds;
    const UIEdgeInsets inset = self.safeAreaInsets;
    CGPoint centre;
    if (c.hasOverride) {
        centre = c.touchCentre;  // under the finger: not moved to fit
    } else {
        centre = c.fraction.x >= 0 ? CGPointMake(c.fraction.x * b.size.width, c.fraction.y * b.size.height)
                                   : default_center(d, b, inset, unit, _scale);
        centre = clamp_center(centre, size, b, inset);
    }
    c.view.hidden = [self shouldHide:c];
    c.view.bounds = CGRectMake(0, 0, size.width, size.height);
    c.view.center = centre;
    c.view.alpha = _editing ? std::max(_opacity, 0.75f) : _opacity;
    c.view.layer.cornerRadius = size.width / 2;
    if (c.sheen) {
        c.sheen.frame = c.view.bounds;
        c.sheen.cornerRadius = size.width / 2;
    }
    if (d.kind == kStick) {
        const CGFloat h = unit * 2.2;
        c.lockLabel.font = [UIFont systemFontOfSize:unit * 1.5 weight:UIFontWeightSemibold];
        c.lockLabel.frame = CGRectMake(-size.width / 2, -h - unit * 0.4, size.width * 2, h);
    } else if (c.icon) {
        c.icon.frame = c.view.bounds;
        c.icon.path = icon_path(d.icon, size.width).CGPath;
    } else {
        c.label.frame = CGRectInset(c.view.bounds, size.width * 0.09, 0);
        c.label.font = [UIFont systemFontOfSize:size.width * d.fs / 100 weight:UIFontWeightBold];
    }
    [self refresh:c];
}

- (void)refresh:(TPControl*)c {
    const Def& d = defs()[c.index];
    const BOOL selected = _editing && c == _selected;
    const BOOL on = c.pressed || c.latched;
    const CGFloat unit = [self unit];
    const CGFloat border = std::max<CGFloat>(1.5, unit * 0.32);
    UIColor* tint = [UIColor colorWithRed:d.r green:d.g blue:d.b alpha:1];
    UIColor* red = [UIColor colorWithRed:0.9 green:0.2 blue:0.2 alpha:1];
    if (d.kind == kStick) {
        const CGFloat w = c.view.bounds.size.width, t = w * 0.46, travel = (w - t) / 2;
        c.view.backgroundColor = white(0.10);
        c.thumb.bounds = CGRectMake(0, 0, t, t);
        c.thumb.layer.cornerRadius = t / 2;
        c.thumb.center = CGPointMake(w / 2 + c.vec.x * travel, w / 2 - c.vec.y * travel);
        c.thumb.backgroundColor = white(0.55);
        c.lockLabel.hidden = !_locked;
        c.view.layer.borderWidth = selected ? 3 : border;
        c.view.layer.borderColor = (selected ? red : _locked ? amber(1) : white(0.6)).CGColor;
        return;
    }
    const BOOL cluster = d.place == kCorner;
    const BOOL amberLatch = c.latched;
    UIColor* fill;
    UIColor* ink;
    if (on) {
        fill = amberLatch ? amber(0.55) : cluster ? [tint colorWithAlphaComponent:0.55] : white(0.55);
        ink = amberLatch ? [UIColor colorWithRed:0.10 green:0.11 blue:0.14 alpha:1] : UIColor.whiteColor;
    } else {
        fill = cluster ? [UIColor colorWithRed:0.04 green:0.05 blue:0.07 alpha:0.28] : white(0.14);
        ink = cluster ? [tint colorWithAlphaComponent:(d.r == d.g && d.g == d.b ? 0.9 : 1.0)] : UIColor.whiteColor;
    }
    c.view.backgroundColor = fill;
    c.sheen.hidden = on;
    c.view.layer.borderWidth = selected ? 3 : border;
    c.view.layer.borderColor = (selected ? red : cluster ? [tint colorWithAlphaComponent:0.9] : white(0.6)).CGColor;
    c.view.transform = on ? CGAffineTransformMakeScale(0.93, 0.93) : CGAffineTransformIdentity;
    c.label.textColor = ink;
    c.icon.fillColor = ink.CGColor;
    if (d.special == kMenu && !_drawer.hidden) c.view.backgroundColor = white(0.55);
}

- (void)relayoutAll {
    for (TPControl* c in _controls) [self placeControl:c];
}

// ---- showing and hiding ----

- (void)updateVisibility {
    NSString* mode = mode_pref();
    const BOOL want = [mode isEqualToString:@"on"] || ([mode isEqualToString:@"auto"] && !physical_pad_present());
    if (want == !self.hidden) return;
    self.hidden = !want;
    if (!want) {
        if (_editing) [self setEditing:NO];
        [self releaseAll];
    }
    [self publish];
}

- (void)releaseAll {
    for (TPControl* c in _controls) {
        c.touch = nil;
        c.pressed = NO;
        c.latched = NO;
        c.dirs = 0;
        c.vec = CGPointZero;
        c.hasOverride = NO;
        [self placeControl:c];
    }
    _rightTouch = nil;
    _rightVec = CGPointZero;
    _leftTouch = nil;
    _locked = NO;
    [_taps removeAllObjects];
    _camDot.hidden = _camTrail.hidden = YES;
    _wheel.hidden = YES;
    _drawer.hidden = YES;
    [self disarmReset];
    [self relayoutAll];
}

// ---- to the game ----

- (void)publish {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, lt = 0, rt = 0, rx = 0, ry = 0;
    if (!_editing && !self.hidden) {
        for (TPControl* c in _controls) {
            const Def& d = defs()[c.index];
            if (c.pressed || c.latched) {
                buttons |= d.bit;
                if (d.trigger == shell::VLeftTrigger) lt = 1;
                if (d.trigger == shell::VRightTrigger) rt = 1;
            }
            if (d.kind == kDPad) buttons |= c.dirs;
            if (d.kind == kStick) lx = (float)c.vec.x, ly = (float)c.vec.y;
        }
        rx = (float)_rightVec.x, ry = (float)_rightVec.y;
    }
    shell::g_virtual_buttons.store(buttons);
    shell::g_virtual_axis[shell::VLeftX].store(lx);
    shell::g_virtual_axis[shell::VLeftY].store(ly);
    shell::g_virtual_axis[shell::VRightX].store(rx);
    shell::g_virtual_axis[shell::VRightY].store(ry);
    shell::g_virtual_axis[shell::VLeftTrigger].store(lt);
    shell::g_virtual_axis[shell::VRightTrigger].store(rt);
}

// ---- touches ----

- (TPControl*)controlAt:(CGPoint)p {
    TPControl* best = nil;
    CGFloat bestDistance = CGFLOAT_MAX;
    for (TPControl* c in _controls) {
        if (c.view.hidden) continue;
        const Def& d = defs()[c.index];
        if (d.kind == kStick && !_editing) continue;  // the left stick is the whole left side, see beginLeft
        const CGFloat grow = d.kind == kStick ? -18 : -10;
        if (!CGRectContainsPoint(CGRectInset(c.view.frame, grow, grow), p)) continue;
        const CGFloat distance = hypot(p.x - c.view.center.x, p.y - c.view.center.y);
        if (distance < bestDistance) best = c, bestDistance = distance;
    }
    return best;
}

- (UIView*)hitTest:(CGPoint)p withEvent:(UIEvent*)event {
    if (self.hidden || !self.userInteractionEnabled) return nil;
    if (_editing && !_panel.hidden && CGRectContainsPoint(_panel.frame, p)) {
        UIView* v = [_panel hitTest:[self convertPoint:p toView:_panel] withEvent:event];
        return v ?: _panel;
    }
    if (!_editing && !_drawer.hidden && CGRectContainsPoint(_drawer.frame, p)) {
        UIView* v = [_drawer hitTest:[self convertPoint:p toView:_drawer] withEvent:event];
        return v ?: _drawer;
    }
    if (_editing || !_mouseMode) return self;  // the pad takes the whole screen (the sides are sticks)
    return [self controlAt:p] ? self : nil;    // mouse: only the buttons, the rest is the game's touch
}

- (void)tick {
    if (_haptics) [_haptic impactOccurred];
}

- (BOOL)inLeftZone:(CGPoint)p {
    return p.x < self.bounds.size.width * 0.46 && p.y > self.bounds.size.height * 0.16;
}

- (BOOL)inRightZone:(CGPoint)p {
    return p.x >= self.bounds.size.width * 0.46 && p.y > self.bounds.size.height * 0.16;
}

- (void)start:(TPControl*)c at:(CGPoint)p {
    const Def& d = defs()[c.index];
    if (_toggleRT && d.trigger == shell::VRightTrigger) {  // sprint: a tap holds it, the next tap lets go
        c.latched = !c.latched;
        [self tick];
        [self refresh:c];
        return;
    }
    c.pressed = YES;
    if (d.kind == kDPad) {  // hold, then flick: the wheel opens under the finger
        c.anchor = p;
        [self showWheelAround:c];
    }
    [self tick];
    [self refresh:c];
}

- (void)move:(TPControl*)c to:(CGPoint)p {
    const Def& d = defs()[c.index];
    if (d.kind == kStick) {
        const CGPoint centre = c.view.center;
        const CGFloat r = c.view.bounds.size.width / 2;
        CGFloat x = (p.x - centre.x) / r, y = -(p.y - centre.y) / r;
        const CGFloat len = hypot(x, y);
        if (len > 1) x /= len, y /= len;
        // a locked run stays pushed forward until the thumb really steers
        c.vec = (_locked && std::min<CGFloat>(len, 1) < 0.25) ? CGPointMake(0, 1) : CGPointMake(x, y);
    } else if (d.kind == kDPad) {
        const CGFloat dx = p.x - c.anchor.x, dy = p.y - c.anchor.y;
        uint32_t dirs = 0;
        if (hypot(dx, dy) > self.bounds.size.width * 0.012) {  // a small flick is enough; one direction at a time
            if (fabs(dx) > fabs(dy)) dirs = dx > 0 ? B::Right : B::Left;
            else dirs = dy > 0 ? B::Down : B::Up;
        }
        if (dirs != c.dirs && dirs) [self tick];
        c.dirs = dirs;
        [self updateWheel:dirs];
    }
    [self refresh:c];
}

- (void)finish:(TPControl*)c at:(CGPoint)p cancelled:(BOOL)cancelled {
    const Def& d = defs()[c.index];
    c.pressed = NO;
    c.dirs = 0;
    c.vec = CGPointZero;
    if (d.kind == kDPad) _wheel.hidden = YES;
    const BOOL inside = CGRectContainsPoint(CGRectInset(c.view.frame, -10, -10), p);
    if (!cancelled && inside && d.special == kMenu) [self toggleDrawer];
    [self refresh:c];
}

// the left stick

- (void)beginLeft:(UITouch*)t at:(CGPoint)p {
    if (_fixed && hypot(p.x - _lstick.view.center.x, p.y - _lstick.view.center.y) > _lstick.view.bounds.size.width * 0.9) return;
    _leftTouch = t;
    _leftBegin = p;
    _leftBeginTime = t.timestamp;
    _lstick.pressed = YES;
    _lstick.hasOverride = !_fixed;
    _lstick.touchCentre = p;
    _lstick.vec = _locked ? CGPointMake(0, 1) : CGPointZero;
    [self placeControl:_lstick];
}

// Back to rest: gone (or in its fixed place), pushed forward if a run is locked.
- (void)idleLeft {
    _lstick.pressed = NO;
    _lstick.hasOverride = NO;
    _lstick.vec = _locked ? CGPointMake(0, 1) : CGPointZero;
    [self placeControl:_lstick];
}

- (void)endLeft:(UITouch*)t at:(CGPoint)p cancelled:(BOOL)cancelled {
    _leftTouch = nil;
    // three quick taps in a row (each a short press that barely moves) toggle the run lock
    if (!cancelled && t.timestamp - _leftBeginTime < 0.25 && hypot(p.x - _leftBegin.x, p.y - _leftBegin.y) < 14) {
        [_taps addObject:@(t.timestamp)];
        if (_taps.count > 3) [_taps removeObjectAtIndex:0];
        if (_taps.count == 3 && t.timestamp - _taps[0].doubleValue < 0.7) {
            [_taps removeAllObjects];
            _locked = !_locked;
            [self tick];
        }
    } else {
        [_taps removeAllObjects];
    }
    [self idleLeft];
}

// the camera

- (void)beginRight:(UITouch*)t at:(CGPoint)p {
    _rightTouch = t;
    _rightOrigin = p;
    _rightVec = CGPointZero;
    const CGFloat dot = 3 * [self unit];
    _camDot.bounds = CGRectMake(0, 0, dot, dot);
    _camDot.layer.cornerRadius = dot / 2;
    _camDot.center = p;
    _camTrail.bounds = CGRectMake(0, 0, 0, std::max<CGFloat>(2, 0.35 * [self unit]));
    _camTrail.layer.position = p;
    _camTrail.transform = CGAffineTransformIdentity;
    _camDot.alpha = _camTrail.alpha = _opacity;
    _camDot.hidden = _camTrail.hidden = !_trail;
}

- (void)moveRight:(CGPoint)p {
    const CGFloat r = self.bounds.size.width * 0.09;
    const CGFloat dx = p.x - _rightOrigin.x, dy = p.y - _rightOrigin.y;
    CGFloat x = dx / r, y = -dy / r;
    const CGFloat len = hypot(x, y);
    if (len > 1) x /= len, y /= len;
    _rightVec = CGPointMake(x, y);
    _camDot.center = p;
    _camTrail.bounds = CGRectMake(0, 0, hypot(dx, dy), _camTrail.bounds.size.height);
    _camTrail.transform = CGAffineTransformMakeRotation(atan2(dy, dx));
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* t in touches) {
        if (t.view != self) continue;  // the menu and the editor's panel handle their own
        const CGPoint p = [t locationInView:self];
        TPControl* c = [self controlAt:p];
        if (_editing) {
            [self select:c];
            if (c && !_dragTouch) {
                _dragTouch = t;
                _dragging = c;
                _dragOffset = CGPointMake(p.x - c.view.center.x, p.y - c.view.center.y);
            }
            continue;
        }
        if (c && !c.touch) {
            c.touch = t;
            [self start:c at:p];
        } else if (!c && !_mouseMode) {
            if (!_leftTouch && [self inLeftZone:p]) [self beginLeft:t at:p];
            else if (!_rightTouch && [self inRightZone:p]) [self beginRight:t at:p];
        }
    }
    [self publish];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* t in touches) {
        if (t.view != self) continue;
        const CGPoint p = [t locationInView:self];
        if (_editing) {
            if (t == _dragTouch && _dragging) {
                const CGPoint centre = clamp_center(CGPointMake(p.x - _dragOffset.x, p.y - _dragOffset.y), [self sizeOf:_dragging],
                                                    self.bounds, self.safeAreaInsets);
                _dragging.fraction = CGPointMake(centre.x / self.bounds.size.width, centre.y / self.bounds.size.height);
                [self placeControl:_dragging];
            }
            continue;
        }
        if (t == _leftTouch) {
            [self move:_lstick to:p];
            continue;
        }
        if (t == _rightTouch) {
            [self moveRight:p];
            continue;
        }
        for (TPControl* c in _controls)
            if (c.touch == t) [self move:c to:p];
    }
    [self publish];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches cancelled:NO];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches cancelled:YES];
}

- (void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled {
    for (UITouch* t in touches) {
        if (t.view != self) continue;
        const CGPoint p = [t locationInView:self];
        if (t == _dragTouch) {
            _dragTouch = nil;
            _dragging = nil;
            [self saveLayout];
            continue;
        }
        if (t == _leftTouch) {
            [self endLeft:t at:p cancelled:cancelled];
            continue;
        }
        if (t == _rightTouch) {
            _rightTouch = nil;
            _rightVec = CGPointZero;
            _camDot.hidden = _camTrail.hidden = YES;
        }
        for (TPControl* c in _controls)
            if (c.touch == t) {
                c.touch = nil;
                [self finish:c at:p cancelled:cancelled];
            }
    }
    [self publish];
}

// ---- the menu ----

- (void)toggleDrawer {
    _drawer.hidden = !_drawer.hidden;
    if (_drawer.hidden) [self disarmReset];
    [self syncDrawer];
    [self setNeedsLayout];
    [self layoutIfNeeded];
    [self refresh:[self controlNamed:"menu"]];
}

- (void)editTapped {
    [self setEditing:YES];  // closes the menu, so the editor's panel never sits on top of it
}

- (void)mouseTapped {
    _mouseMode = !_mouseMode;
    [self syncDrawer];
}

// L3 R3 LB RB pressed from the menu
- (void)holdDown:(UIButton*)b {
    TPControl* c = _controls[b.tag];
    c.pressed = YES;
    [self tick];
    [self publish];
}

- (void)holdUp:(UIButton*)b {
    TPControl* c = _controls[b.tag];
    c.pressed = NO;
    [self publish];
}

- (void)pinTapped:(UIButton*)b {
    TPControl* c = _controls[b.tag];
    c.pinned = !c.pinned;
    [self placeControl:c];
    [self saveLayout];
    [self syncDrawer];
}

- (void)fixedTapped {
    _fixed = !_fixed;
    [NSUserDefaults.standardUserDefaults setBool:_fixed forKey:kFixed];
    if (!_leftTouch) [self idleLeft];
    [self syncDrawer];
}

- (void)trailTapped {
    _trail = !_trail;
    [NSUserDefaults.standardUserDefaults setBool:_trail forKey:kTrail];
    if (!_trail) _camDot.hidden = _camTrail.hidden = YES;  // only the drawing: the camera works the same
    [self syncDrawer];
}

// Reset layout asks twice, so it can't be hit by accident: the first tap arms it for three seconds.
- (void)resetMenuTapped {
    if (!_resetArmed) {
        _resetArmed = YES;
        const NSUInteger generation = ++_resetGeneration;
        [self syncDrawer];
        __weak ScimitarTouchPad* weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            ScimitarTouchPad* me = weakSelf;
            if (me && me->_resetArmed && me->_resetGeneration == generation) [me disarmReset];
        });
        return;
    }
    [self disarmReset];
    [self resetLayout];
}

- (void)disarmReset {
    if (!_resetArmed) return;
    _resetArmed = NO;
    _resetGeneration++;
    [self syncDrawer];
}

// Back to the default layout: every button in its place and size, the optional ones off the screen,
// the left stick floating again, the overall size and opacity as they start. The camera trail setting is
// a preference, not layout, and stays.
- (void)resetLayout {
    _scale = kDefaultScale;
    _opacity = kDefaultOpacity;
    _fixed = NO;
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setFloat:_scale forKey:kScale];
    [defaults setFloat:_opacity forKey:kOpacity];
    [defaults setBool:NO forKey:kFixed];
    [defaults removeObjectForKey:kLayout];
    for (TPControl* c in _controls) {
        c.fraction = CGPointMake(-1, -1);
        c.scale = 1;
        c.pinned = NO;
    }
    if (!_leftTouch) [self idleLeft];
    [self select:nil];
    [self relayoutAll];
    [self syncPanel];
    [self syncDrawer];
}

// ---- the editor ----

- (void)setEditing:(BOOL)editing {
    _editing = editing;
    _panel.hidden = !editing;
    [self setNeedsLayout];
    [self releaseAll];  // also closes the menu: the menu and the panel are never on screen together
    [self select:nil];
    [self syncPanel];
    [self relayoutAll];
    [self publish];
}

- (void)select:(TPControl*)c {
    _selected = c;
    [self syncPanel];
    for (TPControl* x in _controls) [self refresh:x];
}

- (void)syncPanel {
    _sizeSlider.value = _scale;
    _opacitySlider.value = _opacity;
    _selectedSlider.enabled = _selected != nil;
    _selectedSlider.value = _selected ? _selected.scale : 1;
    if (_selected) {
        const Def& d = defs()[_selected.index];
        NSString* name = d.kind == kStick ? @"left stick" : d.kind == kDPad ? @"D-pad" : d.label[0] ? @(d.label) : @(d.name);
        _selectedLabel.text = [NSString stringWithFormat:@"Size: %@", name];
    } else {
        _selectedLabel.text = @"Selected";
    }
    _selectedLabel.alpha = _selected ? 1 : 0.5;
    _panelReset.enabled = _selected != nil;  // nothing selected, nothing to reset
    _panelReset.alpha = _selected ? 1 : 0.4;
}

- (void)sizeChanged:(UISlider*)s {
    _scale = s.value;
    [NSUserDefaults.standardUserDefaults setFloat:_scale forKey:kScale];
    [self relayoutAll];
}

- (void)opacityChanged:(UISlider*)s {
    _opacity = s.value;
    [NSUserDefaults.standardUserDefaults setFloat:_opacity forKey:kOpacity];
    [self relayoutAll];
}

- (void)selectedChanged:(UISlider*)s {
    if (!_selected) return;
    _selected.scale = s.value;
    [self placeControl:_selected];
    [self saveLayout];
}

- (void)doneTapped {
    [self saveLayout];
    [self setEditing:NO];
}

// The editor's own Reset: the same as the menu's Reset layout, without asking twice.
// The editor's Reset: only the selected button goes back to its own place and size (an optional button stays on
// the screen if it was pinned). The whole layout is reset from the menu's "Reset layout".
- (void)resetTapped {
    if (!_selected) return;
    _selected.fraction = CGPointMake(-1, -1);
    _selected.scale = 1;
    [self placeControl:_selected];
    [self saveLayout];
    [self syncPanel];
}

- (void)saveLayout {
    NSMutableDictionary* out = [NSMutableDictionary dictionary];
    for (TPControl* c in _controls) {
        NSMutableDictionary* e = [NSMutableDictionary dictionary];
        if (c.fraction.x >= 0) e[@"x"] = @(c.fraction.x), e[@"y"] = @(c.fraction.y);
        if (c.scale != 1) e[@"s"] = @(c.scale);
        if (c.pinned) e[@"p"] = @YES;
        if (e.count) out[@(defs()[c.index].name)] = e;
    }
    [NSUserDefaults.standardUserDefaults setObject:out forKey:kLayout];
}

@end

namespace {
ScimitarTouchPad* g_touchpad;
}

namespace shell {

UIView* make_touchpad(void) {
    if (!g_touchpad) {
        // The game looks for pads once, at its start: tell it there is one unless the setting is off.
        g_virtual_pad_present = ![mode_pref() isEqualToString:@"off"];
        g_touchpad = [[ScimitarTouchPad alloc] initWithFrame:CGRectZero];
        g_touchpad.hidden = YES;
    }
    return g_touchpad;
}

void touchpad_refresh(void) { [g_touchpad updateVisibility]; }

}  // namespace shell
