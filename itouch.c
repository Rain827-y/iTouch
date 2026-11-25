#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* Private IOKit ABI exported by the arm64 iOS 10 shared cache. */
typedef void *HIDEvent;
typedef void *HIDClient;
extern HIDClient IOHIDEventSystemClientCreate(const void *allocator);
extern void IOHIDEventSystemClientDispatchEvent(HIDClient client, HIDEvent event);
extern HIDEvent IOHIDEventCreateDigitizerEvent(
    const void *allocator, uint64_t timestamp, uint32_t type, uint32_t index,
    uint32_t identity, uint32_t mask, uint32_t buttons, double x, double y,
    double z, double pressure, double barrel_pressure, uint8_t range,
    uint8_t touch, uint32_t options);
extern HIDEvent IOHIDEventCreateDigitizerFingerEvent(
    const void *allocator, uint64_t timestamp, uint32_t index, uint32_t identity,
    uint32_t mask, double x, double y, double z, double pressure, double twist,
    uint8_t range, uint8_t touch, uint32_t options);
extern void IOHIDEventAppendEvent(HIDEvent parent, HIDEvent child);
extern void IOHIDEventSetIntegerValue(HIDEvent event, uint32_t field, int value);
extern void IOHIDEventSetSenderID(HIDEvent event, uint64_t sender);
extern void CFRelease(const void *object);
struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};
extern uint64_t mach_absolute_time(void);
extern int mach_timebase_info(struct mach_timebase_info *info);
extern int mach_wait_until(uint64_t deadline);

enum {
    DIGITIZER_HAND = 3,
    DIGITIZER_RANGE = 1 << 0,
    DIGITIZER_TOUCH = 1 << 1,
    DIGITIZER_POSITION = 1 << 2,
    DIGITIZER_IDENTITY = 1 << 5,
    DIGITIZER_DISPLAY_INTEGRATED = (11 << 16) + 25,
    SWIPE_SAMPLES_PER_SECOND = 120,
    COMMAND_SIZE = 256
};
enum phase { DOWN, MOVE, UP };

static const struct sockaddr_un address = {
    .sun_family = AF_UNIX,
    .sun_path = "/var/run/itouch.sock"
};
static HIDClient hid;
static struct mach_timebase_info clock_base;
static volatile sig_atomic_t stopping;

static int touch(enum phase phase, double x, double y)
{
    uint64_t time = mach_absolute_time();
    uint8_t held = phase != UP;
    uint32_t mask = DIGITIZER_POSITION;
    if (phase != MOVE)
        mask |= DIGITIZER_RANGE | DIGITIZER_TOUCH | DIGITIZER_IDENTITY;

    HIDEvent hand = IOHIDEventCreateDigitizerEvent(
        NULL, time, DIGITIZER_HAND, 0, 1, mask, 0, 0, 0, 0, 0, 0, held, held, 0);
    if (!hand)
        return -1;
    HIDEvent finger = IOHIDEventCreateDigitizerFingerEvent(
        NULL, time, 1, 2, mask, x, y, 0, 0, 0, held, held, 0);
    if (!finger) {
        CFRelease(hand);
        return -1;
    }
    IOHIDEventAppendEvent(hand, finger);
    IOHIDEventSetIntegerValue(hand, DIGITIZER_DISPLAY_INTEGRATED, 1);
    IOHIDEventSetSenderID(hand, UINT64_C(0xDEFACEDBEEFFECE5));
    IOHIDEventSystemClientDispatchEvent(hid, hand);
    CFRelease(finger);
    CFRelease(hand);
    return 0;
}

static int gesture(double x, double y, double end_x, double end_y, int ms, int swipe)
{
    uint64_t duration = (uint64_t)ms * 1000000 * clock_base.denom / clock_base.numer;
    uint64_t start = mach_absolute_time();
    if (touch(DOWN, x, y))
        return -1;
    int intervals = swipe ? (ms * SWIPE_SAMPLES_PER_SECOND + 999) / 1000 : 1;
    int failed = 0;
    double last_x = x;
    double last_y = y;
    for (int step = 1; step < intervals && !stopping; step++) {
        if (mach_wait_until(start + duration * step / intervals) != 0) {
            failed = 1;
            break;
        }
        if (stopping)
            break;

        // Skip missed samples after a late wakeup so their events do not bunch together.
        uint64_t elapsed = mach_absolute_time() - start;
        int due = elapsed >= duration ? intervals - 1 : (int)(elapsed * intervals / duration);
        if (due > step)
            step = due;

        // The last movement reaches the endpoint before the separate lift event.
        double t = (double)step / (intervals - 1);
        double next_x = x + t * (end_x - x);
        double next_y = y + t * (end_y - y);
        if (touch(MOVE, next_x, next_y)) {
            failed = 1;
            break;
        }
        last_x = next_x;
        last_y = next_y;
    }
    if (!failed && !stopping)
        failed = mach_wait_until(start + duration) != 0;
    int release_failed = touch(UP, last_x, last_y);
    return failed || release_failed || stopping ? -1 : 0;
}

