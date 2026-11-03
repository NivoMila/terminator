#define _POSIX_C_SOURCE 200809L

#include "key_config.h"

#include <X11/keysym.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *const action_names[SHORTCUT_COUNT] = {
    [SHORTCUT_NEW_TAB] = "new_tab",
    [SHORTCUT_CLOSE_TAB] = "close_tab",
    [SHORTCUT_NEXT_TAB] = "next_tab",
    [SHORTCUT_PREVIOUS_TAB] = "previous_tab",
    [SHORTCUT_INTERRUPT] = "interrupt",
    [SHORTCUT_SUSPEND] = "suspend",
    [SHORTCUT_COMPLETE] = "complete",
    [SHORTCUT_HISTORY_PREVIOUS] = "history_previous",
    [SHORTCUT_HISTORY_NEXT] = "history_next"
};

static char *
trim(char *text)
{
    char *end;
    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

void
key_config_defaults(KeyConfig *config)
{
    config->bindings[SHORTCUT_NEW_TAB] = (KeyBinding){XK_t, ControlMask};
    config->bindings[SHORTCUT_CLOSE_TAB] = (KeyBinding){XK_w, Mod1Mask};
    config->bindings[SHORTCUT_NEXT_TAB] = (KeyBinding){XK_Tab, ControlMask};
    config->bindings[SHORTCUT_PREVIOUS_TAB] =
        (KeyBinding){XK_Tab, ControlMask | ShiftMask};
    config->bindings[SHORTCUT_INTERRUPT] = (KeyBinding){XK_c, ControlMask};
    config->bindings[SHORTCUT_SUSPEND] = (KeyBinding){XK_z, ControlMask};
    config->bindings[SHORTCUT_COMPLETE] = (KeyBinding){XK_Tab, 0};
    config->bindings[SHORTCUT_HISTORY_PREVIOUS] = (KeyBinding){XK_Up, 0};
    config->bindings[SHORTCUT_HISTORY_NEXT] = (KeyBinding){XK_Down, 0};
}

static int
parse_chord(char *text, KeyBinding *binding)
{
    unsigned int modifiers = 0;
    int have_key = 0;
    char *save = NULL;
    char *part = strtok_r(text, "+", &save);
    KeySym key = NoSymbol;

    if (*text == '+' || text[strlen(text) - 1] == '+' || strstr(text, "++") != NULL)
        return -1;

    while (part != NULL) {
        part = trim(part);
        if (strcasecmp(part, "ctrl") == 0 || strcasecmp(part, "control") == 0) {
            if ((modifiers & ControlMask) != 0)
                return -1;
            modifiers |= ControlMask;
        } else if (strcasecmp(part, "shift") == 0) {
            if ((modifiers & ShiftMask) != 0)
                return -1;
            modifiers |= ShiftMask;
        } else if (strcasecmp(part, "alt") == 0) {
            if ((modifiers & Mod1Mask) != 0)
                return -1;
            modifiers |= Mod1Mask;
        } else {
            if (have_key)
                return -1;
            key = XStringToKeysym(part);
            if (key == NoSymbol && strlen(part) == 1)
                key = (KeySym)(unsigned char)part[0];
            if (key >= XK_A && key <= XK_Z)
                key = XK_a + (key - XK_A);
            if (key == NoSymbol)
                return -1;
            have_key = 1;
        }
        part = strtok_r(NULL, "+", &save);
    }
    if (!have_key)
        return -1;
    binding->key = key;
    binding->modifiers = modifiers;
    return 0;
}

static ShortcutAction
parse_action(const char *name)
{
    int i;
    for (i = 1; i < SHORTCUT_COUNT; ++i)
        if (strcmp(name, action_names[i]) == 0)
            return (ShortcutAction)i;
    return SHORTCUT_NONE;
}

static int
binding_reserved(KeyBinding binding)
{
    static const KeyBinding reserved[] = {
        {XK_a, ControlMask}, {XK_e, ControlMask},
        {XK_u, ControlMask}, {XK_k, ControlMask},
        {XK_w, ControlMask}, {XK_l, ControlMask},
        {XK_d, ControlMask}, {XK_r, ControlMask},
        {XK_c, ControlMask | ShiftMask},
        {XK_v, ControlMask | ShiftMask}
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        if (reserved[i].key == binding.key && reserved[i].modifiers == binding.modifiers)
            return 1;
    return 0;
}

static int
binding_conflicts(const KeyConfig *config, ShortcutAction action, KeyBinding binding)
{
    int i;
    if (binding_reserved(binding))
        return 1;
    for (i = 1; i < SHORTCUT_COUNT; ++i) {
        if (i != (int)action && config->bindings[i].key == binding.key &&
            config->bindings[i].modifiers == binding.modifiers)
            return 1;
    }
    return 0;
}

int
key_config_load(KeyConfig *config, const char *path)
{
    FILE *file;
    char *owned_path = NULL;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int accepted = 0;
    const char *home = getenv("HOME");

    key_config_defaults(config);
    if (path == NULL) {
        if (home == NULL || *home == '\0')
            return 0;
        owned_path = malloc(strlen(home) + sizeof("/.terminatorrc"));
        if (owned_path == NULL)
            return -1;
        (void)sprintf(owned_path, "%s/.terminatorrc", home);
        path = owned_path;
    }
    file = fopen(path, "r");
    free(owned_path);
    if (file == NULL)
        return 0;
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *entry = trim(line);
        char *comment = strchr(entry, '#');
        char *equals;
        char *action_name;
        char *chord;
        ShortcutAction action;
        KeyBinding binding;
        if (comment != NULL)
            *comment = '\0';
        entry = trim(entry);
        if (*entry == '\0')
            continue;
        equals = strchr(entry, '=');
        if (equals == NULL) {
            fprintf(stderr, "terminator: ignoring invalid shortcut entry: %s\n", entry);
            continue;
        }
        *equals = '\0';
        action_name = trim(entry);
        chord = trim(equals + 1);
        action = parse_action(action_name);
        if (action == SHORTCUT_NONE || *chord == '\0' ||
            parse_chord(chord, &binding) < 0 ||
            binding_conflicts(config, action, binding)) {
            fprintf(stderr, "terminator: ignoring invalid or conflicting shortcut: %s\n",
                    action_name);
            continue;
        }
        config->bindings[action] = binding;
        ++accepted;
    }
    free(line);
    if (ferror(file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return accepted;
}

ShortcutAction
key_config_match(const KeyConfig *config, KeySym key, unsigned int modifiers)
{
    int i;
    unsigned int relevant = ControlMask | ShiftMask | Mod1Mask;
    if (key >= XK_A && key <= XK_Z)
        key = XK_a + (key - XK_A);
    modifiers &= relevant;
    for (i = 1; i < SHORTCUT_COUNT; ++i)
        if (config->bindings[i].key == key &&
            config->bindings[i].modifiers == modifiers)
            return (ShortcutAction)i;
    return SHORTCUT_NONE;
}
