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
extern HIDEvent IOHIDEventCreateKeyboardEvent(const void *allocator, uint64_t timestamp,
                                             uint32_t usage_page, uint32_t usage,
                                             uint8_t down, uint32_t options);
extern void IOHIDEventAppendEvent(HIDEvent parent, HIDEvent child);
extern void IOHIDEventSetIntegerValue(HIDEvent event, uint32_t field, int value);
extern void IOHIDEventSetSenderID(HIDEvent event, uint64_t sender);
extern void CFRelease(const void *object);
extern uint32_t SBSSpringBoardServerPort(void);
extern int SBGetInterfaceOrientation(uint32_t port, int *orientation);
struct mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
};
extern uint64_t mach_absolute_time(void);
extern int mach_timebase_info(struct mach_timebase_info *info);
extern int mach_wait_until(uint64_t deadline);

enum {
    HID_PAGE_CONSUMER = 0x0c,
    HID_USAGE_CONSUMER_MENU = 0x40,
    HOME_PRESS_MS = 65,
    DIGITIZER_HAND = 3,
    DIGITIZER_RANGE = 1 << 0,
    DIGITIZER_TOUCH = 1 << 1,
    DIGITIZER_POSITION = 1 << 2,
    DIGITIZER_IDENTITY = 1 << 5,
    DIGITIZER_DISPLAY_INTEGRATED = (11 << 16) + 25,
    SWIPE_SAMPLES_PER_SECOND = 120,
    MAX_SCROLL_STEPS = 8,
    MIN_SCROLL_INTERVAL_MS = 20,
    MAX_SCROLL_INTERVAL_MS = 200,
    DEFAULT_SCROLL_INTERVAL_MS = 160,
    WHEEL_SETTLE_MS = 140,
    TOUCH_TIMEOUT_MS = 3000,
    COMMAND_SIZE = 256
};
enum phase { DOWN, MOVE, UP };
enum touch_path { STATIONARY, LINEAR, SMOOTH };
enum contact_kind { NO_CONTACT, POINTER_CONTACT, WHEEL_CONTACT };

static const struct sockaddr_un address = {
    .sun_family = AF_UNIX,
    .sun_path = "/var/run/itouch.sock"
};
static HIDClient hid;
static struct mach_timebase_info clock_base;
static volatile sig_atomic_t stopping;

struct live_touch {
    enum contact_kind kind;
    double x;
    double y;
    uint64_t deadline;
    double wheel_x;
    double wheel_y;
    int wheel_orientation;
};

static uint64_t ticks(int ms)
{
    return (uint64_t)ms * 1000000 * clock_base.denom / clock_base.numer;
}

static void dispatch_event(HIDEvent event)
{
    IOHIDEventSetSenderID(event, UINT64_C(0xDEFACEDBEEFFECE5));
    IOHIDEventSystemClientDispatchEvent(hid, event);
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
    dispatch_event(hand);
    CFRelease(finger);
    CFRelease(hand);
    return 0;
}

static int home_event(uint8_t down, uint64_t timestamp)
{
    HIDEvent event = IOHIDEventCreateKeyboardEvent(
        NULL, timestamp, HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_MENU, down, 0);
    if (!event)
        return -1;
    dispatch_event(event);
    CFRelease(event);
    return 0;
}

static const char *press_home(void)
{
    uint64_t start = mach_absolute_time();
    if (home_event(1, start))
        return "ERR could not press Home\n";
    int interrupted = mach_wait_until(start + ticks(HOME_PRESS_MS)) != 0;
    if (home_event(0, mach_absolute_time()))
        return "ERR could not release Home\n";
    return interrupted || stopping ? "ERR Home press interrupted\n" : "OK\n";
}

static int lift(struct live_touch *input)
{
    if (input->kind == NO_CONTACT)
        return 0;
    int result = touch(UP, input->x, input->y);
    if (!result)
        input->kind = NO_CONTACT;
    return result;
}

static int move_touch(struct live_touch *input, double end_x, double end_y,
                      int ms, enum touch_path path)
{
    uint64_t duration = ticks(ms);
    uint64_t start = mach_absolute_time();
    int intervals = path == STATIONARY ? 1 : (ms * SWIPE_SAMPLES_PER_SECOND + 999) / 1000;
    double x = input->x;
    double y = input->y;
    for (int step = 1; step < intervals && !stopping; step++) {
        if (mach_wait_until(start + duration * step / intervals) != 0)
            return -1;
        if (stopping)
            break;

        // Skip missed samples after a late wakeup so their events do not bunch together.
        uint64_t elapsed = mach_absolute_time() - start;
        int due = elapsed >= duration ? intervals - 1 : (int)(elapsed * intervals / duration);
        if (due > step)
            step = due;

        // The last movement reaches the endpoint before the separate lift event.
        double t = (double)step / (intervals - 1);
        if (path == SMOOTH)
            t = t * t * (3 - 2 * t);
        double next_x = x + t * (end_x - x);
        double next_y = y + t * (end_y - y);
        if (touch(MOVE, next_x, next_y))
            return -1;
        input->x = next_x;
        input->y = next_y;
    }
    return stopping || mach_wait_until(start + duration) != 0 ? -1 : 0;
}

