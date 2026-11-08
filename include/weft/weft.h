#ifndef WEFT_H
#define WEFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental M0 ABI. Every runtime function requires its owner thread unless
 * explicitly stated. A runtime pointer is invalid after successful destroy.
 * Strings are UTF-8 with an explicit size and are borrowed only during the call. */
#define WEFT_ABI_VERSION 1u

typedef struct weft_runtime weft_runtime;

typedef enum weft_result {
    WEFT_OK = 0,
    WEFT_INVALID_ARGUMENT = 1,
    WEFT_UNSUPPORTED_ABI = 2,
    WEFT_OUT_OF_MEMORY = 3,
    WEFT_WRONG_THREAD = 4,
    WEFT_BUSY = 5,
    WEFT_STALE_HANDLE = 6,
    WEFT_CAPACITY_EXCEEDED = 7,
    WEFT_TIME_REGRESSION = 8,
    WEFT_INTERNAL_ERROR = 9,
    /* The page failed: a script threw, or its stylesheet or keyframes are outside the scene profile; its diagnostics
     * say what. */
    WEFT_SCRIPT_ERROR = 10,
    /* A resource limit ended the View's page: load a page again or destroy the View. */
    WEFT_FAULTED = 11
} weft_result;

/* Runtime flags: the platform's installed fonts (Windows, Android, macOS, iOS) with its generic families and
 * fallbacks. */
#define WEFT_RUNTIME_SYSTEM_FONTS 1u

typedef struct weft_runtime_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t max_views;
    uint32_t flags; /* WEFT_RUNTIME_* */
} weft_runtime_desc;

typedef struct weft_view_handle {
    uint64_t runtime_id;
    uint32_t slot;
    uint32_t generation;
} weft_view_handle;

typedef struct weft_view_desc {
    uint32_t struct_size;
    uint32_t reserved;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved2;
} weft_view_desc;

typedef struct weft_runtime_info {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t runtime_id;
    uint32_t live_views;
    uint32_t max_views;
} weft_runtime_info;

typedef struct weft_view_info {
    uint32_t struct_size;
    uint32_t reserved;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved2;
    uint64_t tick_count;
    double last_time_seconds;
} weft_view_info;

/* NULL desc selects defaults. Output handles are cleared on failure. */
weft_result weft_runtime_create(const weft_runtime_desc* desc, weft_runtime** out_runtime);
/* WEFT_BUSY leaves the runtime intact while live Views remain. */
weft_result weft_runtime_destroy(weft_runtime* runtime);
weft_result weft_runtime_get_info(weft_runtime* runtime, weft_runtime_info* out_info);
weft_result weft_view_create(weft_runtime* runtime, const weft_view_desc* desc, weft_view_handle* out_view);
weft_result weft_view_destroy(weft_runtime* runtime, weft_view_handle view);
weft_result weft_view_get_info(weft_runtime* runtime, weft_view_handle view, weft_view_info* out_info);
/* Validates and stores host monotonic time without running the page (weft_view_advance runs it). */
weft_result weft_view_record_tick(weft_runtime* runtime, weft_view_handle view, double time_seconds);
/* Stateless; callable from any thread. Borrowed static text, do not free. */
const char* weft_result_string(weft_result result);

/* ---- Fonts. Owner thread, before the runtime's first page loads (WEFT_BUSY after). ---- */

/* A face of a font file: CSS matches its family (ASCII case-insensitive), weight (1-1000) and style. unicode_range,
 * when given, is the code points it supplies as CSS writes them ("U+0030-0039, U+3040-309F, U+4??"): a family whose
 * faces have ranges is a composite font, and a character outside a face's range falls to the next face or family,
 * as with @font-face's unicode-range. A struct_size of offsetof(weft_font_face, unicode_range) (the earlier ABI) is a
 * face without one. */
typedef struct weft_font_face {
    uint32_t struct_size;
    uint32_t weight;
    uint32_t italic;     /* 0 or 1 */
    uint32_t face_index; /* in a font collection */
    const char* family;
    size_t family_size;
    const char* path; /* the file */
    size_t path_size;
    const char* unicode_range; /* NULL: every code point */
    size_t unicode_range_size;
} weft_font_face;

weft_result weft_runtime_add_font(weft_runtime* runtime, const weft_font_face* face);
/* The family a CSS generic keyword (serif, sans-serif, monospace, system-ui, cursive, fantasy) names. */
weft_result weft_runtime_set_generic_family(weft_runtime* runtime, const char* generic, size_t generic_size,
                                            const char* family, size_t family_size);
/* Appends a family tried for characters no family of an element's font-family list covers. */
weft_result weft_runtime_add_fallback_family(weft_runtime* runtime, const char* family, size_t family_size);

/* ---- UI shader effects (docs/architecture.md, "UI 着色器效果"). Owner thread. ---- */

/* The type of an effect parameter, and so the CSS value of the custom property --<name> that sets it: a number; two,
 * three or four numbers separated by spaces; a color. */
#define WEFT_EFFECT_NUMBER 1u
#define WEFT_EFFECT_VEC2 2u
#define WEFT_EFFECT_VEC3 3u
#define WEFT_EFFECT_VEC4 4u
#define WEFT_EFFECT_COLOR 5u

