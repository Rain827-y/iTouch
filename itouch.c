#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
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
    TOUCH_TIMEOUT_MS = 3000,
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

struct live_touch {
    int held;
    double x;
    double y;
    uint64_t deadline;
};

static uint64_t ticks(int ms)
{
    return (uint64_t)ms * 1000000 * clock_base.denom / clock_base.numer;
}

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
    uint64_t duration = ticks(ms);
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

static int coordinate(const char *word, double *value)
{
    char *end;
    *value = strtod(word, &end);
    return end != word && !*end && isfinite(*value) && *value >= 0 && *value <= 1;
}

static const char *command(char *line, int touch_held)
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
    if (touch_held)
        return "ERR a live touch is in progress\n";
    double points[4] = {0};
    for (int i = 0; i < (swipe ? 4 : 2); i++) {
        if (!coordinate(words[i + 1], &points[i]))
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

static int write_bytes(int fd, const void *data, size_t remaining)
{
    const char *bytes = data;
    while (remaining) {
        ssize_t n = write(fd, bytes, remaining);
        if (n < 0 && errno == EINTR && !stopping)
            continue;
        if (n <= 0)
            return -1;
        bytes += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int send_line(int fd, const char *line)
{
    return write_bytes(fd, line, strlen(line));
}

static int lift(struct live_touch *input)
{
    if (!input->held)
        return 0;
    int result = touch(UP, input->x, input->y);
    if (!result)
        input->held = 0;
    return result;
}

static const char *live_command(struct live_touch *input, char *line)
{
    if (!strcmp(line, "ping")) {
        if (input->held)
            input->deadline = mach_absolute_time() + ticks(TOUCH_TIMEOUT_MS);
        return "OK\n";
    }
    if (!strcmp(line, "up"))
        return lift(input) ? "ERR could not lift touch\n" : "OK\n";

    char *cursor = NULL;
    const char *name = strtok_r(line, " \t\r", &cursor);
    const char *x_word = strtok_r(NULL, " \t\r", &cursor);
    const char *y_word = strtok_r(NULL, " \t\r", &cursor);
    double x, y;
    if (!name || !x_word || !y_word || strtok_r(NULL, " \t\r", &cursor) ||
        !coordinate(x_word, &x) || !coordinate(y_word, &y))
        return "ERR expected down X Y, move X Y, up, or ping\n";
    int down = !strcmp(name, "down");
    if (!down && strcmp(name, "move"))
        return "ERR expected down X Y, move X Y, up, or ping\n";
    if (down == input->held)
        return down ? "ERR touch already down\n" : "ERR touch is not down\n";

    if ((down || x != input->x || y != input->y) && touch(down ? DOWN : MOVE, x, y))
        return "ERR could not dispatch touch\n";
    input->held = 1;
    input->x = x;
    input->y = y;
    input->deadline = mach_absolute_time() + ticks(TOUCH_TIMEOUT_MS);
    return "OK\n";
}

static void end_stream(int *peer, struct live_touch *input)
{
    if (lift(input) && lift(input))
        fputs("Could not lift the live touch after closing its stream\n", stderr);
    input->held = 0;
    close(*peer);
    *peer = -1;
}

static void peer_timeout(int peer, int seconds)
{
    struct timeval timeout = {.tv_sec = seconds};
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static int serve_peers(int server)
{
    struct pollfd sockets[2] = {
        {.fd = server, .events = POLLIN},
        {.fd = -1, .events = POLLIN},
    };
    struct live_touch input = {0};
    char line[COMMAND_SIZE];
    int result = 0;
    while (!stopping) {
        int timeout = -1;
        if (input.held) {
            uint64_t now = mach_absolute_time();
            if (now >= input.deadline) {
                send_line(sockets[1].fd, "ERR live touch timed out\n");
                end_stream(&sockets[1].fd, &input);
                continue;
            }
            timeout = (int)((input.deadline - now) / ticks(1)) + 1;
        }
        int ready = poll(sockets, 2, timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            result = 1;
            break;
        }
        if (sockets[1].revents) {
            int peer = sockets[1].fd;
            if (!(sockets[1].revents & POLLIN) || read_line(peer, line) || stopping) {
                end_stream(&sockets[1].fd, &input);
            } else {
                const char *reply = live_command(&input, line);
                if (send_line(peer, reply) || reply[0] == 'E')
                    end_stream(&sockets[1].fd, &input);
            }
        }
        if (sockets[0].revents & POLLIN) {
            int peer = accept(server, NULL, NULL);
            if (peer < 0) {
                if (errno == EINTR)
                    continue;
                perror("accept");
                result = 1;
                break;
            }
            peer_timeout(peer, 2);
            if (read_line(peer, line) || stopping) {
                send_line(peer, "ERR incomplete or oversized command\n");
            } else if (strcmp(line, "stream")) {
                send_line(peer, command(line, input.held));
            } else if (sockets[1].fd >= 0) {
                send_line(peer, "ERR a live stream is already connected\n");
            } else if (!send_line(peer, "OK stream/1\n")) {
                sockets[1].fd = peer;
                continue;
            }
            close(peer);
        }
    }
    if (sockets[1].fd >= 0)
        end_stream(&sockets[1].fd, &input);
    return result;
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
    } else {
        result = serve_peers(server);
    }
    close(server);
    unlink(address.sun_path);
    CFRelease(hid);
    close(lock);
    return result;
}

static int connect_peer(void)
{
    int peer = socket(AF_UNIX, SOCK_STREAM, 0);
    if (peer < 0) {
        perror("socket");
        return -1;
    }
    peer_timeout(peer, 12);
    if (connect(peer, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        perror("connect /var/run/itouch.sock");
        close(peer);
        return -1;
    }
    return peer;
}

static int request(const char *line)
{
    int peer = connect_peer();
    if (peer < 0)
        return 1;
    int result = 1;
    char reply[COMMAND_SIZE];
    if (send_line(peer, line) || read_line(peer, reply)) {
        fputs("iTouch did not return a complete reply\n", stderr);
    } else {
        puts(reply);
        result = strncmp(reply, "OK", 2) != 0;
    }
    close(peer);
    return result;
}

static int stream(void)
{
    int peer = connect_peer();
    if (peer < 0)
        return 1;
    int result = 1;
    if (send_line(peer, "stream\n"))
        goto done;
    struct pollfd pipe[2] = {
        {.fd = peer, .events = POLLIN},
        {.fd = STDIN_FILENO, .events = POLLIN},
    };
    while (poll(pipe, 2, -1) > 0) {
        char bytes[1024];
        if (pipe[0].revents) {
            ssize_t n = read(peer, bytes, sizeof(bytes));
            if (n == 0) {
                result = 0;
                break;
            }
            if (n < 0 || write_bytes(STDOUT_FILENO, bytes, (size_t)n))
                break;
        }
        if (pipe[1].revents) {
            ssize_t n = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (n < 0 || (n > 0 && write_bytes(peer, bytes, (size_t)n)))
                break;
            if (n == 0) {
                shutdown(peer, SHUT_WR);
                pipe[1].fd = -1;
            }
        }
    }
done:
    close(peer);
    return result;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && !strcmp(argv[1], "serve"))
        return serve();
    if (argc == 2 && !strcmp(argv[1], "stream"))
        return stream();
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
        fputs("Usage: itouch ping|tap|hold|swipe|stream\n", stderr);
        return 1;
    }
    line[used] = '\0';
    return request(line);
}
