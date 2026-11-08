// In-game UI on macOS (Objective-C++, ARC; docs/ingame-ui.md), through the public headers only (include/weft): three
// weft pages on panels in a 3D world, each drawn into a texture of weft's export extension only when it changes and
// sampled by the engine with mips. A control terminal whose buttons set the reactor's output (the core in the middle
// glows and turns with it), a status board the game updates once a second, and a far sign. The pointer is a ray from
// the camera: where it meets a panel, the panel's page gets it at that point (hover, press, click, wheel); elsewhere a
// drag turns the camera and the wheel moves it nearer.
//
//   weft_macos_ingame [--vsync] [--animated-ui] [--no-mips] [--capture <file.png>] [--quit-after S] [--bench S]
//       a window; Space turns the camera on its own, M toggles mips, A the board's animation (a page that changes
//       every frame), V vsync, F full screen, R resets the camera. --bench S turns the camera for S s without vsync
//       and prints the frame times' percentiles.
//   weft_macos_ingame --shot <file.png> [--frames N] [--size WxH] [--no-mips] [--animated-ui] [--profile]
//       N frames off screen, each waited for: at frame 30 the pointer clicks the terminal's +10 button through the
//       camera's ray; writes the last frame as a PNG and checks the game saw the click. --profile prints the slowest
//       frames' CPU time by phase (the frame's first excluded), as --bench does. --status-every-frame sends the board's
//       status every frame (its time to the millisecond): a page whose text changes every frame, to profile.
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "world.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;
    using namespace ingame;

    bool vsync{};
    bool animated_ui{};
    bool mips = true;
    bool profile{};
    bool status_every_frame{};
    std::string capture_path;
    double quit_after{};
    double bench_seconds{};

    // Writes rows of 8-bit BGRA (opaque, sRGB-encoded) as a PNG.
    void write_png(const std::string& path, const std::uint8_t* bgra, unsigned width, unsigned height) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(const_cast<std::uint8_t*>(bgra), width, height, 8, width * 4,
                                                     space,
                                                     CGBitmapInfo(kCGImageAlphaPremultipliedFirst) |
                                                         CGBitmapInfo(kCGBitmapByteOrder32Little));
        CGImageRef image = CGBitmapContextCreateImage(context);
        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        CGImageDestinationRef out =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        check(out != nullptr, "cannot write " + path);
        CGImageDestinationAddImage(out, image, nullptr);
        const bool written = CGImageDestinationFinalize(out);
        CFRelease(out);
        CGImageRelease(image);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        check(written, "cannot write " + path);
    }

    // Copies target into a shared buffer in command, for a PNG once it completed.
    id<MTLBuffer> copy_out(id<MTLDevice> device, id<MTLCommandBuffer> command, id<MTLTexture> target) {
        const NSUInteger w = target.width;
        const NSUInteger h = target.height;
        id<MTLBuffer> into = [device newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:target
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:into
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * h * 4];
        [blit endEncoding];
        return into;
    }

    // The pointer as the engine routes it: a ray from the camera through a point of the target picks a panel, whose
    // page gets the pointer at the point it meets; a press on a panel keeps going to it until released.
    struct Pointer {
        int hovered = -1;
        int captured = -1;
        std::uint32_t buttons{};

        // The panel and point for a pointer at x, y (0 to 1 over the target): the captured panel's plane while
        // pressed, else the nearest panel under it.
        static Pick at(const Ui& ui, const Camera& camera, float x, float y, float aspect, int captured) {
            const simd_float3 direction = camera.ray(x, y, aspect);
            if (captured >= 0) {
                Pick out;
                if (ui.plane(captured, camera.eye(), direction, out)) {
                    return out;
                }
                return {};
            }
            return ui.pick(camera.eye(), direction);
        }

        // Moves the pointer: the page under it gets a move, the page it left a leave. Returns the pick.
        Pick move(Ui& ui, const Camera& camera, float x, float y, float aspect) {
            const Pick pick = at(ui, camera, x, y, aspect, captured);
            const int over = captured >= 0 ? captured : pick.panel;
            if (hovered >= 0 && hovered != over) {
                ui.pointer(hovered, WEFT_POINTER_LEAVE, 0, 0, 0);
            }
            hovered = over;
            if (over >= 0 && pick.panel >= 0) {
                ui.pointer(over, WEFT_POINTER_MOVE, pick.x, pick.y, buttons);
            }
            return pick;
        }

        // A press: on a panel, its page gets it and keeps the pointer; false when the world gets it.
        bool press(Ui& ui, const Camera& camera, float x, float y, float aspect) {
            const Pick pick = move(ui, camera, x, y, aspect);
            if (pick.panel < 0) {
                return false;
            }
            buttons |= 1;
            captured = pick.panel;
            ui.pointer(pick.panel, WEFT_POINTER_DOWN, pick.x, pick.y, buttons);
            return true;
        }

        void release(Ui& ui, const Camera& camera, float x, float y, float aspect) {
            buttons &= ~1u;
            if (captured >= 0) {
                const Pick pick = at(ui, camera, x, y, aspect, captured);
                ui.pointer(captured, WEFT_POINTER_UP, pick.x, pick.y, buttons);
                captured = -1;
            }
            move(ui, camera, x, y, aspect);
        }
    };

    // A frame of --bench's profile: when, its CPU time, and where it went.
    struct Profiled {
        double t{};
        double cpu{};
        Phases phases;
        unsigned redraws{};
    };

    // The slowest frames of a bench and where their time went, and which phase took the most of the frames over a
    // threshold.
    void print_profile(std::vector<Profiled> frames, std::size_t panels) {
        if (frames.empty()) {
            return;
        }
        static const char* names[]{"terminal", "status", "sign", "4"};
        // The frames that got the game's status event, in order: whether they grow over the run.
        std::printf("  frames with the status event (s: CPU ms = board's advance + its encode):\n   ");
        unsigned shown{};
        for (const auto& f : frames) {
            if (f.phases.second) {
                std::printf(" %.0f: %.3f = %.3f + %.3f;%s", f.t, f.cpu, f.phases.advance[1], f.phases.encode[1],
                            ++shown % 6 ? "" : "\n   ");
            }
        }
        std::printf("\n");
        std::sort(frames.begin(), frames.end(), [](const Profiled& a, const Profiled& b) {
            return a.cpu > b.cpu;
        });
        std::printf("  slowest frames (ms): t, CPU = status event + advance per page + prepare/encode per page that "
                    "drew + pages' commit + world + commit (+ unaccounted)\n");
        for (std::size_t k = 0; k < std::min<std::size_t>(12, frames.size()); ++k) {
            const auto& f = frames[k];
            const auto& p = f.phases;
            double accounted = p.status + p.pages_commit + p.world + p.commit;
            std::string pages;
            for (std::size_t i = 0; i < panels; ++i) {
                accounted += p.advance[i] + p.prepare[i] + p.encode[i];
                char part[96];
                std::snprintf(part, sizeof part, " %s %.3f", names[i], p.advance[i]);
                pages += part;
                if (p.prepare[i] > 0 || p.encode[i] > 0) {
                    std::snprintf(part, sizeof part, " (+%.3f/%.3f)", p.prepare[i], p.encode[i]);
                    pages += part;
                }
            }
            std::printf("  %7.3f s  %6.3f = %s%.3f |%s | %.3f | %.3f | %.3f (+%.3f)\n", f.t, f.cpu,
                        p.second ? "status " : "", p.status, pages.c_str(), p.pages_commit, p.world, p.commit,
                        f.cpu - accounted);
        }
        // Over 0.5 ms: the phase that took the most of each.
        std::vector<std::pair<std::string, unsigned>> blame;
        unsigned over{};
        for (const auto& f : frames) {
            if (f.cpu < 0.5) {
                continue;
            }
            ++over;
            const auto& p = f.phases;
            std::string worst = "status event";
            double most = p.status;
            const auto consider = [&](const std::string& name, double v) {
                if (v > most) {
                    most = v;
                    worst = name;
                }
            };
            for (std::size_t i = 0; i < panels; ++i) {
                consider(std::string("advance ") + names[i], p.advance[i]);
                consider(std::string("prepare ") + names[i], p.prepare[i]);
                consider(std::string("encode ") + names[i], p.encode[i]);
            }
            consider("pages' commit", p.pages_commit);
            consider("world", p.world);
            consider("commit", p.commit);
            const auto found = std::find_if(blame.begin(), blame.end(), [&](const auto& b) {
                return b.first == worst;
            });
            if (found == blame.end()) {
                blame.emplace_back(worst, 1);
            } else {
                ++found->second;
            }
        }
        std::sort(blame.begin(), blame.end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
        std::printf("  %u of %zu frames over 0.5 ms; the phase that took the most of them:", over, frames.size());
        for (const auto& [name, count] : blame) {
            std::printf(" %s %u;", name.c_str(), count);
        }
        std::printf("\n");
    }

    // Percentiles of a series (ms).
    struct Series {
        std::vector<double> values;

        void add(double v) {
            values.push_back(v);
        }

        double at(double q) const {
            if (values.empty()) {
                return 0;
            }
            std::vector<double> sorted = values;
            std::sort(sorted.begin(), sorted.end());
            return sorted[std::min(sorted.size() - 1, std::size_t(q * double(sorted.size())))];
        }

        std::string summary() const {
            char out[160];
            std::snprintf(out, sizeof out, "p50 %.3f  p95 %.3f  p99 %.3f  max %.3f ms (%zu)", at(0.5), at(0.95),
                          at(0.99), at(1), values.size());
            return out;
        }
    };

    // Off screen: frames at 60 per second of game time, each waited for. At frame 30 the pointer clicks the
    // terminal's +10 button: the engine projects the button's point to the target and routes the pointer there
    // through the camera's ray, as a mouse click.
    int shot(const std::string& path, unsigned frames, unsigned width, unsigned height) {
        Ui ui;
        World world(ui);
        world.use_mips = mips;
        Camera camera;
        ui.status_every_frame = status_every_frame;
        if (animated_ui) {
            ui.animate_status(true);
        }
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:World::color_format
                                                                                      width:width
                                                                                     height:height
                                                                                  mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [world.device newTextureWithDescriptor:d];
        const float aspect = float(width) / float(height);
        Pointer pointer;
        int clicked_panel = -1;
        unsigned redraws{};
        unsigned busy{};
        Series ui_gpu;
        Series world_gpu;
        id<MTLBuffer> picture = nil;
        const int before = ui.output;
        std::vector<Profiled> profiled;
        Series cpu;
        for (unsigned n = 0; n < frames; ++n) {
            const double t = n / 60.0;
            if (n == 30) {
                const auto& p = ui.panels[Ui::terminal_panel];
                const float u = plus_ten_x / p.width * 2 - 1;
                const float v = 1 - plus_ten_y / p.height * 2;
                const simd_float2 at = camera.project(p.center + p.right * u + p.up * v, aspect);
                const Pick pick = Pointer::at(ui, camera, at.x, at.y, aspect, -1);
                check(pick.panel == Ui::terminal_panel && std::fabs(pick.x - plus_ten_x) < 0.5f &&
                          std::fabs(pick.y - plus_ten_y) < 0.5f,
                      "the ray meets the terminal at its +10 button");
                const auto hit = ui.hit(pick.panel, pick.x, pick.y);
                check(hit.element && std::string(hit.cursor) == "pointer", "the +10 button under the pointer");
                check(pointer.press(ui, camera, at.x, at.y, aspect), "the press goes to the terminal");
                pointer.release(ui, camera, at.x, at.y, aspect);
                clicked_panel = pick.panel;
                std::printf("clicked the terminal at (%.1f, %.1f) px from the target's (%.0f, %.0f) px\n", pick.x,
                            pick.y, at.x * float(width), at.y * float(height));
            }
            Phases phases;
            const auto begin = Clock::now();
            ui.advance(t, &phases);
            const auto report = world.frame(
                ui, camera, t, target, width, height,
                [&](id<MTLCommandBuffer> command) {
                    if (n + 1 == frames) {
                        picture = copy_out(world.device, command, target);
                    }
                },
                [&](std::uint64_t, double ms) {
                    world_gpu.add(ms);
                },
                [&](double ms) {
                    ui_gpu.add(ms);
                },
                &phases);
            const double spent = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            if (n > 0) {
                cpu.add(spent);
                profiled.push_back({t, spent, phases, report.redraws});
            }
            redraws += report.redraws;
            busy += report.busy;
            id<MTLCommandBuffer> fence = [world.queue commandBuffer];
            [fence commit];
            [fence waitUntilCompleted];
            world.complete(world.serial);
        }
        write_png(path, static_cast<const std::uint8_t*>(picture.contents), width, height);
        std::printf("%u frames at %ux%u px: pages drawn %u times (busy %u), output %d -> %d (%u messages), "
                    "diagnostics %u -> %s\n",
                    frames, width, height, redraws, busy, before, ui.output, ui.output_messages, ui.diagnostics,
                    path.c_str());
        std::printf("CPU        %s\nGPU world  %s\nGPU pages  %s\n", cpu.summary().c_str(), world_gpu.summary().c_str(),
                    ui_gpu.summary().c_str());
        if (profile) {
            print_profile(std::move(profiled), ui.panels.size());
        }
        return clicked_panel == Ui::terminal_panel && ui.output == before + 10 && ui.diagnostics == 0 ? 0 : 1;
    }
} // namespace

