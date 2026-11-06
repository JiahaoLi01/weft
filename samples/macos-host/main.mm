// A game engine's side of weft on macOS (Objective-C++, ARC), through the public headers only (include/weft): the HUD
// page of examples/engine-host (or another page folder: keys 1-5 switch to the logo pages of examples/logo, R reloads)
// in a window, over the engine's own picture of its world, drawn by weft's Metal backend
// into the window's CAMetalLayer drawables. As samples/engine-host does on Windows: units walk and the engine writes
// their positions as anchors, their health to the view model and damage to hud.damage events; the page's end-turn
// button calls the engine, which answers; the minimap is a texture the engine draws every frame. The pointer goes to the
// page (hover, click); a click on the world where the page has nothing to interact with damages the unit there.
//
//   weft_macos_demo [--page <page>] [--vsync] [--fullscreen] [--capture <file.png>] [--quit-after S]
//                                                                   a window, frames as fast as the GPU draws them (V or
//                                                                   --vsync waits for the display; F full screen);
//                                                                   --capture writes what it shows after 3 s as a PNG,
//                                                                   --quit-after closes it after S s
//
// A page is a folder (page.json, or the HUD's hud.js) or one of the examples' names: hud, logo (logo/color), color,
// dark, light, static, logo/<variant>.
//   weft_macos_demo --shot <file.png> [--frames N] [--size WxH] [--scale S] [--page <dir>]
//                                                                   draws N frames off screen (clicking end turn at
//                                                                   frame 30) and writes the last as a PNG
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "gpu.hpp"
#include "page.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace {
    using Clock = std::chrono::steady_clock;

    std::string page_folder = std::string(WEFT_EXAMPLES) + "/engine-host";
    // Present at the display's refresh (the layer's displaySyncEnabled) or as soon as each frame is drawn.
    bool vsync{};
    bool fullscreen{};
    std::string capture_path;
    double quit_after{};

    // The folder a page argument names: a folder of its own (page.json, or the HUD's hud.js), or one of the examples'.
    std::string resolve_page(const std::string& name) {
        namespace fs = std::filesystem;
        const std::string examples = WEFT_EXAMPLES;
        const auto page = [](const fs::path& folder) {
            return fs::exists(folder / "page.json") || fs::exists(folder / "hud.js");
        };
        std::string wanted = name;
        if (name == "hud" || name == "engine-host") {
            wanted = examples + "/engine-host";
        } else if (name == "logo") {
            wanted = examples + "/logo/color";
        }
        for (const auto& candidate : {fs::path(wanted), fs::path(examples) / wanted, fs::path(examples) / "logo" / wanted}) {
            if (page(candidate)) {
                return fs::absolute(candidate).lexically_normal().string();
            }
        }
        throw demo::Failure("no page \"" + name +
                            "\": a folder with page.json (or the HUD's hud.js), or one of hud, logo, color, dark, "
                            "light, static");
    }

    // Writes encoded premultiplied BGRA rows as a PNG.
    void write_png(const std::string& path, const std::vector<std::uint8_t>& bgra, unsigned width, unsigned height) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(const_cast<std::uint8_t*>(bgra.data()), width, height, 8,
                                                     width * 4, space,
                                                     CGBitmapInfo(kCGImageAlphaPremultipliedFirst) |
                                                         CGBitmapInfo(kCGBitmapByteOrder32Little));
        CGImageRef image = CGBitmapContextCreateImage(context);
        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        CGImageDestinationRef out =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        demo::check(out != nullptr, "cannot write " + path);
        CGImageDestinationAddImage(out, image, nullptr);
        const bool written = CGImageDestinationFinalize(out);
        CFRelease(out);
        CGImageRelease(image);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        demo::check(written, "cannot write " + path);
    }

    // Off screen: frames at 60 per second of game time, each waited for; at frame 30 the player clicks end turn.
    int shot(const std::string& path, unsigned frames, unsigned width, unsigned height, float scale) {
        demo::Engine engine(demo::load_page(page_folder), float(width), float(height), scale);
        demo::Gpu gpu;
        const unsigned w = unsigned(float(width) * scale);
        const unsigned h = unsigned(float(height) * scale);
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [gpu.device newTextureWithDescriptor:d];
        id<MTLBuffer> readback = [gpu.device newBufferWithLength:std::size_t(w) * h * 4
                                                         options:MTLResourceStorageModeShared];
        const auto start = Clock::now();
        for (unsigned n = 0; n < frames; ++n) {
            const double t = n / 60.0;
            if (n == 30 && engine.hud) {
                // The end-turn button: the page has it under the pointer, with a pointer cursor.
                const float x = float(width) - 24 - 60;
                const float y = float(height) - 24 - 16;
                const auto hit = engine.hit(x, y);
                demo::check(hit.element && std::string(hit.cursor) == "pointer", "the end-turn button under the pointer");
                engine.pointer(WEFT_POINTER_MOVE, x, y, 0);
                engine.pointer(WEFT_POINTER_DOWN, x, y, 1);
                engine.pointer(WEFT_POINTER_UP, x, y, 0);
            }
            engine.frame(t);
            gpu.frame(
                engine, t, target, w, h,
                [&](id<MTLCommandBuffer> command) {
                    if (n + 1 == frames) {
                        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                        [blit copyFromTexture:target
                                         sourceSlice:0
                                         sourceLevel:0
                                        sourceOrigin:MTLOriginMake(0, 0, 0)
                                          sourceSize:MTLSizeMake(w, h, 1)
                                            toBuffer:readback
                                   destinationOffset:0
                              destinationBytesPerRow:w * 4
                            destinationBytesPerImage:std::size_t(w) * h * 4];
                        [blit endEncoding];
                    }
                },
                [](std::uint64_t, double) {
                });
            id<MTLCommandBuffer> fence = [gpu.queue commandBuffer];
            [fence commit];
            [fence waitUntilCompleted];
            gpu.complete(gpu.serial);
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto* bytes = static_cast<const std::uint8_t*>(readback.contents);
        write_png(path, std::vector<std::uint8_t>(bytes, bytes + std::size_t(w) * h * 4), w, h);
        std::printf("%s: %u frames (%ux%u device px) in %.1f ms; page ready %s, end turn answered %u, diagnostics %u "
                    "-> %s\n",
                    engine.page_name.c_str(), frames, w, h, ms, engine.ready ? "yes" : "no", engine.answered,
                    engine.diagnostics, path.c_str());
        return (!engine.hud || (engine.ready && engine.answered == 1)) && engine.diagnostics == 0 ? 0 : 1;
    }
} // namespace