typedef struct weft_effect_parameter {
    const char* name; /* without the leading "--" */
    size_t name_size;
    const char* initial; /* its value as CSS writes it; NULL or "": zeros */
    size_t initial_size;
    uint32_t type; /* WEFT_EFFECT_NUMBER .. WEFT_EFFECT_COLOR */
    uint32_t reserved;
} weft_effect_parameter;

/* What an effect texture's bytes are: one byte a texel, which the function reads as data in r
 * (WEFT_EFFECT_TEXTURE_DATA), or straight sRGB RGBA8 texels, which it reads as linear premultiplied color
 * (WEFT_EFFECT_TEXTURE_SRGB), width x height of them, rows top-down. With WEFT_EFFECT_TEXTURE_PNG the bytes are a PNG
 * file of the image instead (its red channel as data), and width and height 0. */
#define WEFT_EFFECT_TEXTURE_DATA 1u
#define WEFT_EFFECT_TEXTURE_SRGB 2u
#define WEFT_EFFECT_TEXTURE_PNG 0x100u

typedef struct weft_effect_texture {
    const char* name;
    size_t name_size;
    const void* bytes;
    size_t size;
    uint32_t format; /* WEFT_EFFECT_TEXTURE_DATA or _SRGB, with WEFT_EFFECT_TEXTURE_PNG */
    uint32_t width;
    uint32_t height;
    uint32_t reserved;
} weft_effect_texture;

/* A UI shader effect, which pages show with background-image: -weft-effect(name): its HLSL pixel function compiled with
 * weft's shell (cmake/compile_effect.cmake) to DXBC for D3D11 and D3D12 and to SPIR-V for Vulkan, each for the scene's
 * encoded and the document's linear composition space (a backend without its bytecode cannot draw frames that show the
 * effect); its parameters and textures in the order the function reads them (at most 16 and 4); and whether it reads
 * the time, so that frames keep coming while it shows. With backdrop 1 the function also reads what lies behind its
 * element (docs/effects.md): the scene copies what its target holds before the element draws, over the element's box
 * and backdrop_margin CSS px past it on each side (0 to 256), for reads that leave the box. A struct_size of
 * offsetof(weft_effect_desc, backdrop) (the earlier ABI) is an effect that reads no backdrop. */
typedef struct weft_effect_desc {
    uint32_t struct_size;
    uint32_t animated; /* 0 or 1 */
    const char* name;
    size_t name_size;
    const weft_effect_parameter* parameters;
    const weft_effect_texture* textures;
    uint32_t parameter_count;
    uint32_t texture_count;
    const void* dxbc_encoded;
    size_t dxbc_encoded_size;
    const void* dxbc_linear;
    size_t dxbc_linear_size;
    const void* spirv_encoded;
    size_t spirv_encoded_size;
    const void* spirv_linear;
    size_t spirv_linear_size;
    uint32_t backdrop; /* 0 or 1 */
    float backdrop_margin;
} weft_effect_desc;

/* Registers an effect for every View's pages, its bytes copied. WEFT_INVALID_ARGUMENT for a name registered already, a
 * manifest past its bounds, an initial value not of its parameter's type, or texture bytes that are no image of their
 * format. */
weft_result weft_runtime_add_effect(weft_runtime* runtime, const weft_effect_desc* effect);

/* ---- Pages. Owner thread. The owner thread needs at least 8 MiB of stack for React and D3 pages. ---- */

typedef struct weft_script {
    const char* name; /* for stack traces */
    size_t name_size;
    const char* source;
    size_t source_size;
} weft_script;

/* CRPG Framework's window.ue.sendEvent, __ue_dispatch and __vm_update over weft.engine. */
#define WEFT_PAGE_CRPG_BRIDGE 1u
/* The page's stylesheet and keyframes taken as browsers take CSS: cascade layers, @media, @supports, @property and
 * @keyframes in the stylesheet, CSS nesting, the selectors of Selectors 4 (:is, :where, :not, the structural and
 * sibling selectors, ::before and ::after), logical properties, every <color> and calc(); and what the scene cannot take
 * (a declaration, a selector, an at-rule) dropped with a scene diagnostic, as CSS drops what it does not parse, instead
 * of failing the page or its frame. Pages a build tool generates (Tailwind and the like) need it. */
#define WEFT_PAGE_FORGIVING_CSS 2u
/* The web platform pages written for browsers expect: the console as diagnostics (WEFT_DIAGNOSTIC_CONSOLE*),
 * localStorage (the View's, kept across loads for the host to save: weft_view_get_storage) and sessionStorage,
 * element.dataset, document.head and title, KeyboardEvent and FocusEvent, focus() and blur() with
 * document.activeElement and :focus, matchMedia answered as the stylesheet's @media, IntersectionObserver,
 * requestIdleCallback, navigator and location, toLocaleString with options. */
#define WEFT_PAGE_WEB_PLATFORM 4u
/* Blink's spatial navigation (Chrome's --enable-spatial-navigation): an arrow key moves the focus to the nearest
 * focusable element in its direction that shows (a scroll container scrolls a line when it has none; a text field's
 * caret moves first), Escape blurs it; the pointer follows the focus (hover). Elements meant to respond to the user (a
 * script listener for click, keys or the mouse coming over, cursor: pointer set on the element) take the focus too,
 * Tab's as well; Enter makes the focused element :active while it is down, and Enter's or Space's keypress clicks it
 * unless it takes the key itself. A gamepad's D-pad and left stick, its A and B buttons are the arrow keys, Enter and
 * Escape while the View has the host's focus (weft_view_gamepad). */
