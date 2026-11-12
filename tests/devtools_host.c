/* A host without a window or a renderer, for the developer tools' end-to-end check (tools/devtools_watch_check.mjs):
 * it shows pages and goes from one to another when told, as a game goes from screen to screen. C11 over include/weft
 * alone, as any host; the tools listen as WEFT_DEVTOOLS_LISTEN says.
 *
 *   weft_devtools_host <control file> <sheet.css,script.js[,script.js...]>...
 *
 * A page is its sheet and its scripts, read when the page is loaded; its base is the sheet's path, and its files are
 * read from where they resolve to. The first page is loaded at the start. The control file holds the number of the page
 * to show (0 the first): the host loads that page whenever the file's text changes, and ends when it is "quit". */
#include <weft/weft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#define MAX_PAGES 8
#define MAX_SCRIPTS 8
#define FRAME_MICROSECONDS 4000

/* Seconds on a monotonic clock, and a frame's sleep. */
#ifdef _WIN32
static double seconds_now(void) {
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
}

static void sleep_frame(void) {
    Sleep(FRAME_MICROSECONDS / 1000);
}
#else
static double seconds_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void sleep_frame(void) {
    usleep(FRAME_MICROSECONDS);
}
#endif

typedef struct page {
    char* sheet;
    char* scripts[MAX_SCRIPTS];
    uint32_t script_count;
} page;

/* A file's bytes with a 0 after them; NULL when it cannot be read. */
static char* read_file(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    char* bytes = NULL;
    long length = 0;
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0) {
        bytes = malloc((size_t)length + 1);
        if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
            free(bytes);
            bytes = NULL;
        }
    }
    fclose(file);
    if (bytes) {
        bytes[length] = 0;
        *size = (size_t)length;
    }
    return bytes;
}

static weft_result read_resource(void* user, const char* path, size_t path_size, weft_resource_writer* file) {
    char name[4096];
    size_t size = 0;
    char* bytes;
    (void)user;
    if (path_size >= sizeof name) {
        return WEFT_OK;
    }
    memcpy(name, path, path_size);
    name[path_size] = 0;
    bytes = read_file(name, &size);
    if (bytes) {
        weft_resource_writer_set(file, bytes, size);
        free(bytes);
    }
    return WEFT_OK;
}

static const char* file_name(const char* path) {
    const char* slash = strrchr(path, '/');
#ifdef _WIN32
    const char* backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) {
        slash = backslash;
    }
#endif
    return slash ? slash + 1 : path;
}

static void load(weft_runtime* runtime, weft_view_handle view, const page* shown) {
    weft_script scripts[MAX_SCRIPTS];
    char* sources[MAX_SCRIPTS] = {0};
    weft_page_desc desc;
    size_t sheet_size = 0;
    char* sheet = read_file(shown->sheet, &sheet_size);
    weft_result result;
    uint32_t i;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.flags = WEFT_PAGE_FORGIVING_CSS | WEFT_PAGE_WEB_PLATFORM;
    for (i = 0; i < shown->script_count; ++i) {
        size_t size = 0;
        sources[i] = read_file(shown->scripts[i], &size);
        scripts[i].name = file_name(shown->scripts[i]);
        scripts[i].name_size = strlen(scripts[i].name);
        scripts[i].source = sources[i] ? sources[i] : "";
        scripts[i].source_size = size;
    }
    desc.scripts = scripts;
    desc.script_count = shown->script_count;
    desc.stylesheet = sheet ? sheet : "";
    desc.stylesheet_size = sheet_size;
    desc.base = shown->sheet;
    desc.base_size = strlen(shown->sheet);
    result = weft_view_load(runtime, view, &desc);
    fprintf(stderr, "[host] %s: %s\n", shown->sheet, weft_result_string(result));
    for (i = 0; i < shown->script_count; ++i) {
        free(sources[i]);
    }
    free(sheet);
}

int main(int argc, char** argv) {
    page pages[MAX_PAGES];
    int page_count = 0;
    char last[64] = "0";
    weft_runtime* runtime = NULL;
    weft_view_handle view;
    const weft_view_desc desc = {sizeof desc, 0, 800, 500, 1, 0};
    double start;
    int i;
    if (argc < 3 || argc - 2 > MAX_PAGES) {
        fprintf(stderr, "usage: weft_devtools_host <control file> <sheet.css,script.js[,script.js...]>...\n");
        return 2;
    }
    for (i = 2; i < argc; ++i) {
        page* out = &pages[page_count++];
        char* part = strtok(argv[i], ",");
        out->sheet = part;
        out->script_count = 0;
        while ((part = strtok(NULL, ",")) != NULL && out->script_count < MAX_SCRIPTS) {
            out->scripts[out->script_count++] = part;
        }
    }
    if (weft_runtime_create(NULL, &runtime) != WEFT_OK || weft_view_create(runtime, &desc, &view) != WEFT_OK ||
        weft_view_set_resource_reader(runtime, view, read_resource, NULL) != WEFT_OK) {
        fprintf(stderr, "[host] no runtime\n");
        return 1;
    }
    load(runtime, view, &pages[0]);
    start = seconds_now();
    for (;;) {
        size_t size = 0;
        char* control = read_file(argv[1], &size);
        const double now = seconds_now();
        if (control && size < sizeof last && strcmp(control, last) != 0) {
            memcpy(last, control, size + 1);
            if (strncmp(last, "quit", 4) == 0) {
                free(control);
                break;
            }
            if (atoi(last) >= 0 && atoi(last) < page_count) {
                load(runtime, view, &pages[atoi(last)]);
            }
        }
        free(control);
        weft_view_advance(runtime, view, now - start);
        sleep_frame();
    }
    weft_view_destroy(runtime, view);
    weft_runtime_destroy(runtime);
    return 0;
}
