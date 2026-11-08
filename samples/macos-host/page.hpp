#pragma once

// The Apple host samples' pages (Objective-C++, Foundation): a page folder read as hosts load it, and the example
// pages the samples switch between. samples/macos-host reads them from the source tree, samples/ios-host from its
// bundle.
#import <Foundation/Foundation.h>

#include "engine.hpp"

#include <string>

namespace demo {
    // The pages the samples switch between, as folders of examples: the HUD, the four logo variants, the page to try
    // touch input on, right-to-left and complex-script text, then images.
    constexpr const char* switchable[]{"engine-host", "logo/color", "logo/dark", "logo/light", "logo/static",
                                       "touch",       "bidi",       "img",       "effects",    "animate"};

    inline std::string text_of(id value) {
        return [value isKindOfClass:[NSString class]] ? std::string([(NSString*)value UTF8String]) : std::string();
    }

    // A page folder as hosts load it: page.json ({"scripts": [...], "stylesheet": "...", "keyframes": "...", "root":
    // "...", "crpgBridge": false, "forgivingCss": false, "webPlatform": false}, paths relative to the folder), or the
    // HUD's hud.js, hud.css and hud-keyframes.css.
    inline Page load_page(const std::string& folder) {
        const std::string manifest = folder + "/page.json";
        if (![[NSFileManager defaultManager] fileExistsAtPath:@(manifest.c_str())]) {
            return hud_page(folder);
        }
        NSData* data = [NSData dataWithContentsOfFile:@(manifest.c_str())];
        NSDictionary* json = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        check([json isKindOfClass:[NSDictionary class]], "cannot read " + manifest);
        Page page;
        NSString* path = @(folder.c_str());
        page.name = std::string(path.stringByDeletingLastPathComponent.lastPathComponent.UTF8String) + "/" +
                    path.lastPathComponent.UTF8String;
        for (id script in json[@"scripts"]) {
            const auto name = text_of(script);
            page.scripts.emplace_back(name, read(folder + "/" + name));
        }
        const auto stylesheet = text_of(json[@"stylesheet"]);
        page.stylesheet = read(folder + "/" + stylesheet);
        page.base = folder + "/" + stylesheet;
        if (const auto keyframes = text_of(json[@"keyframes"]); !keyframes.empty()) {
            page.keyframes = read(folder + "/" + keyframes);
        }
        page.root = text_of(json[@"root"]);
        const auto flag = [&](NSString* key, std::uint32_t bit) {
            if ([json[key] respondsToSelector:@selector(boolValue)] && [json[key] boolValue]) {
                page.flags |= bit;
            }
        };
        flag(@"crpgBridge", WEFT_PAGE_CRPG_BRIDGE);
        flag(@"forgivingCss", WEFT_PAGE_FORGIVING_CSS);
        flag(@"webPlatform", WEFT_PAGE_WEB_PLATFORM);
        return page;
    }
} // namespace demo