#define WEFT_PAGE_SPATIAL_NAVIGATION 8u

/* A page: scripts evaluated in order after the platform modules (DOM host, animation frames, the scene platform,
 * weft.engine), the scene's stylesheet and keyframes, the selector of the scene's root ("#id" or a tag name; NULL
 * is "#root", the element the document's body starts with), and the base the page's relative URLs resolve against:
 * where its stylesheet lies, as the host's resource reader names files ("UI/City/page.css" or "UI/City/"; what follows
 * its last '/' is no part of it; NULL is ""). A struct_size of offsetof(weft_page_desc, base) (the earlier ABI) is a
 * page without one. */
typedef struct weft_page_desc {
    uint32_t struct_size;
    uint32_t flags; /* WEFT_PAGE_* */
    const weft_script* scripts;
    uint32_t script_count;
    uint32_t reserved;
    const char* stylesheet;
    size_t stylesheet_size;
    const char* keyframes;
    size_t keyframes_size;
    const char* root;
    size_t root_size;
    const char* base;
    size_t base_size;
} weft_page_desc;

/* Replaces the View's page. WEFT_SCRIPT_ERROR leaves an empty View and the error a diagnostic: a script's, or a scene
 * diagnostic for a stylesheet or keyframes outside the profile (no stylesheet is an invalid argument). The View's
 * engine functions stay; its pending engine inputs and the page's messages are dropped, its open calls closed. */
weft_result weft_view_load(weft_runtime* runtime, weft_view_handle view, const weft_page_desc* page);
/* Runs the page up to time_seconds (monotonic with weft_view_record_tick): engine events, completions and view
 * models, tasks, animation frames, observers and the scene; publishes a frame when what it shows changed. Exceptions
 * of the page are diagnostics, not errors. */
weft_result weft_view_advance(weft_runtime* runtime, weft_view_handle view, double time_seconds);
weft_result weft_view_resize(weft_runtime* runtime, weft_view_handle view, float logical_width, float logical_height,
                             float device_scale);
/* The View's safe area in its logical px: how far in from the top, right, bottom and left edges the host's display
 * keeps content clear (a sensor housing, rounded corners, a home indicator), which pages read as CSS
 * env(safe-area-inset-top), -right, -bottom and -left. 0 on every side until the host sets it, as for a View the host
 * already keeps inside the safe area; kept across page loads. The next advance lays out the values that read it
 * again. Each inset finite and at least 0 (WEFT_INVALID_ARGUMENT otherwise). Owner thread. */
weft_result weft_view_set_safe_area(weft_runtime* runtime, weft_view_handle view, float top, float right, float bottom,
                                    float left);

/* ---- Pointer input and hit queries. Owner thread; positions in the View's logical px. ---- */

typedef enum weft_pointer_kind {
    WEFT_POINTER_MOVE = 0,
    WEFT_POINTER_DOWN = 1,
    WEFT_POINTER_UP = 2,
    WEFT_POINTER_WHEEL = 3,
    WEFT_POINTER_LEAVE = 4 /* the pointer left the View */
} weft_pointer_kind;

typedef struct weft_pointer_event {
    uint32_t struct_size;
    uint32_t kind; /* weft_pointer_kind */
    float x;
    float y;
    uint32_t buttons;   /* MouseEvent.buttons after the event */
    uint32_t modifiers; /* WEFT_MODIFIER_* held (MouseEvent.shiftKey and the like; Shift extends a text field's
                           selection to a press) */
    float wheel_x;      /* logical px */
    float wheel_y;
} weft_pointer_event;

/* Dispatches the pointer and mouse events of the input (over, out, enter and leave as the hovered element changes,
 * click when a button comes up over the element it went down on; detail counts the presses that come within Windows'
 * double-click time and area, and a second click is followed by dblclick). A press in a text field places its caret
 * (two select a word, three all), and moving with the button down selects. out_prevented, when given: whether a
 * listener prevented the main event's default action. */
weft_result weft_view_pointer(weft_runtime* runtime, weft_view_handle view, const weft_pointer_event* event,
                              uint32_t* out_prevented);

/* ---- Input devices. Owner thread. ---- */

#define WEFT_INPUT_MOUSE 1u /* a fine pointer that hovers */
#define WEFT_INPUT_TOUCH 2u /* a touch screen: a coarse pointer that does not hover */

typedef struct weft_input_devices {
    uint32_t struct_size;
    uint32_t primary;          /* WEFT_INPUT_MOUSE, WEFT_INPUT_TOUCH, or 0: none (a gamepad's) */
    uint32_t available;        /* WEFT_INPUT_* of every device there is; it includes primary */
    uint32_t max_touch_points; /* navigator.maxTouchPoints: 1 to 32 with WEFT_INPUT_TOUCH, 0 without */
} weft_input_devices;

