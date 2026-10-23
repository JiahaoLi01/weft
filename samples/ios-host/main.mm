// A game engine's side of weft on iOS (Objective-C++, ARC), through the public headers only (include/weft): the HUD
// page of examples/engine-host (the Next page button steps through the logo pages of examples/logo) in the screen's
// safe area, over the engine's own picture of its world, drawn by weft's Metal backend into a CAMetalLayer at the
// display's rate (up to 120 Hz on ProMotion), as samples/macos-host does in a window: units walk and the engine writes
// their positions as anchors, their health to the view model and damage to hud.damage events; the page's end-turn
// button calls the engine, which answers; the minimap is a texture the engine draws every frame. One touch is the
// pointer: it moves to the touch, goes down and up, then leaves (a tap leaves no hover); a touch the system cancels
// leaves without going up, so it clicks nothing. A touch on the world where the page has nothing to interact with
// damages the unit there. A line at the top shows the page, frame rate, frame times and the slowest frame on the
// display; stdout prints them every second, and each touch (Xcode's console, xcrun simctl launch --console, or xcrun
// devicectl device process launch --console).
//
//   weft_ios_demo [--page <page>]          hud (the default), logo (logo/color), color, dark, light or static
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include "gpu.hpp"
#include "page.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace {
    using Clock = std::chrono::steady_clock;

    // The page the app starts with: an index into demo::switchable.
    std::size_t first_page{};

    // The folder of page index in the bundle (CMakeLists.txt copies the pages to examples/).
    std::string page_folder(std::size_t index) {
        return std::string(NSBundle.mainBundle.resourcePath.UTF8String) + "/examples/" + demo::switchable[index];
    }

    // The page a --page argument names: one of the examples' names, or a folder of demo::switchable.
    std::size_t page_index(const std::string& name) {
        static constexpr std::pair<const char*, std::size_t> names[]{{"hud", 0},  {"logo", 1},  {"color", 1},
                                                                     {"dark", 2}, {"light", 3}, {"static", 4}};
        for (const auto& [known, index] : names) {
            if (name == known) {
                return index;
            }
        }
        for (std::size_t i = 0; i < std::size(demo::switchable); ++i) {
            if (name == demo::switchable[i]) {
                return i;
            }
        }
        throw demo::Failure("no page \"" + name + "\": one of hud, logo, color, dark, light, static");
    }
} // namespace

// The weft View's picture: a CAMetalLayer drawn every display frame, and the touches for the page and the world.
@interface WeftView : UIView
// The line of frame statistics once a second, or what stopped the frames.
@property(nonatomic, copy) void (^onStatus)(NSString* status, BOOL failed);
- (void)resume;
- (void)pause;
- (void)nextPage;
- (void)shutdown;
@end

@implementation WeftView {
    std::unique_ptr<demo::Engine> _engine;
    std::shared_ptr<demo::Gpu> _gpu;
    CAMetalLayer* _layer;
    CADisplayLink* _link;
    dispatch_semaphore_t _inflight;
    Clock::time_point _start;
    Clock::time_point _second;
    std::size_t _page;
    // The touch that is the pointer; others are ignored until it ends.
    UITouch* _touch;
    unsigned _frames_this_second;
    double _advance_ms;
    // This second's frame times, their sums and slowest frames: the CPU's (the View's advance, encoding and
    // committing), the wait for a drawable, and the GPU's (from Metal's completed command buffers, on the main thread).
    double _cpu_ms;
    double _cpu_max_ms;
    double _wait_ms;
    double _gpu_ms;
    double _gpu_max_ms;
    unsigned _gpu_frames;
    // This second's presentation: the longest interval between two frames reaching the display (the slowest frame as
    // seen) and the frames never shown. _presented: when the last frame shown reached the display (0: none since the
    // frames resumed).
    double _interval_max_ms;
    unsigned _dropped;
    CFTimeInterval _presented;
}

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    _gpu = std::make_shared<demo::Gpu>();
    _layer = (CAMetalLayer*)self.layer;
    _layer.device = _gpu->device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    // Backdrop filters at the drawable's level copy from it.
    _layer.framebufferOnly = NO;
    _layer.opaque = YES;
    _layer.maximumDrawableCount = 3;
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    _layer.colorspace = space;
    CGColorSpaceRelease(space);
    self.multipleTouchEnabled = NO;
    _page = first_page;
    _inflight = dispatch_semaphore_create(2);
    _start = _second = Clock::now();
    // A screen of another scale lays the View out again.
    [self registerForTraitChanges:@[ UITraitDisplayScale.class ] withAction:@selector(setNeedsLayout)];
    return self;
}