// The window's content: a CAMetalLayer drawn as often as the run loop turns (or the display, with vsync), the pointer
// routed to the panels or the camera.
@interface IngameView : NSView
@end

@implementation IngameView {
    std::unique_ptr<Ui> _ui;
    std::shared_ptr<World> _world;
    Camera _camera;
    Pointer _pointer;
    CAMetalLayer* _layer;
    dispatch_semaphore_t _inflight;
    Clock::time_point _start;
    Clock::time_point _second;
    NSTimer* _timer;
    bool _orbit;
    bool _dragging;
    NSPoint _last;
    bool _captured_png;
    // This second's frames and the whole run's, for the title, the log and --bench.
    unsigned _frames;
    unsigned _redraws;
    unsigned _busy;
    double _cpu_ms;
    double _gpu_ms;
    unsigned _gpu_frames;
    double _ui_ms;
    unsigned _ui_frames;
    Series _cpu_series;
    Series _gpu_series;
    Series _ui_series;
    std::vector<Profiled> _profile;
    unsigned _total_redraws;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    self.wantsLayer = YES;
    _ui = std::make_unique<Ui>();
    _world = std::make_shared<World>(*_ui);
    _world->use_mips = mips;
    if (animated_ui) {
        _ui->animate_status(true);
    }
    _orbit = bench_seconds > 0;
    _layer = [CAMetalLayer layer];
    _layer.device = _world->device;
    _layer.pixelFormat = World::color_format;
    _layer.framebufferOnly = !capture_path.empty() ? NO : YES;
    _layer.opaque = YES;
    _layer.maximumDrawableCount = 3;
    _layer.displaySyncEnabled = vsync;
    self.layer = _layer;
    _inflight = dispatch_semaphore_create(2);
    _start = _second = Clock::now();
    [self updateDrawableSize];
    [self schedule];
    return self;
}

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
    _layer.contentsScale = scale;
    _layer.drawableSize = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