/* The input devices the page sees, as Chrome on such a device: the media features pointer and hover (@media and
 * matchMedia) answer from primary (a mouse: fine, hover; a touch screen: coarse, none; none: none), any-pointer and
 * any-hover from available, navigator.maxTouchPoints is max_touch_points, and ontouchstart and the like exist with a
 * touch screen. A mouse until set; kept across page loads. matchMedia lists whose outcome changes get a change event,
 * and the next weft_view_advance restyles where an @media outcome changed. WEFT_INVALID_ARGUMENT for other device bits,
 * a primary device not available, or touch points that do not go with the touch screen. */
weft_result weft_view_set_input_devices(weft_runtime* runtime, weft_view_handle view,
                                        const weft_input_devices* devices);

/* ---- Touch input. Owner thread; positions in the View's logical px. ---- */

#define WEFT_TOUCH_BEGAN 0u
#define WEFT_TOUCH_MOVED 1u
#define WEFT_TOUCH_ENDED 2u
#define WEFT_TOUCH_CANCELLED 3u /* the system took the touch: no tap, no fling */

typedef struct weft_touch_point {
    uint64_t id;    /* the host's name of the touch from its start to its end (a UITouch, an Android pointer id) */
    uint32_t phase; /* WEFT_TOUCH_* */
    uint32_t reserved;
    float x;
    float y;
    float radius_x; /* the contact's radii in logical px; 0 when unknown */
    float radius_y;
    float force; /* 0 to 1; 0 when unknown */
    float reserved2;
} weft_touch_point;

typedef struct weft_touch_event {
    uint32_t struct_size;
    uint32_t modifiers;             /* WEFT_MODIFIER_* held */
    const weft_touch_point* points; /* the touches that changed, each once */
    uint32_t count;
    uint32_t reserved;
    /* When they changed, in seconds on weft_view_advance's clock (a touch's own timestamp: the release speed of a pan
       is measured with it); 0 is the View's time. */
    double time_seconds;
} weft_touch_event;

/* Touch input, as Chrome takes a touch screen's (docs/engine-communication.md, "Touch"): each touch is a pointer
 * (pointerType "touch", captured by the element it went down on) and touch events (touchstart; touchmove once it moved
 * past the 15 px slop; touchend; touchcancel), cancelable until a touchmove goes unprevented. A touch that stays
 * within the slop is a tap: compatibility mouse events and a click at where it went down (a second tap: dblclick), the
 * element hovered after; touchstart or touchend prevented, none. One that moves past it pans the nearest scroll
 * container that can scroll that way, as touch-action allows, and flings on after it lifts (weft_view_advance runs the
 * fling); its touchmove prevented, the page drags instead. out_prevented, when given: whether a listener prevented a
 * touch event. WEFT_INVALID_ARGUMENT for a touch that begins twice or changes before it began. */
weft_result weft_view_touch(weft_runtime* runtime, weft_view_handle view, const weft_touch_event* event,
                            uint32_t* out_prevented);

/* What is under a point: whether the topmost element taking pointer events is something other than the scene's
 * root (and its ancestors), and that element's cursor (of it or its nearest ancestor that declares one). A host can
 * give a click to the page when element is 1 and the cursor shows something to interact with, and to the game
 * otherwise. cursor holds the CSS keyword ("auto" when none), NUL-terminated and cut to fit. */
typedef struct weft_hit {
    uint32_t struct_size;
    uint32_t element;
    char cursor[32];
} weft_hit;

weft_result weft_view_hit_test(weft_runtime* runtime, weft_view_handle view, float x, float y, weft_hit* out_hit);

/* ---- Keyboard input (docs/engine-communication.md, "键盘与焦点"). Owner thread unless stated. ---- */

/* The modifiers held while a key event happens (KeyboardEvent.getModifierState): a modifier key's own keydown has its
 * flag, its keyup not. */
#define WEFT_MODIFIER_SHIFT 1u
#define WEFT_MODIFIER_CONTROL 2u
#define WEFT_MODIFIER_ALT 4u
#define WEFT_MODIFIER_META 8u
#define WEFT_MODIFIER_ALT_GRAPH 16u
#define WEFT_MODIFIER_CAPS_LOCK 32u
#define WEFT_MODIFIER_NUM_LOCK 64u

typedef enum weft_key_kind {
    WEFT_KEY_DOWN = 0,
    WEFT_KEY_UP = 1
} weft_key_kind;

/* weft_key_event::flags: the key repeats (the system's auto-repeat; keydown only); an input method took the key (its
 * keydown is key "Process" with keyCode 229 and has no default action, as Chrome gives a key Windows reports as
 * VK_PROCESSKEY). */
#define WEFT_KEY_REPEAT 1u
#define WEFT_KEY_IME 2u

/* A key going down or up. key and code are KeyboardEvent's values in UTF-8 (key 1 to 64 bytes, code at most 64): key is
 * what the key types with the modifiers ("a", "A", "!", " ") or its name ("Enter", "ArrowLeft", "Shift", "F5", "Dead";
 * weft_keyboard_key gives it for a physical key), code names the physical key ("KeyA", "Digit1", "Space",
 * "ShiftLeft", "Numpad1"), and KeyboardEvent.location follows from code. key_code is KeyboardEvent.keyCode of keydown
 * and keyup, the Windows virtual key; 0 takes the one the system gives the key: a numeric keypad key's with Num Lock
 * off is what it is then (Numpad1 is End, 35), and Windows maps a key that types a character by the foreground window's
 * layout, as the keys Chrome is given (other platforms take a US keyboard's). */
