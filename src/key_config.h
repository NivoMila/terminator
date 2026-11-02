#ifndef TERMINATOR_KEY_CONFIG_H
#define TERMINATOR_KEY_CONFIG_H

#include <X11/Xlib.h>

typedef enum {
    SHORTCUT_NONE,
    SHORTCUT_NEW_TAB,
    SHORTCUT_CLOSE_TAB,
    SHORTCUT_NEXT_TAB,
    SHORTCUT_PREVIOUS_TAB,
    SHORTCUT_INTERRUPT,
    SHORTCUT_SUSPEND,
    SHORTCUT_COMPLETE,
    SHORTCUT_HISTORY_PREVIOUS,
    SHORTCUT_HISTORY_NEXT,
    SHORTCUT_COUNT
} ShortcutAction;

typedef struct {
    KeySym key;
    unsigned int modifiers;
} KeyBinding;

typedef struct {
    KeyBinding bindings[SHORTCUT_COUNT];
} KeyConfig;

void key_config_defaults(KeyConfig *config);
int key_config_load(KeyConfig *config, const char *path);
ShortcutAction key_config_match(const KeyConfig *config,
                                KeySym key, unsigned int modifiers);

#endif
