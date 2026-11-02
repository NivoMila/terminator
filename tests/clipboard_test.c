#define _POSIX_C_SOURCE 200809L

#include "xclipboard.h"

#include <X11/Xatom.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    char text[256];
    size_t length;
} PasteResult;

static void
capture_paste(void *context, const char *text, size_t length)
{
    PasteResult *result = context;
    assert(length < sizeof(result->text));
    memcpy(result->text, text, length);
    result->text[length] = '\0';
    result->length = length;
}

int
main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        puts("clipboard tests skipped (no X display)");
        return 0;
    }
    Window owner = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                       0, 0, 1, 1, 0, 0, 0);
    Window requestor = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                           0, 0, 1, 1, 0, 0, 0);
    XClipboard clipboard;
    PasteResult result = {{0}, 0};
    xclipboard_init(display, &clipboard);
    XSelectInput(display, owner, PropertyChangeMask);
    XSelectInput(display, requestor, PropertyChangeMask);
    assert(xclipboard_copy(display, owner, &clipboard, "clipboard round trip", 20) == 0);
    xclipboard_paste(display, requestor, &clipboard);

    /* Both windows are owned by this test process. Drain X selection events
       to exercise SelectionRequest, SelectionNotify and property retrieval. */
    for (int attempts = 0; attempts < 1000 && result.length == 0; ++attempts) {
        XEvent event;
        if (XPending(display) == 0) {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
            XFlush(display);
            (void)nanosleep(&delay, NULL);
            continue;
        }
        XNextEvent(display, &event);
        (void)xclipboard_handle_event(display, owner, &clipboard, &event,
                                      NULL, NULL);
        (void)xclipboard_handle_event(display, requestor, &clipboard, &event,
                                      capture_paste, &result);
    }
    assert(strcmp(result.text, "clipboard round trip") == 0);

    xclipboard_destroy(&clipboard);
    XDestroyWindow(display, requestor);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    puts("clipboard X11 selection tests passed");
    return 0;
}