- (float)aspect {
    return float(self.bounds.size.width / std::max(1.0, self.bounds.size.height));
}

- (void)tick {
    if (dispatch_semaphore_wait(_inflight, DISPATCH_TIME_NOW) != 0) {
        return;
    }
    const auto begin = Clock::now();
    const double t = std::chrono::duration<double>(begin - _start).count();
    try {
        if (_orbit) {
            _camera.yaw = float(std::sin(t * 0.25) * 0.9);
            _camera.pitch = 0.1f + 0.08f * float(std::sin(t * 0.17));
        }
        Phases phases;
        _ui->advance(t, &phases);
        const auto advanced = Clock::now();
        id<CAMetalDrawable> drawable = [_layer nextDrawable];
        if (!drawable) {
            dispatch_semaphore_signal(_inflight);
            return;
        }
        const auto acquired = Clock::now();
        dispatch_semaphore_t inflight = _inflight;
        std::weak_ptr<World> world = _world;
        __weak IngameView* view = self;
        const auto report = _world->frame(
            *_ui, _camera, t, drawable.texture, unsigned(drawable.texture.width), unsigned(drawable.texture.height),
            [&](id<MTLCommandBuffer> command) {
                if (!capture_path.empty() && !_captured_png && t >= 3) {
                    _captured_png = true;
                    id<MTLTexture> shown = drawable.texture;
                    id<MTLBuffer> into = copy_out(_world->device, command, shown);
                    const unsigned w = unsigned(shown.width);
                    const unsigned h = unsigned(shown.height);
                    [command addCompletedHandler:^(id<MTLCommandBuffer>) {
                      dispatch_async(dispatch_get_main_queue(), ^{
                        write_png(capture_path, static_cast<const std::uint8_t*>(into.contents), w, h);
                        std::printf("captured %ux%u px -> %s\n", w, h, capture_path.c_str());
                        std::fflush(stdout);
                      });
                    }];
                }
                [command presentDrawable:drawable];
            },
            [inflight, world, view](std::uint64_t serial, double ms) {
                dispatch_semaphore_signal(inflight);
                // weft's renderer is the owner thread's: the completion reaches it there.
                dispatch_async(dispatch_get_main_queue(), ^{
                  if (const auto alive = world.lock()) {
                      alive->complete(serial);
                  }
                  [view tookGpu:ms pages:false];
                });
            },
            [view](double ms) {
                dispatch_async(dispatch_get_main_queue(), ^{
                  [view tookGpu:ms pages:true];
                });
            },
            &phases);
        // The CPU's part of the frame: the pages' advance, their exports and the world's encoding, without the wait
        // for a drawable.
        const double cpu = std::chrono::duration<double, std::milli>(advanced - begin).count() +
                           std::chrono::duration<double, std::milli>(Clock::now() - acquired).count();
        _cpu_ms += cpu;
        if (bench_seconds > 0 && t > 1) {
            _cpu_series.add(cpu);
            _profile.push_back({t, cpu, phases, report.redraws});
        }
        _redraws += report.redraws;
        _busy += report.busy;
        _total_redraws += report.redraws;
    } catch (const std::exception& e) {
        dispatch_semaphore_signal(_inflight);
        std::fprintf(stderr, "frame failed: %s\n", e.what());
        [_timer invalidate];
        [NSApp terminate:nil];
        return;
    }
    ++_frames;
    const auto now = Clock::now();
    if (now - _second >= std::chrono::seconds(1)) {
        const double seconds = std::chrono::duration<double>(now - _second).count();
        const double frames = std::max(1u, _frames);
        const double cpu = _cpu_ms / frames;
        const double gpu = _gpu_frames ? _gpu_ms / _gpu_frames : 0;
        const double pages = _ui_frames ? _ui_ms / _ui_frames : 0;
        self.window.title = [NSString
            stringWithFormat:@"weft in-game UI — %.0f fps · CPU %.2f ms · GPU world %.2f ms · pages drawn %u/s (GPU "
                             @"%.2f ms each time) · mips %s · Space orbit, M mips, A animate board, V vsync",
                             _frames / seconds, cpu, gpu, _redraws, pages, _world->use_mips ? "on" : "off"];
        std::printf("%.0f fps (vsync %s), %ldx%ld px: CPU %.3f ms/frame, GPU world %.3f ms, pages drawn %u times "
                    "(GPU %.3f ms each frame that drew; busy %u), output %d\n",
                    _frames / seconds, vsync ? "on" : "off", long(_layer.drawableSize.width),
                    long(_layer.drawableSize.height), cpu, gpu, _redraws, pages, _busy, _ui->output);
        std::fflush(stdout);
        _frames = _redraws = _busy = _gpu_frames = _ui_frames = 0;
        _cpu_ms = _gpu_ms = _ui_ms = 0;
        _second = now;
    }
    const double elapsed = std::chrono::duration<double>(now - _start).count();
    if (bench_seconds > 0 && elapsed >= bench_seconds + 1) {
        std::printf("bench %.0f s at %ldx%ld px, vsync %s, animated board %s, mips %s:\n  CPU        %s\n  GPU world  "
                    "%s\n  GPU pages  %s\n  pages drawn %u times\n",
                    bench_seconds, long(_layer.drawableSize.width), long(_layer.drawableSize.height),
                    vsync ? "on" : "off", animated_ui ? "on" : "off", _world->use_mips ? "on" : "off",
                    _cpu_series.summary().c_str(), _gpu_series.summary().c_str(), _ui_series.summary().c_str(),
                    _total_redraws);
        print_profile(std::move(_profile), _ui->panels.size());
        std::fflush(stdout);
        [NSApp terminate:nil];
    } else if (quit_after > 0 && elapsed >= quit_after) {
        [NSApp terminate:nil];
    }
}