typedef struct weft_key_event {
    uint32_t struct_size;
    uint32_t kind; /* weft_key_kind */
    const char* key;
    size_t key_size;
    const char* code;
    size_t code_size;
    uint32_t modifiers; /* WEFT_MODIFIER_* */
    uint32_t flags;     /* WEFT_KEY_* */
    uint32_t key_code;
    uint32_t reserved;
} weft_key_event;

/* What became of an input: a listener prevented its default action (WEFT_INPUT_PREVENTED); weft performed a default
 * action, or began one a later event finishes (WEFT_INPUT_DEFAULT: the focus moved, a box scrolled, an element was
 * clicked or a button became :active). A host gives the game a key with neither, as Chrome gives the browser the keys a
 * page did not use. */
#define WEFT_INPUT_PREVENTED 1u
#define WEFT_INPUT_DEFAULT 2u

/* Dispatches keydown or keyup at the focused element (the body when no element has the focus) with Chrome's default
 * actions: Tab and Shift+Tab move the focus in sequential order (from the last element round to the first, as in an
 * embedded Chromium), Enter and Space activate buttons and links, the arrow keys, Page Up, Page Down, Home and End
 * scroll the scroll container of the focused element (else of the element the mouse went down on). A keydown of Enter
 * nobody prevented dispatches Enter's keypress as well. out_handled, when given: WEFT_INPUT_* bits. */
weft_result weft_view_key(weft_runtime* runtime, weft_view_handle view, const weft_key_event* event,
                          uint32_t* out_handled);

/* Text a key typed (UTF-8, at most 4096 bytes) after its keydown, as Windows' WM_CHAR, Slate's OnKeyChar and Unity's
 * inputString give it: a keypress for each character (Space's scrolls a page outside a button). Characters below U+0020
 * and U+007F are ignored: Enter, Tab and Backspace act from their keydown. After a keydown the page prevented or a
 * default action used, the text is dropped (out_handled WEFT_INPUT_DEFAULT), as Chrome drops the characters that follow
 * such a key. */
weft_result weft_view_text(weft_runtime* runtime, weft_view_handle view, const char* text, size_t text_size,
                           uint32_t* out_handled);

/* The host's keyboard focus came to the View (focused 1) or left it (0), as a browser window gains and loses it: the
 * focused element gets blur and focusout, then window blur (window focus, then focus and focusin when it comes back);
 * :focus, :focus-visible and :focus-within match only while the View has it, and document.hasFocus() says whether it
 * has. A View starts with it and keeps it across page loads. */
weft_result weft_view_set_focus(weft_runtime* runtime, weft_view_handle view, uint32_t focused);

/* Any thread: the KeyboardEvent.key the system's keyboard layout gives the physical key `code` with the modifiers
 * (WEFT_MODIFIER_SHIFT, _ALT_GRAPH, _CAPS_LOCK and _NUM_LOCK count): the character it types ("a", "A", "1", "!", "é"),
 * "Dead" for a dead key, its name for a key that types nothing ("Enter", "ArrowLeft", "F5"), "Unidentified" when the
 * layout types nothing with it. Windows reads the layout of the foreground window's thread without changing its
 * dead-key state, as Chromium's PlatformKeyMap does; other platforms answer for a US keyboard. out_key receives it
 * NUL-terminated (at most 31 bytes); WEFT_INVALID_ARGUMENT for a code weft does not know. */
weft_result weft_keyboard_key(const char* code, size_t code_size, uint32_t modifiers, char out_key[32]);

/* Any thread: the code of the physical key the system's keyboard layout puts a Windows virtual key on, for hosts that
 * have virtual keys and no scan codes (Unreal's FKeyEvent): a key that types a character wherever the layout has it
 * (Windows maps the virtual key back to its scan code: AZERTY's VK_A is "KeyQ"), the sided modifiers on their side
 * (VK_LSHIFT "ShiftLeft"), the other keys by a US keyboard's virtual keys (VK_RETURN "Enter", VK_NUMPAD1 "Numpad1"; with
 * Num Lock off a keypad key comes as the key it acts as, VK_END "End", where Chrome says "Numpad1"). Other platforms
 * answer for a US keyboard. out_code receives it NUL-terminated (at most 31 bytes); WEFT_INVALID_ARGUMENT for a
 * virtual key that is no key weft knows. */
weft_result weft_keyboard_code(uint32_t virtual_key, char out_code[32]);

/* ---- Text fields and input methods (docs/engine-communication.md, "文本框与输入法"). Owner thread. ---- */

/* Where text input goes (weft_view_text_input). active: a text field the user may edit (an input of a text type or a
 * textarea, neither read-only nor disabled) has the focus while the View has the host's; a host turns its input method
 * on while it is 1, and off when it goes to 0. composing: an input method's composition goes on in the field (when the
 * page ends it, by script or by moving the focus, a host whose input method still composes cancels it). password: a
 * password field, where Chrome keeps input methods off. caret: the field's caret (in a composition, its caret in it) in
 * the View's logical px, where a host places the input method's candidate window (below it); field: the field's border
 * box. */