// The View takes the bounds' size in logical px (points) at the screen's scale; the first layout makes it.
- (void)layoutSubviews {
    [super layoutSubviews];
    const CGSize size = self.bounds.size;
    const CGFloat scale = self.traitCollection.displayScale;
    if (size.width <= 0 || size.height <= 0 || scale <= 0 || !_gpu) {
        return;
    }
    _layer.contentsScale = scale;
    _layer.drawableSize = CGSizeMake(std::round(size.width * scale), std::round(size.height * scale));
    try {
        if (_engine) {
            _engine->resize(float(size.width), float(size.height), float(scale));
        } else {
            _engine = std::make_unique<demo::Engine>(demo::load_page(page_folder(_page)), float(size.width),
                                                     float(size.height), float(scale));
            [self resume];
        }
    } catch (const std::exception& e) {
        [self fail:std::string("layout: ") + e.what()];
    }
}

// Frames at the display's rate while the app is in the foreground.
- (void)resume {
    if (_link || !_engine) {
        return;
    }
    _presented = 0;
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)];
    _link.preferredFrameRateRange = CAFrameRateRangeMake(60, 120, 120);
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

// No frames in the background (Metal refuses a background app's work); the frames in flight complete.
- (void)pause {
    [_link invalidate];
    _link = nil;
}

