#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <libusb-1.0/libusb.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#define AW_VID 0x187c
#define AW_PID 0x0551

#define PREAMBLE 0x03
#define CMD_QUERY 0x20
#define CMD_USER_ANIM 0x21
#define CMD_POWER_ANIM 0x22
#define CMD_ZONE_SELECT 0x23
#define CMD_ADD_ACTION 0x24
#define CMD_SET_DIM 0x26

#define QUERY_ANIM_COUNT 0x03

#define ANIM_START_NEW 0x0001
#define ANIM_FINISH_SAVE 0x0002
#define ANIM_FINISH_PLAY 0x0003
#define ANIM_REMOVE 0x0004
#define ANIM_SET_DEFAULT 0x0006

#define ACTION_COLOR 0x00

#define ANIM_ID_STARTUP 0x0008
#define ANIM_ID_AC_SLEEP 0x005B
#define ANIM_ID_AC_CHARGED 0x005C
#define ANIM_ID_AC_CHARGING 0x005D
#define ANIM_ID_DC_SLEEP 0x005E
#define ANIM_ID_DC_ON 0x005F
#define ANIM_ID_DC_LOW 0x0060
#define ANIM_ID_DEFAULT 0x0061

/* Scancode emitted by Fn+F5 (Keyboard Backlight key) on Dell G15 5530 */
#define MSC_SCAN_LIGHT_KEY 105
#define ATKBD_BY_PATH "/dev/input/by-path/platform-i8042-serio-0-event-kbd"
#define DELL_KBD_SYSFS "/sys/class/leds/dell::kbd_backlight/brightness"

static const uint8_t ZONES_4[4] = {0x00, 0x01, 0x02, 0x03};

static volatile sig_atomic_t g_running = 1;
static libusb_context *g_usb_ctx = NULL;
static libusb_device_handle *g_usb_handle = NULL;

static void handle_sig(int sig) {
    (void)sig;
    g_running = 0;
}

typedef struct {
    int brightness; /* 0..100 */
    uint32_t color; /* 24-bit RGB hex */
    int initialized;
} State;

static void get_state_path(char *buf, size_t sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, sz, "%s/g15-kbd-backlight.conf", xdg);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = (pw && pw->pw_dir) ? pw->pw_dir : "/tmp";
    }
    snprintf(buf, sz, "%s/.config/g15-kbd-backlight.conf", home);
}

static State load_state(void) {
    State st = {.brightness = 100, .color = 0x00FFFF, .initialized = 0};
    char path[512];
    get_state_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return st;

    int b = 100, init = 0;
    unsigned int c = 0x00FFFF;
    if (fscanf(f, "brightness=%d\ncolor=%x\ninitialized=%d\n", &b, &c, &init) >= 2) {
        if (b < 0) b = 0;
        if (b > 100) b = 100;
        st.brightness = b;
        st.color = c & 0xFFFFFF;
        st.initialized = init;
    }
    fclose(f);
    return st;
}

static void save_state(const State *st) {
    char path[512];
    get_state_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "brightness=%d\ncolor=%06x\ninitialized=%d\n",
            st->brightness, st->color & 0xFFFFFF, st->initialized);
    fclose(f);
}

static int read_sysfs_brightness_pct(void) {
    FILE *f = fopen(DELL_KBD_SYSFS, "r");
    if (!f) return -1;
    int val = -1;
    if (fscanf(f, "%d", &val) != 1) val = -1;
    fclose(f);
    if (val < 0) return -1;
    if (val == 0) return 0;
    if (val == 1) return 50;
    return 100;
}

static int awelc_send(libusb_device_handle *handle, const uint8_t *data, size_t len) {
    uint8_t buf[33] = {0};
    if (len > sizeof(buf)) len = sizeof(buf);
    memcpy(buf, data, len);

    int ret = libusb_control_transfer(
        handle,
        0x21,   /* HOST_TO_DEVICE | CLASS | INTERFACE */
        0x09,   /* SET_REPORT */
        0x0202, /* Output Report */
        0x0000,
        buf,
        sizeof(buf),
        1000
    );
    if (ret < 0) {
        fprintf(stderr, "libusb SET_REPORT failed: %s (%d)\n", libusb_error_name(ret), ret);
        return ret;
    }
    return 0;
}