typedef struct weft_text_input {
    uint32_t struct_size;
    uint32_t active;
    uint32_t composing;
    uint32_t password;
    float caret_x;
    float caret_y;
    float caret_width;
    float caret_height;
    float field_x;
    float field_y;
    float field_width;
    float field_height;
} weft_text_input;

weft_result weft_view_text_input(weft_runtime* runtime, weft_view_handle view, weft_text_input* out_input);

/* An input method's composition at the focused text field (Windows' GCS_COMPSTR with its cursor, Unity's
 * Input.compositionString, Slate's text input method context): text (UTF-8, at most 4096 bytes) replaces the
 * composition (or, as one starts, the selection) with the events Chrome gives (compositionstart, compositionupdate,
 * beforeinput and input of insertCompositionText), its selection [selection_start, selection_end) in UTF-8 offsets of
 * text (the caret when they are equal); the composition shows underlined. Empty text cancels the composition
 * (compositionend). out_taken, when given: 1 when a text field the user may edit took it. */
weft_result weft_view_composition(weft_runtime* runtime, weft_view_handle view, const char* text, size_t text_size,
                                  uint32_t selection_start, uint32_t selection_end, uint32_t* out_taken);

/* An input method commits text (UTF-8, at most 4096 bytes; Windows' GCS_RESULTSTR): the composition ends with it
 * (compositionupdate, beforeinput, input, compositionend); without one the text goes in as typed text does (beforeinput
 * and input of insertText, without keys). The keys an input method takes reach the page as WEFT_KEY_IME keys.
 * out_taken as for weft_view_composition. */
weft_result weft_view_commit_text(weft_runtime* runtime, weft_view_handle view, const char* text, size_t text_size,
                                  uint32_t* out_taken);

/* ---- Gamepads (docs/engine-communication.md, "手柄与空间导航"). Owner thread. ---- */

#define WEFT_GAMEPAD_SLOTS 4u
#define WEFT_GAMEPAD_BUTTONS 17u
#define WEFT_GAMEPAD_AXES 4u

/* A gamepad's state in the Gamepad API's standard mapping. index: its slot, 0 to WEFT_GAMEPAD_SLOTS - 1, which
 * navigator.getGamepads() shows it in. connected: 0 as it goes. id: Gamepad.id (UTF-8, at most 128 bytes; Chrome's for
 * an XInput pad is "Xbox 360 Controller (XInput STANDARD GAMEPAD)"). buttons: the standard buttons' values, 0 to 1 (A,
 * B, X, Y, the bumpers, the triggers, back, start, the stick presses, the D-pad up, down, left, right, the guide
 * button); pressed and touched: bit i for button i (Chrome presses an XInput trigger from 30/255; a button pressed or
 * above 0 is touched without its bit, as Blink's). axes: the left and
 * the right stick, x then y, -1 to 1, y down. */
typedef struct weft_gamepad_state {
    uint32_t struct_size;
    uint32_t index;
    uint32_t connected;
    uint32_t pressed;
    uint32_t touched;
    uint32_t reserved;
    const char* id;
    size_t id_size;
    float buttons[WEFT_GAMEPAD_BUTTONS];
    float axes[WEFT_GAMEPAD_AXES];
} weft_gamepad_state;

/* A gamepad's state: each frame while it is connected, and once with connected 0 as it goes. The page reads it through
 * navigator.getGamepads(), as Chrome's pages do: no gamepad shows until one has a button pressed or an axis past half,
 * then gamepadconnected fires for each, and gamepaddisconnected for one that goes. On a page with
 * WEFT_PAGE_SPATIAL_NAVIGATION, while the View has the host's focus, the D-pad and the left stick (past half) are the
 * arrow keys, A Enter and B Escape (keydown as it goes down, repeats of a held direction after 0.5 s every 0.1 s,
 * keyup as it comes up or the gamepad goes); out_handled, when given, has their WEFT_INPUT_* bits, as weft_view_key's:
 * the host keeps from the game what the page used. */
weft_result weft_view_gamepad(weft_runtime* runtime, weft_view_handle view, const weft_gamepad_state* state,
                              uint32_t* out_handled);

/* ---- Tools. Owner thread. ---- */

/* A text a tool asked for, valid during the callback; ok is 0 when it is an error's. */
typedef void (*weft_text_fn)(void* user, uint32_t ok, const char* text, size_t text_size);

/* Evaluates source in the page as a classic script, for tools such as a debug console: fn gets String() of the
 * completion value, or (ok 0) of what the script threw, after the microtasks it queued ran. The page's DOM changes reach
 * its scene at the next advance. WEFT_BUSY without a loaded page. */
weft_result weft_view_evaluate(weft_runtime* runtime, weft_view_handle view, const char* source, size_t source_size,
                               weft_text_fn fn, void* user);

/* ---- Frames. Any thread. ---- */

/* A frame the View published: immutable, reference-counted; it outlives the View while held. */
typedef struct weft_view_frame weft_view_frame;

typedef struct weft_view_frame_info {
    uint32_t struct_size;
    uint32_t animated;
    uint64_t revision; /* frames of one nonzero revision draw alike */
    double time_seconds;
    float logical_width;
    float logical_height;
    float device_scale;
    uint32_t reserved;
} weft_view_frame_info;