- (void)fail:(const std::string&)what {
    [self pause];
    std::fprintf(stderr, "weft demo: %s\n", what.c_str());
    if (self.onStatus) {
        self.onStatus(@(what.c_str()), YES);
    }
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
        // When the display shows the frame (0: never shown). Made here, not in the lambda below: a block made there
        // would capture the lambda's reference to view, a stack slot gone when the frame is shown.
        void (^presented)(id<MTLDrawable>) = ^(id<MTLDrawable> shown) {
          const CFTimeInterval at = shown.presentedTime;
          dispatch_async(dispatch_get_main_queue(), ^{
            [view presentedAt:at];
          });
        };
        _gpu->frame(
            *_engine, t, drawable.texture, unsigned(drawable.texture.width), unsigned(drawable.texture.height),
            [&](id<MTLCommandBuffer> command) {
                [drawable addPresentedHandler:presented];
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
        const double cpu = std::chrono::duration<double, std::milli>(Clock::now() - acquired).count() + _advance_ms;
        _cpu_ms += cpu;
        _cpu_max_ms = std::max(_cpu_max_ms, cpu);
    } catch (const std::exception& e) {
        dispatch_semaphore_signal(_inflight);
        [self fail:std::string("frame failed: ") + e.what()];
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
        const long width = long(_layer.drawableSize.width);
        const long height = long(_layer.drawableSize.height);
        if (self.onStatus) {
            self.onStatus([NSString stringWithFormat:@"%s · %.0f fps · %ld×%ld px · CPU %.2f ms · GPU %.2f ms · "
                                                     @"slowest %.1f ms",
                                                     _engine->page_name.c_str(), _frames_this_second / seconds, width,
                                                     height, cpu, gpu, _interval_max_ms],
                          NO);
        }
        std::printf("%s: %.0f fps, %ldx%ld px; per frame CPU %.2f ms (max %.2f, View %.2f), GPU %.2f ms (max %.2f), "
                    "waiting for a drawable %.2f ms; slowest frame on the display %.1f ms, %u not shown; turn %u\n",
                    _engine->page_name.c_str(), _frames_this_second / seconds, width, height, cpu, _cpu_max_ms,
                    _advance_ms, gpu, _gpu_max_ms, wait, _interval_max_ms, _dropped, _engine->turn);
        _cpu_ms = _cpu_max_ms = _wait_ms = _gpu_ms = _gpu_max_ms = _interval_max_ms = 0;
        _gpu_frames = _dropped = 0;
        std::fflush(stdout);
        _frames_this_second = 0;
        _second = now;
    }
}

- (void)frameTookGpu:(double)ms {
    _gpu_ms += ms;
    _gpu_max_ms = std::max(_gpu_max_ms, ms);
    ++_gpu_frames;
}

// A frame reached the display at time at (s), or never did (0).
- (void)presentedAt:(CFTimeInterval)at {
    if (at <= 0) {
        ++_dropped;
        return;
    }
    if (_presented > 0) {
        _interval_max_ms = std::max(_interval_max_ms, (at - _presented) * 1000);
    }
    _presented = at;
}

- (void)pointer:(weft_pointer_kind)kind at:(CGPoint)p buttons:(std::uint32_t)buttons {
    try {
        _engine->pointer(kind, float(p.x), float(p.y), buttons);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pointer: %s\n", e.what());
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_touch || !_engine) {
        return;
    }
    _touch = touches.anyObject;
    const CGPoint p = [_touch locationInView:self];
    try {
        const auto hit = _engine->hit(float(p.x), float(p.y));
        const bool page = hit.element && std::string(hit.cursor) == "pointer";
        std::printf("touch down at %.0f, %.0f: %s\n", p.x, p.y, page ? "the page's" : "the world's");
        [self pointer:WEFT_POINTER_MOVE at:p buttons:0];
        [self pointer:WEFT_POINTER_DOWN at:p buttons:1];
        if (!page) {
            // The world's: the unit under the touch takes damage.
            const double t = std::chrono::duration<double>(Clock::now() - _start).count();
            if (const int unit = _engine->unit_at(float(p.x), float(p.y), t); unit >= 0) {
                _engine->damage(unsigned(unit), 25);
                std::printf("unit %d takes 25\n", unit + 1);
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "touch: %s\n", e.what());
    }
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_touch && [touches containsObject:_touch]) {
        [self pointer:WEFT_POINTER_MOVE at:[_touch locationInView:self] buttons:1];
    }
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_touch && [touches containsObject:_touch]) {
        const CGPoint p = [_touch locationInView:self];
        std::printf("touch up at %.0f, %.0f\n", p.x, p.y);
        [self pointer:WEFT_POINTER_UP at:p buttons:0];
        [self pointer:WEFT_POINTER_LEAVE at:CGPointZero buttons:0];
        _touch = nil;
    }
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    if (_touch && [touches containsObject:_touch]) {
        std::printf("touch cancelled\n");
        [self pointer:WEFT_POINTER_LEAVE at:CGPointZero buttons:0];
        _touch = nil;
    }
}

- (void)nextPage {
    if (!_engine) {
        return;
    }
    const std::size_t next = (_page + 1) % std::size(demo::switchable);
    try {
        // The same View takes the page: its animations start over.
        _engine->load(demo::load_page(page_folder(next)));
        _page = next;
        std::printf("page %s\n", _engine->page_name.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "load %s: %s\n", demo::switchable[next], e.what());
    }
}

- (void)shutdown {
    [self pause];
    // The frames in flight complete before the renderer and the View go.
    _gpu.reset();
    _engine.reset();
}

@end

// The screen: the weft View in the safe area (clear of the sensor housing and the home indicator), the line of frame
// statistics over its top left and the Next page button over its top right.
@interface DemoController : UIViewController
@property(nonatomic, readonly) WeftView* weft;
@end

@implementation DemoController {
    UILabel* _status;
}