static int awelc_query(libusb_device_handle *handle, const uint8_t *data, size_t len, uint8_t *reply_out) {
    if (awelc_send(handle, data, len) != 0) return -1;
    uint8_t inbuf[33] = {0};
    int ret = libusb_control_transfer(
        handle,
        0xA1,   /* DEVICE_TO_HOST | CLASS | INTERFACE */
        0x01,   /* GET_REPORT */
        0x0101, /* Input Report */
        0x0000,
        inbuf,
        sizeof(inbuf),
        1000
    );
    if (ret < 0) return -1;
    if (reply_out) memcpy(reply_out, inbuf, sizeof(inbuf));
    return 0;
}

static libusb_device_handle *awelc_acquire(void) {
    if (!g_usb_ctx && libusb_init(&g_usb_ctx) != 0) {
        fprintf(stderr, "Failed to initialize libusb\n");
        return NULL;
    }
    if (!g_usb_handle) {
        g_usb_handle = libusb_open_device_with_vid_pid(g_usb_ctx, AW_VID, AW_PID);
        if (!g_usb_handle) {
            fprintf(stderr, "Failed to open Alienware AW-ELC USB device (187c:0551)\n");
            return NULL;
        }
    }
    if (libusb_kernel_driver_active(g_usb_handle, 0) != 0) {
        libusb_detach_kernel_driver(g_usb_handle, 0);
    }
    if (libusb_claim_interface(g_usb_handle, 0) != 0) {
        /* Reopen once if handle became stale after suspend/resume */
        libusb_close(g_usb_handle);
        g_usb_handle = libusb_open_device_with_vid_pid(g_usb_ctx, AW_VID, AW_PID);
        if (!g_usb_handle) return NULL;
        if (libusb_kernel_driver_active(g_usb_handle, 0) != 0) {
            libusb_detach_kernel_driver(g_usb_handle, 0);
        }
        if (libusb_claim_interface(g_usb_handle, 0) != 0) {
            fprintf(stderr, "Failed to claim USB interface 0\n");
            return NULL;
        }
    }
    return g_usb_handle;
}

static void awelc_release(bool keep_open) {
    if (g_usb_handle) {
        libusb_release_interface(g_usb_handle, 0);
        libusb_attach_kernel_driver(g_usb_handle, 0);
        if (!keep_open) {
            libusb_close(g_usb_handle);
            g_usb_handle = NULL;
        }
    }
    if (!keep_open && g_usb_ctx) {
        libusb_exit(g_usb_ctx);
        g_usb_ctx = NULL;
    }
}

static uint8_t anim_cmd_type(uint16_t anim_id) {
    return (anim_id >= ANIM_ID_AC_SLEEP && anim_id <= ANIM_ID_DC_LOW)
               ? CMD_POWER_ANIM
               : CMD_USER_ANIM;
}

static int awelc_anim_op(libusb_device_handle *h, uint16_t subcmd, uint16_t anim_id) {
    uint8_t pkt[6] = {
        PREAMBLE,
        anim_cmd_type(anim_id),
        (uint8_t)((subcmd >> 8) & 0xFF),
        (uint8_t)(subcmd & 0xFF),
        (uint8_t)((anim_id >> 8) & 0xFF),
        (uint8_t)(anim_id & 0xFF)
    };
    return awelc_send(h, pkt, sizeof(pkt));
}

static void awelc_send_zone_color_actions(libusb_device_handle *h, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    for (size_t i = 0; i < sizeof(ZONES_4); i++) {
        uint8_t zone_pkt[6] = {PREAMBLE, CMD_ZONE_SELECT, 0x01, 0x00, 0x01, ZONES_4[i]};
        awelc_send(h, zone_pkt, sizeof(zone_pkt));

        uint8_t act_pkt[10] = {
            PREAMBLE, CMD_ADD_ACTION, ACTION_COLOR,
            0x00, 0x01, /* duration = 1 */
            0x00, 0x02, /* tempo = 2 */
            r, g, b
        };
        awelc_send(h, act_pkt, sizeof(act_pkt));
    }
}