/* The frame published last; WEFT_BUSY before the first. Release it with weft_view_frame_release. */
weft_result weft_view_acquire_frame(weft_runtime* runtime, weft_view_handle view, weft_view_frame** out_frame);
void weft_view_frame_release(weft_view_frame* frame);
weft_result weft_view_frame_get_info(const weft_view_frame* frame, weft_view_frame_info* out_info);

/* ---- Anchors (docs/engine-communication.md section 5). Any thread. ---- */

#define WEFT_ANCHOR_HIDDEN 1u
#define WEFT_ANCHOR_BEHIND 2u
#define WEFT_ANCHOR_OFFSCREEN 4u

/* Where the element with data-anchor="key" goes: x, y in the View's logical px, scaled by scale, ordered by depth
 * (far to near among siblings); flags 0 is visible. */
typedef struct weft_anchor {
    uint64_t key;
    float x;
    float y;
    float scale;
    float depth;
    uint32_t flags;
    uint32_t reserved;
} weft_anchor;

/* Replaces all of the View's anchors; renderers draw with the latest table, the owner hits with it. */
weft_result weft_view_write_anchors(weft_runtime* runtime, weft_view_handle view, const weft_anchor* anchors,
                                    size_t count);

/* ---- The engine bridge (docs/engine-communication.md section 6). ---- */

typedef enum weft_value_format {
    WEFT_VALUE_MSGPACK = 1,
    WEFT_VALUE_JSON = 2
} weft_value_format;

/* weft_view_emit: replace the value of an undelivered event of the name instead of queueing another. */
#define WEFT_EMIT_LATEST 1u

/* Any thread; the payload is copied and delivered at the View's next advance. WEFT_CAPACITY_EXCEEDED when the
 * View's queue is full (nothing is taken). */
weft_result weft_view_emit(weft_runtime* runtime, weft_view_handle view, const char* name, size_t name_size,
                           uint32_t format, const void* payload, size_t payload_size, uint32_t flags);
/* Any thread: settles the page's call (resolves it, or rejects it with the value). WEFT_STALE_HANDLE for a call that
 * is not open (completed, timed out, from an earlier page, or never made). */
weft_result weft_view_complete_call(weft_runtime* runtime, weft_view_handle view, uint64_t call_id, uint32_t succeeded,
                                    uint32_t format, const void* payload, size_t payload_size);
/* Any thread: merges fields (a map of field names to values) into the view model; the page sees one batch an
 * advance. */
weft_result weft_view_set_model(weft_runtime* runtime, weft_view_handle view, const char* model, size_t model_size,
                                uint32_t format, const void* fields, size_t fields_size);

#define WEFT_MESSAGE_SEND 1u
#define WEFT_MESSAGE_CALL 2u

/* A message of the page: weft.engine.send (call_id 0) or weft.engine.call. The name and the MessagePack payload are
 * valid during the callback. */
typedef struct weft_message {
    uint32_t struct_size;
    uint32_t kind; /* WEFT_MESSAGE_* */
    uint64_t call_id;
    const char* name;
    size_t name_size;
    const void* payload;
    size_t payload_size;
} weft_message;

typedef void (*weft_message_fn)(void* user, weft_view_handle view, const weft_message* message);

/* Owner thread: hands the page's messages since the last drain to fn, oldest first. fn may complete calls and emit
 * events; owner operations on the View from fn return WEFT_BUSY. */
weft_result weft_view_drain_messages(weft_runtime* runtime, weft_view_handle view, weft_message_fn fn, void* user);

/* Where a sync function writes its result. Valid during the function's call. */
typedef struct weft_value_writer weft_value_writer;

/* The function's result: one value in format (an empty result is undefined). */
weft_result weft_value_writer_set(weft_value_writer* writer, uint32_t format, const void* value, size_t size);

/* A sync function: weft.engine.invoke(name, value) calls it on the owner thread in the middle of JS with the value
 * as MessagePack. It must not call into the View (WEFT_BUSY). A result other than WEFT_OK is the invoke's exception. */
typedef weft_result (*weft_function_fn)(void* user, const void* argument, size_t argument_size,
                                        weft_value_writer* result);

/* Owner thread. Kept across page loads; registering a name again replaces it. */
weft_result weft_view_register_function(weft_runtime* runtime, weft_view_handle view, const char* name,
                                        size_t name_size, weft_function_fn fn, void* user);

/* ---- Resources. Owner thread. ---- */

/* Where a resource reader writes a file. Valid during the reader's call. */
typedef struct weft_resource_writer weft_resource_writer;

/* The file's bytes, copied; at most 16 MiB. */
weft_result weft_resource_writer_set(weft_resource_writer* writer, const void* bytes, size_t size);