- (void)tookGpu:(double)ms pages:(bool)pages {
    const double t = std::chrono::duration<double>(Clock::now() - _start).count();
    if (pages) {
        _ui_ms += ms;
        ++_ui_frames;
        if (bench_seconds > 0 && t > 1) {
            _ui_series.add(ms);
        }
    } else {
        _gpu_ms += ms;
        ++_gpu_frames;
        if (bench_seconds > 0 && t > 1) {
            _gpu_series.add(ms);
        }
    }
}

// The pointer at a point of the view, 0 to 1 from its top left.
- (simd_float2)unitOf:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    return simd_make_float2(float(p.x / std::max(1.0, self.bounds.size.width)),
                            float(p.y / std::max(1.0, self.bounds.size.height)));
}

- (void)cursorFor:(const Pick&)pick {
    if (pick.panel >= 0) {
        const auto hit = _ui->hit(pick.panel, pick.x, pick.y);
        if (hit.element && std::string(hit.cursor) == "pointer") {
            [[NSCursor pointingHandCursor] set];
            return;
        }
    }
    [[NSCursor arrowCursor] set];
}

- (void)mouseMoved:(NSEvent*)event {
    const auto at = [self unitOf:event];
    try {
        [self cursorFor:_pointer.move(*_ui, _camera, at.x, at.y, [self aspect])];
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pointer: %s\n", e.what());
    }
}

