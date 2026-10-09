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
        auto size = CCDirector::get()->getWinSize();
        CCPoint point{float(p.x / bounds.size.width * size.width), float(p.y / bounds.size.height * size.height)};
        Loader::get()->queueInMainThread([point] { separate_song::jukebox_link::rightClick(point); });
    }
    [view performSelector:original withObject:event];
}
$on_mod(Loaded) {
    auto result = ObjcHook::create("EAGLView", "rightMouseDown:", &rightMouse);
    if (result) {
        auto hook = std::move(result).unwrap();
        (void)Mod::get()->claimHook(std::move(hook));
    } else {
        log::error("Could not install Jukebox right-click input: {}", result.unwrapErr());
    }
}
