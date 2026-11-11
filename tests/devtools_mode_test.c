/* One host for both builds of the engine (docs/devtools.md). It is compiled once, as C11, against include/weft alone, with
 * no idea which build it will be given: the argument says what to expect from the library it was started with. */
#include <weft/weft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define close_socket close
#define INVALID_SOCKET (-1)
#endif

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                            \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static void set_listen(const char* value) {
#ifdef _WIN32
    _putenv_s("WEFT_DEVTOOLS_LISTEN", value);
#else
    if (value[0]) {
        setenv("WEFT_DEVTOOLS_LISTEN", value, 1);
    } else {
        unsetenv("WEFT_DEVTOOLS_LISTEN");
    }
#endif
}

static weft_runtime* create(void) {
    weft_runtime* runtime = NULL;
    REQUIRE(weft_runtime_create(NULL, &runtime) == WEFT_OK);
    return runtime;
}

static uint32_t listening_port(weft_runtime* runtime) {
    weft_devtools_info info = {sizeof info, 0};
    REQUIRE(weft_devtools_get_info(runtime, &info) == WEFT_OK);
    return info.port;
}

/* The answer to a GET of the path into buffer; 0 when nothing listens on the port. */
static int fetch(uint32_t port, const char* path, char* buffer, size_t capacity) {
    struct sockaddr_in address;
    char request[128];
    size_t used = 0;
    socket_t socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(socket_handle != INVALID_SOCKET);
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)port);
    if (connect(socket_handle, (const struct sockaddr*)&address, sizeof address) != 0) {
        close_socket(socket_handle);
        return 0;
    }
    snprintf(request, sizeof request, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    REQUIRE(send(socket_handle, request, (int)strlen(request), 0) == (int)strlen(request));
    for (;;) {
        int received;
        REQUIRE(used + 1 < capacity);
        received = (int)recv(socket_handle, buffer + used, (int)(capacity - used - 1), 0);
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
    }
    buffer[used] = 0;
    close_socket(socket_handle);
    return 1;
}

static void check_release(void) {
    weft_runtime* runtime;
    weft_devtools_desc desc = {sizeof desc, 0, 0, 0};
    weft_devtools_info info = {sizeof info, 0};
    /* The variable means nothing to this build. */
    set_listen("9230");
    runtime = create();
    REQUIRE(weft_devtools_start(runtime, NULL) == WEFT_UNAVAILABLE);
    REQUIRE(weft_devtools_start(runtime, &desc) == WEFT_UNAVAILABLE);
    REQUIRE(weft_devtools_get_info(runtime, &info) == WEFT_UNAVAILABLE);
    REQUIRE(weft_devtools_stop(runtime) == WEFT_UNAVAILABLE);
    REQUIRE(weft_runtime_destroy(runtime) == WEFT_OK);
}

static void check_dev(void) {
    char answer[1024];
    weft_runtime* runtime;
    weft_runtime* other;
    weft_devtools_desc taken = {sizeof taken, 0, 0, 0};
    weft_devtools_desc bad = {sizeof bad, 2u, 0, 0};
    uint32_t port;

    /* Off until asked for. */
    set_listen("");
    runtime = create();
    REQUIRE(listening_port(runtime) == 0);
    REQUIRE(weft_devtools_stop(runtime) == WEFT_OK);
    REQUIRE(weft_devtools_start(runtime, &bad) == WEFT_INVALID_ARGUMENT);

    /* Asked for by the host. */
    REQUIRE(weft_devtools_start(runtime, NULL) == WEFT_OK);
    port = listening_port(runtime);
    REQUIRE(port != 0);
    REQUIRE(weft_devtools_start(runtime, NULL) == WEFT_BUSY);
    REQUIRE(fetch(port, "/json/version", answer, sizeof answer));
    REQUIRE(strstr(answer, "200 OK") && strstr(answer, "\"Browser\":\"Weft/"));
    REQUIRE(fetch(port, "/nothing", answer, sizeof answer));
    REQUIRE(strstr(answer, "404"));

    /* A port asked for by number is that port or a refusal. */
    other = create();
    taken.port = port;
    REQUIRE(weft_devtools_start(other, &taken) == WEFT_BUSY);
    REQUIRE(listening_port(other) == 0);
    /* With no number the next runtime takes the next free port. */
    REQUIRE(weft_devtools_start(other, NULL) == WEFT_OK);
    REQUIRE(listening_port(other) != port);
    REQUIRE(weft_runtime_destroy(other) == WEFT_OK);

    REQUIRE(weft_devtools_stop(runtime) == WEFT_OK);
    REQUIRE(listening_port(runtime) == 0);
    REQUIRE(!fetch(port, "/json/version", answer, sizeof answer));
    REQUIRE(weft_runtime_destroy(runtime) == WEFT_OK);

    /* Asked for by the environment, with no change to the host; a runtime is created even when the value is wrong. */
    set_listen("not a port");
    runtime = create();
    REQUIRE(listening_port(runtime) == 0);
    REQUIRE(weft_runtime_destroy(runtime) == WEFT_OK);
    set_listen("0");
    runtime = create();
    port = listening_port(runtime);
    REQUIRE(port != 0);
    REQUIRE(fetch(port, "/json/version", answer, sizeof answer));
    REQUIRE(strstr(answer, "200 OK"));
    /* Destroying the runtime lets the port go. */
    REQUIRE(weft_runtime_destroy(runtime) == WEFT_OK);
    REQUIRE(!fetch(port, "/json/version", answer, sizeof answer));
    set_listen("");
}

int main(int argc, char** argv) {
    weft_build_info info;
    int dev;
#ifdef _WIN32
    WSADATA data;
    REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    REQUIRE(argc == 2 && (!strcmp(argv[1], "dev") || !strcmp(argv[1], "release")));
    dev = !strcmp(argv[1], "dev");

    info.struct_size = 4;
    REQUIRE(weft_get_build_info(&info) == WEFT_INVALID_ARGUMENT);
    REQUIRE(weft_get_build_info(NULL) == WEFT_INVALID_ARGUMENT);
    memset(&info, 0, sizeof info);
    info.struct_size = sizeof info;
    REQUIRE(weft_get_build_info(&info) == WEFT_OK);
    REQUIRE(info.abi_version == WEFT_ABI_VERSION);
    REQUIRE(((info.flags & WEFT_BUILD_DEVTOOLS) != 0) == dev);

    if (dev) {
        check_dev();
    } else {
        check_release();
    }
    printf("devtools mode: %s\n", argv[1]);
    return 0;
}
