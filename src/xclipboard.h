#ifndef TERMINATOR_XCLIPBOARD_H
#define TERMINATOR_XCLIPBOARD_H

#include <X11/Xlib.h>
#include <stddef.h>

typedef struct {
    Atom clipboard;
    Atom targets;
    Atom utf8_string;
    Atom property;
    char *owned_text;
    size_t owned_length;
    int paste_fallback;
} XClipboard;

typedef void (*XClipboardPaste)(void *context, const char *text, size_t length);

void xclipboard_init(Display *display, XClipboard *clipboard);
void xclipboard_destroy(XClipboard *clipboard);
int xclipboard_copy(Display *display, Window owner, XClipboard *clipboard,
                    const char *text, size_t length);
void xclipboard_paste(Display *display, Window requestor, XClipboard *clipboard);
int xclipboard_handle_event(Display *display, Window window,
                            XClipboard *clipboard, XEvent *event,
                            XClipboardPaste paste, void *context);

#endif
