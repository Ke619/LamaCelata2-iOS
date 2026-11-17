// The on-screen pad (iPhone, iPad): touchpad_ios.mm.
#pragma once
#import <UIKit/UIKit.h>

namespace shell {

// The overlay to put above the game's view (full screen). Its size and opacity, the layout of its
// buttons and when it shows are kept in the user defaults (touchpad.*), set in the launcher and by
// the menu (the three dots) in the game. Making it also tells the game it has a pad, unless the setting is off.
UIView* make_touchpad(void);

// A controller connected or went away: shows or hides the overlay (the "automatic" setting).
void touchpad_refresh(void);

}  // namespace shell