/* Reads a file a page names: a scene page's image (an img's src, a url() of background-image or border-image-source:
 * a PNG, JPEG or WebP file, told apart by its bytes), or a font of its @font-face rules (a forgiving page's: a TrueType
 * or OpenType file), by its path, the URL resolved against weft_page_desc::base (its "." and ".." segments removed; a
 * URL with a scheme or one that starts with '/' as written, without a query or fragment). A data: URL is not read: its
 * bytes are in it. It runs on the owner thread while the View loads its page (fonts) or lays its scene out (images),
 * once for a path each page load, and must not call into the View (WEFT_BUSY). A file it does not write, a result
 * other than WEFT_OK, or bytes that are no image the scene decodes within the bounds (docs/engine-communication.md,
 * docs/img.md) is a scene diagnostic, and the image shows nothing; a font none of whose rule's sources reads is one
 * too (WEFT_FONT_FACE), and its text falls to other fonts. */
typedef weft_result (*weft_resource_fn)(void* user, const char* path, size_t path_size, weft_resource_writer* file);

/* Owner thread. Kept across page loads, for paths the page has not read yet; fn NULL removes it (url() images and
 * @font-face fonts are then diagnostics). */
weft_result weft_view_set_resource_reader(weft_runtime* runtime, weft_view_handle view, weft_resource_fn fn,
                                          void* user);

/* ---- The clipboard of text fields (copy, cut, paste) and navigator.clipboard. Owner thread. ---- */

/* Reads the system clipboard's text for a paste or navigator.clipboard.readText(): its UTF-8 text, written with
 * weft_resource_writer_set. Nothing written, a result other than WEFT_OK, or bytes that are not UTF-8: no text. It
 * must not call into the View (WEFT_BUSY). */
typedef weft_result (*weft_clipboard_read_fn)(void* user, weft_resource_writer* text);

/* Puts UTF-8 text on the system clipboard: a copy or cut (the selection, or what a copy listener gave), or
 * navigator.clipboard.writeText(). It must not call into the View (WEFT_BUSY). */
typedef weft_result (*weft_clipboard_write_fn)(void* user, const char* text, size_t text_size);

/* Owner thread. Kept across page loads. Both NULL removes it: the View keeps its own clipboard, the text copied last,
 * in memory (the default). One NULL and not the other: WEFT_INVALID_ARGUMENT. The shortcuts are the platform's
 * (Command+C, X, V, Z, Shift+Z, A on Apple platforms; Control+C, X, V, Z, Y, Shift+Z, A, Insert and Shift+Insert,
 * Shift+Delete elsewhere), docs/compatibility.md. */
weft_result weft_view_set_clipboard(weft_runtime* runtime, weft_view_handle view, weft_clipboard_read_fn read,
                                    weft_clipboard_write_fn write, void* user);

/* ---- Storage: the web platform's localStorage (WEFT_PAGE_WEB_PLATFORM). Owner thread. ---- */

/* Sets an item of the View's localStorage, whose items its pages share across loads (value NULL removes it): a host
 * restores what it saved, before it loads the page. Keys and values are UTF-8. WEFT_CAPACITY_EXCEEDED past the quota
 * (5 MiB of keys and values), and nothing changes. */
weft_result weft_view_set_storage_item(weft_runtime* runtime, weft_view_handle view, const char* key, size_t key_size,
                                       const char* value, size_t value_size);
/* Removes every item of the View's localStorage. */
weft_result weft_view_clear_storage(weft_runtime* runtime, weft_view_handle view);

/* An item of the View's localStorage, valid during the call. */
typedef void (*weft_storage_item_fn)(void* user, const char* key, size_t key_size, const char* value,
                                     size_t value_size);

/* When the View's localStorage changed since revision (0: in any case), hands fn its items in key order; out_revision,
 * when given, receives the revision of what the View holds. A host saves the items when the revision moved since it
 * saved them last. fn must not call into the View (WEFT_BUSY). */
weft_result weft_view_get_storage(weft_runtime* runtime, weft_view_handle view, uint64_t revision,
                                  weft_storage_item_fn fn, void* user, uint64_t* out_revision);

/* ---- Diagnostics. Owner thread. ---- */

typedef enum weft_diagnostic_kind {
    WEFT_DIAGNOSTIC_SCRIPT = 1,  /* a script or task threw */
    WEFT_DIAGNOSTIC_HANDLER = 2, /* a weft.engine handler or subscriber threw */
    WEFT_DIAGNOSTIC_FRAME = 3,   /* an animation frame callback threw */
    WEFT_DIAGNOSTIC_SCENE = 4,   /* the page's styles make no scene (the last frame stays) */
    /* what a page with the web platform wrote to its console: log, info and debug; warn; error and failed asserts */
    WEFT_DIAGNOSTIC_CONSOLE = 5,
    WEFT_DIAGNOSTIC_CONSOLE_WARNING = 6,
    WEFT_DIAGNOSTIC_CONSOLE_ERROR = 7
} weft_diagnostic_kind;

typedef struct weft_diagnostic {
    uint32_t struct_size;
    uint32_t kind; /* weft_diagnostic_kind */
    const char* message;
    size_t message_size;
} weft_diagnostic;

typedef void (*weft_diagnostic_fn)(void* user, weft_view_handle view, const weft_diagnostic* diagnostic);

/* Hands the diagnostics since the last drain to fn, oldest first; out_dropped, when given, receives how many were
 * dropped past the View's bound. */
weft_result weft_view_drain_diagnostics(weft_runtime* runtime, weft_view_handle view, weft_diagnostic_fn fn, void* user,
                                        uint32_t* out_dropped);

#ifdef __cplusplus
}
#endif

#endif
