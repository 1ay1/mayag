// Linux HID mover for photon measurement: a uinput relative mouse that wiggles
// the pointer at a fixed rate, so input reaches the app through the real
// kernel -> compositor -> client path.
//   uinput_mover [seconds] [hz]
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int val) {
    struct input_event ie;
    memset(&ie, 0, sizeof ie);
    ie.type = type; ie.code = code; ie.value = val;
    if (write(fd, &ie, sizeof ie) < 0) perror("write");
}

int main(int argc, char** argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 5.0;
    const int hz = argc > 2 ? atoi(argv[2]) : 125;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_USB; us.id.vendor = 0x1234; us.id.product = 0x5678;
    strcpy(us.name, "mayag photon mover");
    ioctl(fd, UI_DEV_SETUP, &us);
    ioctl(fd, UI_DEV_CREATE);
    usleep(400000);   // let the compositor pick the device up
    const long step_ns = 1000000000L / hz;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    const long total = (long)(secs * hz);
    for (long i = 0; i < total; ++i) {
        const int dx = (i / 20) % 2 ? -3 : 3;
        emit(fd, EV_REL, REL_X, dx);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        next.tv_nsec += step_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
