#include "UpdateNoticePlatform.hpp"
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>
#include <mutex>

namespace update_notice::platform {
namespace {
std::mutex mutex;
std::atomic<bool> cancelled = false;
NSURLSessionDataTask* task = nil;
}

std::optional<std::string> get(std::string const& url, size_t cap) {
    NSURL* address = [NSURL URLWithString:@(url.c_str())];
    if (!address) return std::nullopt;
    auto configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.timeoutIntervalForResource = 15;
    // The completion handler runs on this queue; draining it before returning keeps the handler
    // from running after the plugin unloads.
    auto queue = [NSOperationQueue new];
    auto session = [NSURLSession sessionWithConfiguration:configuration delegate:nil delegateQueue:queue];
    auto done = dispatch_semaphore_create(0);
    __block std::optional<std::string> body;
    auto started = [session dataTaskWithURL:address
                          completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                              auto http = [response isKindOfClass:[NSHTTPURLResponse class]]
                                  ? static_cast<NSHTTPURLResponse*>(response)
                                  : nil;
                              if (!error && http.statusCode == 200 && data.length <= cap)
                                  body = std::string(static_cast<char const*>(data.bytes), data.length);
                              dispatch_semaphore_signal(done);
                          }];
    {
        std::lock_guard lock(mutex);
        if (cancelled) {
            [session invalidateAndCancel];
            return std::nullopt;
        }
        task = started;
    }
    [started resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    {
        std::lock_guard lock(mutex);
        task = nil;
    }
    [session finishTasksAndInvalidate];
    [queue waitUntilAllOperationsAreFinished];
    return body;
}

std::optional<bool> ask(Notice const& notice) {
    auto header = "OBS Jukebox " + notice.latest + " is out for Geometry Dash " + notice.latestGd;
    auto message = "Your version (" + notice.running + ") only works with GD " + notice.runningGd +
        ", so the game mod stops loading after the GD update. Install " + notice.latest +
        " to get it working again.";
    NSDictionary* content = @{
        (__bridge NSString*)kCFUserNotificationAlertHeaderKey: @(header.c_str()),
        (__bridge NSString*)kCFUserNotificationAlertMessageKey: @(message.c_str()),
        (__bridge NSString*)kCFUserNotificationDefaultButtonTitleKey: @"Open Download Page",
        (__bridge NSString*)kCFUserNotificationAlternateButtonTitleKey: @"Later",
    };
    SInt32 error = 0;
    auto alert = CFUserNotificationCreate(
        kCFAllocatorDefault, 0, kCFUserNotificationNoteAlertLevel, &error,
        (__bridge CFDictionaryRef)content
    );
    if (!alert) return std::nullopt;
    CFOptionFlags response = kCFUserNotificationCancelResponse;
    while (CFUserNotificationReceiveResponse(alert, .25, &response) != 0) {
        if (cancelled) {
            CFUserNotificationCancel(alert);
            response = kCFUserNotificationCancelResponse;
            break;
        }
    }
    CFRelease(alert);
    return (response & 3) == kCFUserNotificationDefaultResponse;
}

void cancel() {
    std::lock_guard lock(mutex);
    cancelled = true;
    [task cancel];
}

void open(char const* url) {
    if (auto address = [NSURL URLWithString:@(url)]) LSOpenCFURLRef((__bridge CFURLRef)address, nullptr);
}
}