- (void)loadView {
    UIView* root = [UIView new];
    root.backgroundColor = UIColor.blackColor;
    self.view = root;
    UILayoutGuide* safe = root.safeAreaLayoutGuide;

    _status = [UILabel new];
    _status.translatesAutoresizingMaskIntoConstraints = NO;
    _status.font = [UIFont monospacedDigitSystemFontOfSize:11 weight:UIFontWeightMedium];
    _status.textColor = [UIColor colorWithWhite:1 alpha:0.85];
    _status.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    _status.numberOfLines = 0;
    _status.layer.cornerRadius = 4;
    _status.layer.masksToBounds = YES;
    _status.text = @" weft on Metal ";

    try {
        _weft = [[WeftView alloc] initWithFrame:CGRectZero];
    } catch (const std::exception& e) {
        std::fprintf(stderr, "weft demo failed: %s\n", e.what());
        _status.text = [NSString stringWithFormat:@" weft demo failed: %s ", e.what()];
        _status.textColor = UIColor.systemRedColor;
    }
    if (_weft) {
        _weft.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:_weft];
        [NSLayoutConstraint activateConstraints:@[
            [_weft.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
            [_weft.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
            [_weft.topAnchor constraintEqualToAnchor:safe.topAnchor],
            [_weft.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        ]];
        UILabel* status = _status;
        _weft.onStatus = ^(NSString* text, BOOL failed) {
          status.text = [NSString stringWithFormat:@" %@ ", text];
          if (failed) {
              status.textColor = UIColor.systemRedColor;
          }
        };

        // As the line of statistics: legible over the light and the dark pages alike.
        UIButtonConfiguration* look = [UIButtonConfiguration filledButtonConfiguration];
        look.title = @"Next page";
        look.buttonSize = UIButtonConfigurationSizeSmall;
        look.baseBackgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
        look.baseForegroundColor = [UIColor colorWithWhite:1 alpha:0.85];
        __weak WeftView* weft = _weft;
        UIButton* next = [UIButton buttonWithConfiguration:look
                                             primaryAction:[UIAction actionWithHandler:^(UIAction*) {
                                               [weft nextPage];
                                             }]];
        next.translatesAutoresizingMaskIntoConstraints = NO;
        [root addSubview:next];
        [NSLayoutConstraint activateConstraints:@[
            [next.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
            [next.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        ]];
    }
    [root addSubview:_status];
    [NSLayoutConstraint activateConstraints:@[
        [_status.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:8],
        [_status.topAnchor constraintEqualToAnchor:safe.topAnchor constant:8],
        [_status.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-120],
    ]];
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}

// The pages are laid out for a landscape screen (examples/engine-host is a game's HUD): a phone turns sideways, an
// iPad takes any orientation.
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return self.traitCollection.userInterfaceIdiom == UIUserInterfaceIdiomPad ? UIInterfaceOrientationMaskAll
                                                                              : UIInterfaceOrientationMaskLandscape;
}

@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation SceneDelegate {
    DemoController* _controller;
}

- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    _controller = [DemoController new];
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = _controller;
    [self.window makeKeyAndVisible];
}

- (void)sceneWillEnterForeground:(UIScene*)scene {
    [_controller.weft resume];
}

- (void)sceneDidEnterBackground:(UIScene*)scene {
    [_controller.weft pause];
}

- (void)sceneDidDisconnect:(UIScene*)scene {
    [_controller.weft shutdown];
}

@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    return YES;
}

@end

int main(int argc, char** argv) {
    // Each line as it is printed: a console attached over USB shows the touches when they happen.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    @autoreleasepool {
        for (int i = 1; i < argc; ++i) {
            // Other arguments are the system's and Xcode's (-NS..., -Apple...).
            if (std::string(argv[i]) == "--page" && i + 1 < argc) {
                try {
                    first_page = page_index(argv[++i]);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "weft demo: %s\n", e.what());
                    return 2;
                }
            }
        }
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