// The window's content: a CAMetalLayer drawn every display frame, and the pointer for the page and the world.
@interface WeftView : NSView
@end

@implementation WeftView {
    std::unique_ptr<demo::Engine> _engine;
    std::shared_ptr<demo::Gpu> _gpu;
    CAMetalLayer* _layer;
    dispatch_semaphore_t _inflight;
    Clock::time_point _start;
    Clock::time_point _second;
    unsigned _frames_this_second;
    double _advance_ms;
    NSTimer* _timer;
    std::uint32_t _buttons;
    bool _captured;
    id<MTLBuffer> _capture;
    // This second's frame times: the CPU's (the View's advance, encoding and committing), the wait for a drawable,
    // and the GPU's (from Metal's completed command buffers, on the main thread).
    double _cpu_ms;
    double _wait_ms;
    double _gpu_ms;
    unsigned _gpu_frames;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    self.wantsLayer = YES;
    _gpu = std::make_shared<demo::Gpu>();
    _layer = [CAMetalLayer layer];
    _layer.device = _gpu->device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Backdrop filters at the drawable's level copy from it.
    _layer.framebufferOnly = NO;
    _layer.opaque = YES;
    _layer.maximumDrawableCount = 3;
    _layer.displaySyncEnabled = vsync;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    _layer.colorspace = space;
    CGColorSpaceRelease(space);
    self.layer = _layer;
    const float scale = float(NSScreen.mainScreen.backingScaleFactor);
    _engine = std::make_unique<demo::Engine>(demo::load_page(page_folder), float(frame.size.width),
                                             float(frame.size.height), scale);
    _inflight = dispatch_semaphore_create(2);
    _start = _second = Clock::now();
    [self updateDrawableSize];
    [self schedule];
    return self;
}