static int gesture(double x, double y, double end_x, double end_y, int ms, enum touch_path path)
{
    if (touch(DOWN, x, y))
        return -1;
    struct live_touch input = {.kind = POINTER_CONTACT, .x = x, .y = y};
    int failed = move_touch(&input, end_x, end_y, ms, path);
    int release_failed = lift(&input);
    return failed || release_failed || stopping ? -1 : 0;
}

static int coordinate(const char *word, double *value)
{
    char *end;
    *value = strtod(word, &end);
    return end != word && !*end && isfinite(*value) && *value >= 0 && *value <= 1;
}

static int current_orientation(void)
{
    uint32_t port = SBSSpringBoardServerPort();
    int orientation = 0;
    if (!port || SBGetInterfaceOrientation(port, &orientation) || orientation < 1 || orientation > 4)
        return 0;
    return orientation;
}

static void native_point(int orientation, double *x, double *y)
{
    double u = *x, v = *y;
    switch (orientation) {
    case 2:
        *x = 1 - u;
        *y = 1 - v;
        break;
    case 3:
        *x = 1 - v;
        *y = u;
        break;
    case 4:
        *x = v;
        *y = 1 - u;
        break;
    }
}

static int settle_wheel(struct live_touch *input)
{
    if (input->kind != WHEEL_CONTACT)
        return 0;
    int interrupted = !stopping && mach_wait_until(input->deadline) != 0;
    int failed = lift(input);
    return interrupted || failed || stopping ? -1 : 0;
}

static double wheel_start(double y, double delta)
{
    return fmax(0.10 - fmin(0, delta), fmin(y, 0.90 - fmax(0, delta)));
}

static int wheel_drag(struct live_touch *input, int orientation, double x, double y,
                      double delta, int duration)
{
    if (input->kind == WHEEL_CONTACT && input->wheel_orientation != orientation && lift(input))
        return -1;

    // Start a fresh stroke only when the old one has too little travel left.
    // A long burst can span two strokes at the screen edge.
    while (fabs(delta) > 0.00001) {
        if (stopping)
            return -1;
        double end_y = fmax(0.10, fmin(input->wheel_y + delta, 0.90));
        if (input->kind == WHEEL_CONTACT && fabs(end_y - input->wheel_y) < 0.02 &&
            settle_wheel(input))
            return -1;
        if (input->kind == NO_CONTACT) {
            double start_y = wheel_start(y, delta);
            double down_x = x, down_y = start_y;
            native_point(orientation, &down_x, &down_y);
            if (touch(DOWN, down_x, down_y))
                return -1;
            *input = (struct live_touch){
                .kind = WHEEL_CONTACT, .x = down_x, .y = down_y,
                .wheel_x = x, .wheel_y = start_y, .wheel_orientation = orientation,
            };
            end_y = fmax(0.10, fmin(start_y + delta, 0.90));
        }
        double distance = end_y - input->wheel_y;
        int travel_ms = (int)fmax(25, duration * fabs(distance / delta));
        double end_x = input->wheel_x;
        double native_y = end_y;
        native_point(orientation, &end_x, &native_y);
        if (move_touch(input, end_x, native_y, travel_ms, SMOOTH))
            return -1;
        input->wheel_y = end_y;
        input->deadline = mach_absolute_time() + ticks(WHEEL_SETTLE_MS);
        delta -= distance;
    }
    return 0;
}