- (void)mouseDragged:(NSEvent*)event {
    if (_dragging) {
        // The camera turns with the drag.
        _camera.yaw -= float(event.deltaX) * 0.006f;
        _camera.pitch = std::clamp(_camera.pitch + float(event.deltaY) * 0.004f, -0.05f, 1.2f);
        return;
    }
    [self mouseMoved:event];
}

- (void)mouseDown:(NSEvent*)event {
    const auto at = [self unitOf:event];
    try {
        _dragging = !_pointer.press(*_ui, _camera, at.x, at.y, [self aspect]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pointer: %s\n", e.what());
    }
}

- (void)mouseUp:(NSEvent*)event {
    const auto at = [self unitOf:event];
    _dragging = false;
    try {
        _pointer.release(*_ui, _camera, at.x, at.y, [self aspect]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pointer: %s\n", e.what());
    }
}

- (void)mouseExited:(NSEvent*)event {
    if (_pointer.hovered >= 0 && _pointer.captured < 0) {
        _ui->pointer(_pointer.hovered, WEFT_POINTER_LEAVE, 0, 0, 0);
        _pointer.hovered = -1;
    }
    [[NSCursor arrowCursor] set];
}

- (void)scrollWheel:(NSEvent*)event {
    const auto at = [self unitOf:event];
    const Pick pick = Pointer::at(*_ui, _camera, at.x, at.y, [self aspect], -1);
    if (pick.panel >= 0) {
        _ui->pointer(pick.panel, WEFT_POINTER_WHEEL, pick.x, pick.y, _pointer.buttons, float(-event.scrollingDeltaX),
                     float(-event.scrollingDeltaY));
        return;
    }
    _camera.distance = std::clamp(_camera.distance * std::exp(-float(event.scrollingDeltaY) * 0.01f), 2.5f, 16.f);
}

- (void)keyDown:(NSEvent*)event {
    NSString* key = event.charactersIgnoringModifiers.lowercaseString;
    if ([key isEqualToString:@" "]) {
        _orbit = !_orbit;
    } else if ([key isEqualToString:@"m"]) {
        _world->use_mips = !_world->use_mips;
        std::printf("mips %s\n", _world->use_mips ? "on" : "off");
    } else if ([key isEqualToString:@"a"]) {
        _ui->animate_status(!_ui->animated_status);
        std::printf("animated board %s\n", _ui->animated_status ? "on" : "off");
    } else if ([key isEqualToString:@"v"]) {
        vsync = !vsync;
        _layer.displaySyncEnabled = vsync;
        [self schedule];
        std::printf("vsync %s\n", vsync ? "on" : "off");
    } else if ([key isEqualToString:@"f"]) {
        [self.window toggleFullScreen:nil];
    } else if ([key isEqualToString:@"r"]) {
        _camera = Camera{};
    } else {
        [super keyDown:event];
    }
}

- (void)shutdown {
    [_timer invalidate];
    _timer = nil;
    // The frames in flight complete before the renderer and the Views go.
    _world.reset();
    _ui.reset();
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
    NSWindow* _window;
    IngameView* _view;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    NSMenu* bar = [NSMenu new];
    NSMenuItem* item = [NSMenuItem new];
    [bar addItem:item];
    NSMenu* menu = [NSMenu new];
    [menu addItemWithTitle:@"Quit weft in-game UI" action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = menu;
    NSApp.mainMenu = bar;

    const NSRect frame = NSMakeRect(0, 0, 1280, 720);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"weft in-game UI";
    _window.contentMinSize = NSMakeSize(640, 360);
    _window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    try {
        _view = [[IngameView alloc] initWithFrame:frame];
    } catch (const std::exception& e) {
        std::fprintf(stderr, "weft in-game failed: %s\n", e.what());
        [NSApp terminate:nil];
        return;
    }
    _window.contentView = _view;
    _window.acceptsMouseMovedEvents = YES;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];
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
        unsigned frames = 90;
        unsigned width = 1280;
        unsigned height = 720;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--vsync") {
                vsync = true;
            } else if (a == "--animated-ui") {
                animated_ui = true;
            } else if (a == "--profile") {
                profile = true;
            } else if (a == "--status-every-frame") {
                status_every_frame = true;
            } else if (a == "--no-mips") {
                mips = false;
            } else if (a == "--capture" && i + 1 < argc) {
                capture_path = argv[++i];
            } else if (a == "--quit-after" && i + 1 < argc) {
                quit_after = std::stod(argv[++i]);
            } else if (a == "--bench" && i + 1 < argc) {
                bench_seconds = std::stod(argv[++i]);
            } else if (a == "--shot" && i + 1 < argc) {
                shot_path = argv[++i];
            } else if (a == "--frames" && i + 1 < argc) {
                frames = unsigned(std::stoul(argv[++i]));
            } else if (a == "--size" && i + 1 < argc) {
                std::sscanf(argv[++i], "%ux%u", &width, &height);
            } else if (a.size() > 1 && a[0] == '-' && (std::isupper(static_cast<unsigned char>(a[1])) ||
                                                       a.rfind("-psn", 0) == 0)) {
                // Launch Services', AppKit's and Instruments' own arguments (-NSKey value, -ApplePersistence... value).
                ++i;
            } else {
                std::fprintf(stderr, "unknown argument \"%s\"\n", a.c_str());
                std::fprintf(stderr, "usage: weft_macos_ingame [--vsync] [--animated-ui] [--no-mips] [--capture file.png] "
                                     "[--quit-after S] [--bench S] | --shot file.png [--frames N] [--size WxH] [--profile] [--status-every-frame]\n");
                return 2;
            }
        }
        if (!shot_path.empty()) {
            try {
                return shot(shot_path, std::max(frames, 31u), width, height);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "weft in-game failed: %s\n", e.what());
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