static const char *command(char *line)
{
    const char *invalid = "ERR expected ping, tap X Y, hold X Y MS, or swipe X1 Y1 X2 Y2 MS\n";
    char *words[6];
    char *cursor = NULL;
    int count = 0;
    for (char *part = strtok_r(line, " \t\r", &cursor); part;
         part = strtok_r(NULL, " \t\r", &cursor)) {
        if (count == 6)
            return invalid;
        words[count++] = part;
    }
    if (!count)
        return invalid;
    if (count == 1 && !strcmp(words[0], "ping"))
        return "OK iTouch/1\n";
    int tap = count == 3 && !strcmp(words[0], "tap");
    int hold = count == 4 && !strcmp(words[0], "hold");
    int swipe = count == 6 && !strcmp(words[0], "swipe");
    if (!tap && !hold && !swipe)
        return invalid;
    double points[4] = {0};
    for (int i = 0; i < (swipe ? 4 : 2); i++) {
        char *end;
        points[i] = strtod(words[i + 1], &end);
        if (*end || !isfinite(points[i]) || points[i] < 0 || points[i] > 1)
            return "ERR coordinates must be numbers from 0 to 1\n";
    }
    long duration = 65;
    if (!tap) {
        char *end;
        duration = strtol(words[count - 1], &end, 10);
        if (*end || duration < 30 || duration > 5000)
            return "ERR duration must be an integer from 30 to 5000 ms\n";
    }
    if (gesture(points[0], points[1], points[2], points[3], (int)duration, swipe))
        return "ERR touch event failed\n";
    return "OK\n";
}

static int read_line(int fd, char line[COMMAND_SIZE])
{
    for (unsigned i = 0; i < COMMAND_SIZE - 1; i++) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n != 1 || ch == '\0')
            return -1;
        if (ch == '\n') {
            line[i] = '\0';
            return 0;
        }
        line[i] = ch;
    }
    return -1;
}

static int send_line(int fd, const char *line)
{
    size_t remaining = strlen(line);
    while (remaining) {
        ssize_t n = write(fd, line, remaining);
        if (n < 0 && errno == EINTR && !stopping)
            continue;
        if (n <= 0)
            return -1;
        line += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static int serve(void)
{
    if (geteuid() != 0) {
        fputs("The iTouch service must run as root\n", stderr);
        return 1;
    }
    if (mach_timebase_info(&clock_base) != 0 || !clock_base.numer || !clock_base.denom) {
        fputs("Could not read the system clock scale\n", stderr);
        return 1;
    }
    int qos_error = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (qos_error)
        fprintf(stderr, "Could not set input thread QoS: %s\n", strerror(qos_error));
    struct sigaction handler = {.sa_handler = stop};
    sigemptyset(&handler.sa_mask);
    sigaction(SIGTERM, &handler, NULL);
    sigaction(SIGINT, &handler, NULL);
    int lock = open("/var/run/itouch.lock", O_CREAT | O_RDONLY | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        perror("lock /var/run/itouch.lock");
        if (lock >= 0)
            close(lock);
        return 1;
    }
    /* A crash or SIGKILL can leave the socket path behind. */
    struct stat entry;
    if (lstat(address.sun_path, &entry) == 0) {
        if (!S_ISSOCK(entry.st_mode) || entry.st_uid != 0) {
            fputs("Unexpected file at /var/run/itouch.sock\n", stderr);
            close(lock);
            return 1;
        }
        if (unlink(address.sun_path) != 0) {
            perror("unlink /var/run/itouch.sock");
            close(lock);
            return 1;
        }
    }
    hid = IOHIDEventSystemClientCreate(NULL);
    if (!hid) {
        fputs("Could not create IOHID event client\n", stderr);
        close(lock);
        return 1;
    }
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        CFRelease(hid);
        close(lock);
        return 1;
    }
    umask(0177);
    if (bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("bind /var/run/itouch.sock");
        close(server);
        CFRelease(hid);
        close(lock);
        return 1;
    }
    int result = 0;
    if (listen(server, 8) != 0) {
        perror("listen");
        result = 1;
    }
    while (!stopping && !result) {
        int peer = accept(server, NULL, NULL);
        if (peer < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            result = 1;
            break;
        }
        struct timeval timeout = {.tv_sec = 2};
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        char line[COMMAND_SIZE];
        const char *reply = read_line(peer, line) == 0 && !stopping
            ? command(line) : "ERR incomplete or oversized command\n";
        send_line(peer, reply);
        close(peer);
    }
    close(server);
    unlink(address.sun_path);
    CFRelease(hid);
    close(lock);
    return result;
}

static int request(const char *line)
{
    int peer = socket(AF_UNIX, SOCK_STREAM, 0);
    if (peer < 0) {
        perror("socket");
        return 1;
    }
    struct timeval timeout = {.tv_sec = 12};
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int result = 1;
    char reply[COMMAND_SIZE];
    if (connect(peer, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("connect /var/run/itouch.sock");
    } else if (send_line(peer, line) || read_line(peer, reply)) {
        fputs("iTouch did not return a complete reply\n", stderr);
    } else {
        puts(reply);
        result = strncmp(reply, "OK", 2) != 0;
    }
    close(peer);
    return result;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "serve"))
        return serve();
    char line[COMMAND_SIZE];
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        size_t length = strlen(argv[i]);
        if (length + used >= sizeof(line) - 2 || strpbrk(argv[i], "\r\n")) {
            fputs("Invalid command length or embedded newline\n", stderr);
            return 1;
        }
        memcpy(line + used, argv[i], length);
        used += length;
        line[used++] = i == argc - 1 ? '\n' : ' ';
    }
    if (!used) {
        fputs("Usage: itouch ping|tap|hold|swipe\n", stderr);
        return 1;
    }
    line[used] = '\0';
    return request(line);
}