static const char *scroll_gesture(const char *steps_word, const char *x_word,
                                  const char *y_word, const char *interval_word,
                                  int drag, struct live_touch *active)
{
    if (!steps_word || (x_word && !y_word))
        return "ERR expected scroll STEPS [X Y [INTERVAL_MS]]\n";
    char *end;
    long steps = strtol(steps_word, &end, 10);
    if (*end || !steps || steps < -MAX_SCROLL_STEPS || steps > MAX_SCROLL_STEPS)
        return "ERR scroll steps must be a nonzero integer from -8 to 8\n";
    double x = 0.5, y = 0.5;
    if (x_word && (!coordinate(x_word, &x) || !coordinate(y_word, &y)))
        return "ERR scroll coordinates must be numbers from 0 to 1\n";
    long interval = DEFAULT_SCROLL_INTERVAL_MS;
    if (interval_word) {
        interval = strtol(interval_word, &end, 10);
        if (*end || interval < MIN_SCROLL_INTERVAL_MS || interval > MAX_SCROLL_INTERVAL_MS)
            return "ERR scroll interval must be an integer from 20 to 200 ms\n";
    }
    int orientation = current_orientation();
    if (!orientation)
        return "ERR could not read interface orientation\n";

    // Keep the swipe inside the display so it doesn't start a system edge gesture.
    double pace = (double)(MAX_SCROLL_INTERVAL_MS - interval) /
        (MAX_SCROLL_INTERVAL_MS - MIN_SCROLL_INTERVAL_MS);
    x = fmax(0.04, fmin(x, 0.96));
    double distance = drag ? 0.065 + 0.055 * pace : 0.105 + 0.085 * pace;
    double delta = copysign(fmin(0.65, distance * labs(steps)), steps);
    int duration = (int)((drag ? 70 - 38 * pace : 105 - 55 * pace) +
        8 * fmin(3, labs(steps) - 1));
    int failed;
    if (drag) {
        struct live_touch standalone = {0};
        failed = wheel_drag(active ? active : &standalone, orientation, x, y, delta, duration);
        if (standalone.kind != NO_CONTACT) {
            int release_failed = settle_wheel(&standalone);
            failed = failed || release_failed;
        }
    } else {
        if (active && settle_wheel(active))
            return "ERR could not release wheel touch\n";
        double start_y = wheel_start(y, delta);
        double end_x = x, end_y = start_y + delta;
        native_point(orientation, &x, &start_y);
        native_point(orientation, &end_x, &end_y);
        failed = gesture(x, start_y, end_x, end_y, duration, SMOOTH);
    }
    return failed ? "ERR scroll touch event failed\n" : "OK\n";
}

static const char *command(char *line, int touch_held, struct live_touch *stream_input)
{
    const char *invalid = "ERR expected ping, home, tap, hold, swipe, or scroll\n";
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
    if (count == 1 && !strcmp(words[0], "home"))
        return touch_held ? "ERR a live touch is in progress\n" : press_home();
    int drag = !strcmp(words[0], "scroll-drag");
    if (drag || !strcmp(words[0], "scroll")) {
        if (touch_held)
            return "ERR a live touch is in progress\n";
        if (count == 2)
            return scroll_gesture(words[1], NULL, NULL, NULL, drag, stream_input);
        if (count == 4 || count == 5)
            return scroll_gesture(words[1], words[2], words[3], count == 5 ? words[4] : NULL,
                                  drag, stream_input);
        return "ERR expected scroll or scroll-drag STEPS [X Y [INTERVAL_MS]]\n";
    }
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
    if (gesture(points[0], points[1], points[2], points[3], (int)duration, swipe ? LINEAR : STATIONARY))
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

static const char *live_command(struct live_touch *input, char *line)
{
    if (!strcmp(line, "ping")) {
        if (input->kind == POINTER_CONTACT)
            input->deadline = mach_absolute_time() + ticks(TOUCH_TIMEOUT_MS);
        return "OK\n";
    }
    if (!strcmp(line, "up"))
        return lift(input) ? "ERR could not lift touch\n" : "OK\n";
    if (!strcmp(line, "home"))
        return lift(input) ? "ERR could not lift touch\n" : press_home();
    if (!strncmp(line, "scroll ", 7) || !strncmp(line, "scroll-drag ", 12))
        return command(line, input->kind == POINTER_CONTACT, input);

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
    if (down == (input->kind == POINTER_CONTACT))
        return down ? "ERR touch already down\n" : "ERR touch is not down\n";
    if (down && input->kind == WHEEL_CONTACT && lift(input))
        return "ERR could not release wheel touch\n";

    if ((down || x != input->x || y != input->y) && touch(down ? DOWN : MOVE, x, y))
        return "ERR could not dispatch touch\n";
    input->kind = POINTER_CONTACT;
    input->x = x;
    input->y = y;
    input->deadline = mach_absolute_time() + ticks(TOUCH_TIMEOUT_MS);
    return "OK\n";
}

static void end_stream(int *peer, struct live_touch *input)
{
    if (lift(input) && lift(input))
        fputs("Could not lift the live touch after closing its stream\n", stderr);
    input->kind = NO_CONTACT;
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
        if (input.kind != NO_CONTACT) {
            uint64_t now = mach_absolute_time();
            if (now >= input.deadline) {
                if (input.kind == POINTER_CONTACT || lift(&input)) {
                    send_line(sockets[1].fd, "ERR live touch timed out\n");
                    end_stream(&sockets[1].fd, &input);
                }
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
                send_line(peer, command(line, input.kind != NO_CONTACT, NULL));
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

static int interface_orientation(void)
{
    int orientation = current_orientation();
    if (!orientation) {
        fputs("Could not read interface orientation\n", stderr);
        return 1;
    }
    printf("%d\n", orientation);
    return 0;
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
    if (argc == 2 && !strcmp(argv[1], "orientation"))
        return interface_orientation();
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
        fputs("Usage: itouch ping|orientation|home|tap|hold|swipe|scroll|scroll-drag|stream\n", stderr);
        return 1;
    }
    line[used] = '\0';
    return request(line);
}
