// Moves the pointer in a circle around the main display's centre via real
// HID-level CGEvents, for `secs` seconds at ~250 Hz (a trackpad's rate).
#include <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    double secs = argc > 1 ? atof(argv[1]) : 2.0;
    CGRect b = CGDisplayBounds(CGMainDisplayID());
    double cx = b.origin.x + b.size.width / 2, cy = b.origin.y + b.size.height / 2;
    int n = (int)(secs * 250);
    for (int i = 0; i < n; ++i) {
        double a = i * 0.05;
        CGPoint p = CGPointMake(cx + 120 * cos(a), cy + 80 * sin(a));
        CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
        CGEventPost(kCGHIDEventTap, e);
        CFRelease(e);
        usleep(4000);
    }
    return 0;
}