// The frame timer: with vsync, a little faster than the display (the drawables pace the frames); without, as often as
// the run loop turns (a tick with two frames in flight does nothing), so frames go as fast as the GPU draws them.
- (void)schedule {
    [_timer invalidate];
    _timer = [NSTimer timerWithTimeInterval:vsync ? 1.0 / 240 : 0
                                     target:self
                                   selector:@selector(tick)
                                   userInfo:nil
                                    repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (void)updateTrackingAreas {
    for (NSTrackingArea* area in self.trackingAreas) {
        [self removeTrackingArea:area];
    }
    [self addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:self.bounds
                                   options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                           NSTrackingActiveAlways | NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:nil]];
    [super updateTrackingAreas];
}

- (void)updateDrawableSize {
    const CGFloat scale = self.window ? self.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    const NSSize size = self.bounds.size;
    _layer.contentsScale = scale;
    _layer.drawableSize = CGSizeMake(size.width * scale, size.height * scale);
    if (_engine && size.width > 0 && size.height > 0) {
        _engine->resize(float(size.width), float(size.height), float(scale));
    }
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

- (void)tick {
    // At most two frames in flight; a drawable when the layer has one free.
    if (dispatch_semaphore_wait(_inflight, DISPATCH_TIME_NOW) != 0) {
        return;
    }
    try {
        const double t = std::chrono::duration<double>(Clock::now() - _start).count();
        const auto before = Clock::now();
        _engine->frame(t);
        const auto advanced = Clock::now();
        _advance_ms = std::chrono::duration<double, std::milli>(advanced - before).count();
        id<CAMetalDrawable> drawable = [_layer nextDrawable];
        const auto acquired = Clock::now();
        _wait_ms += std::chrono::duration<double, std::milli>(acquired - advanced).count();
        if (!drawable) {
            dispatch_semaphore_signal(_inflight);
            return;
        }
        dispatch_semaphore_t inflight = _inflight;
        std::weak_ptr<demo::Gpu> gpu = _gpu;
        __weak WeftView* view = self;
        _gpu->frame(
            *_engine, t, drawable.texture, unsigned(drawable.texture.width), unsigned(drawable.texture.height),
            [&](id<MTLCommandBuffer> command) {
                if (!capture_path.empty() && !_captured && t >= 3) {
                    // What the window shows: the drawable, copied before it is presented (it is not framebufferOnly).
                    _captured = true;
                    id<MTLTexture> shown = drawable.texture;
                    const NSUInteger w = shown.width;
                    const NSUInteger h = shown.height;
                    _capture = [_gpu->device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
                    id<MTLBuffer> into = _capture;
                    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
                    [blit copyFromTexture:shown
                                     sourceSlice:0
                                     sourceLevel:0
                                    sourceOrigin:MTLOriginMake(0, 0, 0)
                                      sourceSize:MTLSizeMake(w, h, 1)
                                        toBuffer:into
                               destinationOffset:0
                          destinationBytesPerRow:w * 4
                        destinationBytesPerImage:w * h * 4];
                    [blit endEncoding];
                    [command addCompletedHandler:^(id<MTLCommandBuffer>) {
                      dispatch_async(dispatch_get_main_queue(), ^{
                        const auto* bytes = static_cast<const std::uint8_t*>(into.contents);
                        write_png(capture_path, std::vector<std::uint8_t>(bytes, bytes + w * h * 4), unsigned(w),
                                  unsigned(h));
                        std::printf("captured %lux%lu px -> %s\n", (unsigned long)w, (unsigned long)h,
                                    capture_path.c_str());
                        std::fflush(stdout);
                      });
                    }];
                }
                [command presentDrawable:drawable];
            },
            [inflight, gpu, view](std::uint64_t serial, double gpu_ms) {
                dispatch_semaphore_signal(inflight);
                // weft's renderer is the owner thread's: the completion reaches it there.
                dispatch_async(dispatch_get_main_queue(), ^{
                  if (const auto alive = gpu.lock()) {
                      alive->complete(serial);
                  }
                  [view frameTookGpu:gpu_ms];
                });
            });
        _cpu_ms += std::chrono::duration<double, std::milli>(Clock::now() - acquired).count() + _advance_ms;
    } catch (const std::exception& e) {
        dispatch_semaphore_signal(_inflight);
        std::fprintf(stderr, "frame failed: %s\n", e.what());
        [_timer invalidate];
        [NSApp terminate:nil];
        return;
    }
    ++_frames_this_second;
    const auto now = Clock::now();
    if (now - _second >= std::chrono::seconds(1)) {
        const double seconds = std::chrono::duration<double>(now - _second).count();
        const double frames = std::max(1u, _frames_this_second);
        const double cpu = _cpu_ms / frames;
        const double wait = _wait_ms / frames;
        const double gpu = _gpu_frames ? _gpu_ms / _gpu_frames : 0;
        self.window.title =
            [NSString stringWithFormat:@"weft on Metal — %s · %.0f fps · vsync %s · %ld×%ld px · CPU %.2f ms · GPU %.2f "
                                       @"ms · keys 1–5 pages, R reload, V vsync, F full screen",
                                       _engine->page_name.c_str(), _frames_this_second / seconds, vsync ? "on" : "off",
                                       long(_layer.drawableSize.width), long(_layer.drawableSize.height), cpu, gpu];
        std::printf("%s: %.0f fps (vsync %s), %ldx%ld px; per frame CPU %.2f ms (View %.2f), GPU %.2f ms, waiting "
                    "for a drawable %.2f ms; turn %u\n",
                    _engine->page_name.c_str(), _frames_this_second / seconds, vsync ? "on" : "off",
                    long(_layer.drawableSize.width), long(_layer.drawableSize.height), cpu, _advance_ms, gpu, wait,
                    _engine->turn);
        _cpu_ms = _wait_ms = _gpu_ms = 0;
        _gpu_frames = 0;
        std::fflush(stdout);
        _frames_this_second = 0;
        _second = now;
    }
    if (quit_after > 0 && std::chrono::duration<double>(now - _start).count() >= quit_after) {
        [NSApp terminate:nil];
    }
}

- (void)frameTookGpu:(double)ms {
    _gpu_ms += ms;
    ++_gpu_frames;
}

- (NSPoint)pointOf:(NSEvent*)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)pointer:(weft_pointer_kind)kind event:(NSEvent*)event {
    const NSPoint p = [self pointOf:event];
    try {
        _engine->pointer(kind, float(p.x), float(p.y), _buttons);
        // The page's cursor where it has something to interact with.
        const auto hit = _engine->hit(float(p.x), float(p.y));
        if (hit.element && std::string(hit.cursor) == "pointer") {
            [[NSCursor pointingHandCursor] set];
        } else {
            [[NSCursor arrowCursor] set];
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pointer: %s\n", e.what());
    }
}

- (void)mouseMoved:(NSEvent*)event {
    [self pointer:WEFT_POINTER_MOVE event:event];
}

- (void)mouseDragged:(NSEvent*)event {
    [self pointer:WEFT_POINTER_MOVE event:event];
}

- (void)mouseDown:(NSEvent*)event {
    _buttons |= 1;
    const NSPoint p = [self pointOf:event];
    const auto hit = _engine->hit(float(p.x), float(p.y));
    [self pointer:WEFT_POINTER_DOWN event:event];
    if (!hit.element || std::string(hit.cursor) != "pointer") {
        // The world's: the unit under the click takes damage.
        const double t = std::chrono::duration<double>(Clock::now() - _start).count();
        if (const int unit = _engine->unit_at(float(p.x), float(p.y), t); unit >= 0) {
            _engine->damage(unsigned(unit), 25);
        }
    }
}

- (void)mouseUp:(NSEvent*)event {
    _buttons &= ~1u;
    [self pointer:WEFT_POINTER_UP event:event];
}

- (void)mouseExited:(NSEvent*)event {
    try {
        _engine->pointer(WEFT_POINTER_LEAVE, 0, 0, 0);
    } catch (...) {
    }
    [[NSCursor arrowCursor] set];
}

- (void)scrollWheel:(NSEvent*)event {
    const NSPoint p = [self pointOf:event];
    try {
        // A browser's wheel delta: positive scrolls down, in px.
        _engine->pointer(WEFT_POINTER_WHEEL, float(p.x), float(p.y), _buttons, float(-event.scrollingDeltaX),
                         float(-event.scrollingDeltaY));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wheel: %s\n", e.what());
    }
}

- (void)keyDown:(NSEvent*)event {
    NSString* key = event.charactersIgnoringModifiers.lowercaseString;
    std::string folder;
    if (key.length == 1 && [key characterAtIndex:0] >= '1' && [key characterAtIndex:0] <= '5') {
        folder = std::string(WEFT_EXAMPLES) + "/" + demo::switchable[[key characterAtIndex:0] - '1'];
    } else if ([key isEqualToString:@"r"]) {
        folder = page_folder;
    } else if ([key isEqualToString:@"f"]) {
        [self.window toggleFullScreen:nil];
        return;
    } else if ([key isEqualToString:@"v"]) {
        vsync = !vsync;
        _layer.displaySyncEnabled = vsync;
        [self schedule];
        std::printf("vsync %s\n", vsync ? "on" : "off");
        return;
    } else {
        [super keyDown:event];
        return;
    }
    try {
        // The same View takes the page: its animations start over.
        _engine->load(demo::load_page(folder));
        page_folder = folder;
        std::printf("page %s\n", _engine->page_name.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "load %s: %s\n", folder.c_str(), e.what());
    }
}

- (void)shutdown {
    [_timer invalidate];
    _timer = nil;
    // The frames in flight complete before the renderer and the View go.
    _gpu.reset();
    _engine.reset();
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate {
    NSWindow* _window;
    WeftView* _view;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* item = [NSMenuItem new];
    [bar addItem:item];
    NSMenu* menu = [NSMenu new];
    [menu addItemWithTitle:@"Quit weft demo" action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = menu;
    NSApp.mainMenu = bar;

    const NSRect frame = NSMakeRect(0, 0, 1280, 720);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"weft on Metal";
    _window.contentMinSize = NSMakeSize(720, 480);
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    _window.delegate = self;
    try {
        _view = [[WeftView alloc] initWithFrame:frame];
    } catch (const std::exception& e) {
        std::fprintf(stderr, "weft demo failed: %s\n", e.what());
        [NSApp terminate:nil];
        return;
    }
    _window.contentView = _view;
    _window.acceptsMouseMovedEvents = YES;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];
    if (fullscreen) {
        [_window toggleFullScreen:nil];
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    [_view shutdown];
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        std::string shot_path;
        std::string page_name_argument;
        unsigned frames = 90;
        unsigned width = 1280;
        unsigned height = 720;
        float scale = 1;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--page" && i + 1 < argc) {
                page_name_argument = argv[++i];
            } else if (a == "--vsync") {
                vsync = true;
            } else if (a == "--fullscreen") {
                fullscreen = true;
            } else if (a == "--capture" && i + 1 < argc) {
                capture_path = argv[++i];
            } else if (a == "--quit-after" && i + 1 < argc) {
                quit_after = std::stod(argv[++i]);
            } else if (a == "--shot" && i + 1 < argc) {
                shot_path = argv[++i];
            } else if (a == "--frames" && i + 1 < argc) {
                frames = unsigned(std::stoul(argv[++i]));
            } else if (a == "--size" && i + 1 < argc) {
                std::sscanf(argv[++i], "%ux%u", &width, &height);
            } else if (a == "--scale" && i + 1 < argc) {
                scale = std::stof(argv[++i]);
            } else if (a.rfind("-NS", 0) == 0 || a.rfind("-psn", 0) == 0) {
                // Launch Services' and AppKit's own arguments.
                ++i;
            } else {
                std::fprintf(stderr, "usage: weft_macos_demo [--page page] [--vsync] [--fullscreen] [--capture file.png] "
                                     "[--quit-after S] "
                                     "[--shot file.png [--frames N] [--size WxH] [--scale S]]\n");
                return 2;
            }
        }
        if (!page_name_argument.empty()) {
            try {
                page_folder = resolve_page(page_name_argument);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "weft demo: %s\n", e.what());
                return 2;
            }
        }
        if (!shot_path.empty()) {
            try {
                return shot(shot_path, std::max(frames, 31u), width, height, scale);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "weft demo failed: %s\n", e.what());
                return 1;
            }
        }
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
