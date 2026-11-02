#define _POSIX_C_SOURCE 200809L

#include "xclipboard.h"

#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>

enum { CLIPBOARD_MAX_LONGS = 262144 };

void
xclipboard_init(Display *display, XClipboard *clipboard)
{
    memset(clipboard, 0, sizeof(*clipboard));
    clipboard->clipboard = XInternAtom(display, "CLIPBOARD", False);
    clipboard->targets = XInternAtom(display, "TARGETS", False);
    clipboard->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    clipboard->property = XInternAtom(display, "_TERMINATOR_CLIPBOARD", False);
}

void
xclipboard_destroy(XClipboard *clipboard)
{
    free(clipboard->owned_text);
    memset(clipboard, 0, sizeof(*clipboard));
}

int
xclipboard_copy(Display *display, Window owner, XClipboard *clipboard,
                const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy == NULL)
        return -1;
    memcpy(copy, text, length);
    copy[length] = '\0';
    free(clipboard->owned_text);
    clipboard->owned_text = copy;
    clipboard->owned_length = length;
    XSetSelectionOwner(display, clipboard->clipboard, owner, CurrentTime);
    XFlush(display);
    return XGetSelectionOwner(display, clipboard->clipboard) == owner ? 0 : -1;
}

void
xclipboard_paste(Display *display, Window requestor, XClipboard *clipboard)
{
    clipboard->paste_fallback = 0;
    XDeleteProperty(display, requestor, clipboard->property);
    XConvertSelection(display, clipboard->clipboard, clipboard->utf8_string,
                      clipboard->property, requestor, CurrentTime);
    XFlush(display);
}

static void
selection_notify(Display *display, const XSelectionRequestEvent *request,
                 Atom property)
{
    XEvent response;
    memset(&response, 0, sizeof(response));
    response.xselection.type = SelectionNotify;
    response.xselection.display = request->display;
    response.xselection.requestor = request->requestor;
    response.xselection.selection = request->selection;
    response.xselection.target = request->target;
    response.xselection.property = property;
    response.xselection.time = request->time;
    XSendEvent(display, request->requestor, False, NoEventMask, &response);
}

static void
serve_selection(Display *display, XClipboard *clipboard,
                const XSelectionRequestEvent *request)
{
    Atom property = request->property == None ? request->target : request->property;
    if (request->target == clipboard->targets) {
        Atom targets[] = {clipboard->targets, clipboard->utf8_string, XA_STRING};
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)targets,
                        (int)(sizeof(targets) / sizeof(targets[0])));
    } else if (request->target == clipboard->utf8_string || request->target == XA_STRING) {
        XChangeProperty(display, request->requestor, property, request->target, 8,
                        PropModeReplace, (const unsigned char *)clipboard->owned_text,
                        (int)clipboard->owned_length);
    } else {
        property = None;
    }
    selection_notify(display, request, property);
}

int
xclipboard_handle_event(Display *display, Window window,
                        XClipboard *clipboard, XEvent *event,
                        XClipboardPaste paste, void *context)
{
    if (event->type == SelectionRequest && event->xselectionrequest.owner == window &&
        event->xselectionrequest.selection == clipboard->clipboard) {
        serve_selection(display, clipboard, &event->xselectionrequest);
        XFlush(display);
        return 1;
    }
    if (event->type == SelectionClear && event->xselectionclear.window == window &&
        event->xselectionclear.selection == clipboard->clipboard) {
        free(clipboard->owned_text);
        clipboard->owned_text = NULL;
        clipboard->owned_length = 0;
        return 1;
    }
    if (event->type == SelectionNotify && event->xselection.requestor == window &&
        event->xselection.selection == clipboard->clipboard &&
        (event->xselection.target == clipboard->utf8_string ||
         (clipboard->paste_fallback && event->xselection.target == XA_STRING))) {
        if (event->xselection.property == None) {
            if (event->xselection.target == clipboard->utf8_string && !clipboard->paste_fallback) {
                clipboard->paste_fallback = 1;
                XConvertSelection(display, clipboard->clipboard, XA_STRING,
                                 clipboard->property, window, event->xselection.time);
                XFlush(display);
            }
            return 1;
        }
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long item_count = 0, bytes_after = 0;
        unsigned char *data = NULL;
        int result = XGetWindowProperty(display, window, clipboard->property,
                                        0, CLIPBOARD_MAX_LONGS, True, AnyPropertyType,
                                        &actual_type, &actual_format, &item_count,
                                        &bytes_after, &data);
        if (result == Success && data != NULL && actual_format == 8 &&
            (actual_type == clipboard->utf8_string || actual_type == XA_STRING) && paste != NULL)
            paste(context, (const char *)data, (size_t)item_count);
        if (data != NULL)
            XFree(data);
        return 1;
    }
    return 0;
}