static void awelc_clear_all_animations(libusb_device_handle *h) {
    static const uint16_t fixed_ids[] = {
        ANIM_ID_AC_SLEEP, ANIM_ID_AC_CHARGED, ANIM_ID_AC_CHARGING,
        ANIM_ID_DC_SLEEP, ANIM_ID_DC_ON, ANIM_ID_DC_LOW,
        ANIM_ID_DEFAULT, ANIM_ID_STARTUP, 0x0081, 0xFFFF, 0x00FF, 0x0000
    };
    for (size_t i = 0; i < sizeof(fixed_ids) / sizeof(fixed_ids[0]); i++) {
        awelc_anim_op(h, ANIM_REMOVE, fixed_ids[i]);
    }

    for (int iter = 0; iter < 32; iter++) {
        uint8_t q[3] = {PREAMBLE, CMD_QUERY, QUERY_ANIM_COUNT};
        uint8_t rep[33] = {0};
        if (awelc_query(h, q, sizeof(q), rep) != 0) break;
        uint16_t count = ((uint16_t)rep[3] << 8) | rep[4];
        uint16_t last_id = ((uint16_t)rep[5] << 8) | rep[6];
        if (count == 0 || last_id == 0) break;
        awelc_anim_op(h, ANIM_REMOVE, last_id);
    }
}

static void awelc_program_static_slot(libusb_device_handle *h, uint16_t anim_id, uint32_t color) {
    awelc_anim_op(h, ANIM_REMOVE, anim_id);
    awelc_anim_op(h, ANIM_START_NEW, anim_id);
    awelc_send_zone_color_actions(h, color);
    awelc_anim_op(h, ANIM_FINISH_SAVE, anim_id);
    awelc_anim_op(h, ANIM_SET_DEFAULT, anim_id);
}

static void awelc_play_temporary(libusb_device_handle *h, uint32_t color) {
    awelc_anim_op(h, ANIM_START_NEW, 0x0000);
    awelc_send_zone_color_actions(h, color);
    awelc_anim_op(h, ANIM_FINISH_PLAY, 0x0000);
}

static int awelc_set_dim(libusb_device_handle *h, int brightness) {
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;
    uint8_t dim = (uint8_t)(100 - brightness);
    uint8_t pkt[9] = {
        PREAMBLE, CMD_SET_DIM, dim,
        0x00, 0x04,
        ZONES_4[0], ZONES_4[1], ZONES_4[2], ZONES_4[3]
    };
    return awelc_send(h, pkt, sizeof(pkt));
}

static void notify_kde_osd(int brightness) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "busctl --user call org.kde.plasmashell /org/kde/osdService "
             "org.kde.osdService keyboardBrightnessChanged i %d >/dev/null 2>&1 &",
             brightness);
    int r = system(cmd);
    (void)r;
}

static void sync_upower_brightness(int brightness) {
    int level = (brightness == 0) ? 0 : (brightness <= 50 ? 1 : 2);
    if (read_sysfs_brightness_pct() == (level * 50)) return;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "busctl call org.freedesktop.UPower /org/freedesktop/UPower/KbdBacklight "
             "org.freedesktop.UPower.KbdBacklight SetBrightness i %d >/dev/null 2>&1 &",
             level);
    int r = system(cmd);
    (void)r;
}

static int apply_backlight(State *st, bool full_reprogram, bool keep_usb_open) {
    libusb_device_handle *h = awelc_acquire();
    if (!h) return -1;

    if (full_reprogram || !st->initialized) {
        awelc_clear_all_animations(h);
        awelc_set_dim(h, st->brightness);
        awelc_program_static_slot(h, ANIM_ID_AC_CHARGED, st->color);
        awelc_program_static_slot(h, ANIM_ID_AC_CHARGING, st->color);
        awelc_program_static_slot(h, ANIM_ID_DC_ON, st->color);
        awelc_program_static_slot(h, ANIM_ID_DEFAULT, st->color);
        awelc_play_temporary(h, st->color);
        st->initialized = 1;
    } else {
        awelc_set_dim(h, st->brightness);
    }

    awelc_release(keep_usb_open);
    save_state(st);
    return 0;
}

static int next_brightness_step(int current) {
    if (current == 0) return 50;
    if (current <= 50) return 100;
    return 0;
}

