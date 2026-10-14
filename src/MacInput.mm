#import <Cocoa/Cocoa.h>
#include <Geode/Geode.hpp>
#include <Geode/utils/ObjcHook.hpp>
#include "JukeboxLink.hpp"
using namespace geode::prelude;
static void rightMouse(NSView* view, SEL original, NSEvent* event) {
    NSPoint p = [view convertPoint:event.locationInWindow fromView:nil];
    auto bounds = view.bounds;
    if (bounds.size.width > 0 && bounds.size.height > 0) {
        if (view.isFlipped) p.y = bounds.size.height - p.y;
        // This runs in AppKit's event handler; only the queued callback reads the director.
        float x = float(p.x / bounds.size.width), y = float(p.y / bounds.size.height);
        Loader::get()->queueInMainThread([x, y] {
            auto size = CCDirector::get()->getWinSize();
            separate_song::jukebox_link::rightClick({x * size.width, y * size.height});
        });
    }
    [view performSelector:original withObject:event];
}
$on_mod(Loaded) {
    auto result = ObjcHook::create("EAGLView", "rightMouseDown:", &rightMouse);
    if (result) {
        auto claimed = Mod::get()->claimHook(std::move(result).unwrap());
        if (!claimed) log::error("Could not claim Jukebox right-click input: {}", claimed.unwrapErr());
    } else {
        log::error("Could not install Jukebox right-click input: {}", result.unwrapErr());
    }
}