static double monotonic_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int run_daemon(void) {
    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    State st = load_state();
    apply_backlight(&st, !st.initialized, true);
    sync_upower_brightness(st.brightness);

    int kbd_fd = open(ATKBD_BY_PATH, O_RDONLY | O_NONBLOCK);
    struct libevdev *dev = NULL;
    if (kbd_fd < 0 || libevdev_new_from_fd(kbd_fd, &dev) != 0) {
        fprintf(stderr, "Failed to open keyboard input device %s\n", ATKBD_BY_PATH);
        if (kbd_fd >= 0) close(kbd_fd);
        awelc_release(false);
        return 1;
    }

    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        inotify_add_watch(inotify_fd, DELL_KBD_SYSFS, IN_MODIFY | IN_CLOSE_WRITE);
    }

    fprintf(stdout, "g15-kbd-backlight listening on '%s' (brightness=%d%%, color=#%06x)\n",
            libevdev_get_name(dev), st.brightness, st.color);
    fflush(stdout);

    double last_trigger = 0.0;
    struct pollfd pfds[2] = {
        {.fd = kbd_fd, .events = POLLIN, .revents = 0},
        {.fd = inotify_fd, .events = POLLIN, .revents = 0}
    };
    nfds_t nfds = (inotify_fd >= 0) ? 2 : 1;

    while (g_running) {
        int prc = poll(pfds, nfds, -1);
        if (prc < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            struct input_event ev;
            while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == 0) {
                if (ev.type == EV_MSC && ev.code == MSC_SCAN && ev.value == MSC_SCAN_LIGHT_KEY) {
                    double now = monotonic_sec();
                    if (now - last_trigger >= 0.12) {
                        last_trigger = now;
                        st = load_state();
                        st.brightness = next_brightness_step(st.brightness);
                        if (apply_backlight(&st, false, true) == 0) {
                            notify_kde_osd(st.brightness);
                        }
                    }
                }
            }
        }

        if (nfds > 1 && (pfds[1].revents & POLLIN)) {
            char ibuf[256];
            while (read(inotify_fd, ibuf, sizeof(ibuf)) > 0) {}
            /* If changed externally (e.g. KDE Brightness slider or PowerDevil idle dim), sync AW-ELC */
            if (monotonic_sec() - last_trigger >= 0.25) {
                int sys_pct = read_sysfs_brightness_pct();
                st = load_state();
                if (sys_pct >= 0 && st.brightness != sys_pct) {
                    st.brightness = sys_pct;
                    apply_backlight(&st, false, true);
                }
            }
        }
    }

    if (inotify_fd >= 0) close(inotify_fd);
    libevdev_free(dev);
    close(kbd_fd);
    awelc_release(false);
    return 0;
}

static void print_usage(const char *prog) {
    printf("Usage:\n"
           "  %s --daemon            Run Fn+F5 backlight daemon\n"
           "  %s cycle               Cycle brightness (0%% -> 50%% -> 100%% -> 0%%)\n"
           "  %s brightness <0-100>  Set keyboard brightness percentage\n"
           "  %s color <RRGGBB>      Set keyboard RGB hex color (e.g. ffffff or 00ffff)\n",
           prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        State st = load_state();
        printf("Current brightness: %d%%, color: #%06x\n", st.brightness, st.color);
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "-d") == 0 || strcmp(argv[1], "--daemon") == 0) {
        return run_daemon();
    }

    if (strcmp(argv[1], "cycle") == 0) {
        State st = load_state();
        st.brightness = next_brightness_step(st.brightness);
        if (apply_backlight(&st, false, false) != 0) return 1;
        sync_upower_brightness(st.brightness);
        notify_kde_osd(st.brightness);
        printf("Keyboard brightness: %d%%\n", st.brightness);
        return 0;
    }

    if (strcmp(argv[1], "brightness") == 0 && argc >= 3) {
        State st = load_state();
        st.brightness = atoi(argv[2]);
        if (st.brightness < 0) st.brightness = 0;
        if (st.brightness > 100) st.brightness = 100;
        if (apply_backlight(&st, false, false) != 0) return 1;
        sync_upower_brightness(st.brightness);
        notify_kde_osd(st.brightness);
        printf("Keyboard brightness set to %d%%\n", st.brightness);
        return 0;
    }

    if (strcmp(argv[1], "color") == 0 && argc >= 3) {
        State st = load_state();
        const char *hex = argv[2];
        if (hex[0] == '#') hex++;
        st.color = (uint32_t)strtoul(hex, NULL, 16) & 0xFFFFFF;
        if (apply_backlight(&st, true, false) != 0) return 1;
        notify_kde_osd(st.brightness);
        printf("Keyboard color set to #%06x (brightness %d%%)\n", st.color, st.brightness);
        return 0;
    }

    print_usage(argv[0]);
    return 1;
}
