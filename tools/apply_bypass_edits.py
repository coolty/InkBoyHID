#!/usr/bin/env python3
"""Start a game without a controller: long press BOOT on the "turn on a gamepad" screen.

Run from the repository root AFTER apply_hid_edits.py (its text in app.c is the anchor here). All or nothing."""
import sys
from pathlib import Path

EDITS = {}


def edit(path, old, new):
    EDITS.setdefault(path, []).append((old, new))


edit('main/game_session.c',
'''        if (!input_wait_controller()) return false;
        if (!input_mapping_valid() && !input_calibrate()) return false;''',
'''        if (!input_wait_controller_or_bypass()) return false;
        if (!input_mapping_valid() && !input_bypassed() && !input_calibrate()) return false;''')

edit('main/app.c',
'''        if (play && !screen_failed() && input_wait_controller()) {
            /* Resume has no mapping check of its own, and a different pad may be connected now. */
            if (!input_mapping_valid() && !input_calibrate()) continue;''',
'''        if (play && !screen_failed() && input_wait_controller_or_bypass()) {
            /* Resume has no mapping check of its own, and a different pad may be connected now. */
            if (!input_mapping_valid() && !input_bypassed() && !input_calibrate()) continue;''')
edit('main/app.c',
'''    while (!screen_failed()) {
        const char *items[] = {"RESUME GAME", "GAMES", "SETTINGS", "POWER OFF"};''',
'''    while (!screen_failed()) {
        input_set_bypass(false); /* Playing without a controller is chosen per game start. */
        const char *items[] = {"RESUME GAME", "GAMES", "SETTINGS", "POWER OFF"};''')

edit('tools/test_hardware_input.py',
     'bool controller_connected(void) { return true; }',
     'static bool pad_connected = true;\nbool controller_connected(void) { return pad_connected; }')
edit('tools/test_hardware_input.py',
     '    if (!strncmp(title,"PRESS ",6)) {',
'''    if (!strcmp(title,"ON / CONNECT")) {
        ++wait_messages;
        if (inject_pin >= 0) { sample(inject_pin,0,1); sample(inject_pin,0,30000); sample(inject_pin,0,700000); } /* A hold. */
    }
    if (!strncmp(title,"PRESS ",6)) {''')
edit('tools/test_hardware_input.py',
     'static bool raw_pending;',
     'static bool raw_pending;\nstatic unsigned wait_messages;\nstatic int inject_pin = -1;')
edit('tools/test_hardware_input.py',
     '    } else if(!strcmp(argv[1],"console")) {',
'''    } else if(!strcmp(argv[1],"bypass")) {
        pad_connected=false; inject_pin=0;           /* A long press of BOOT plays without a controller. */
        assert(input_wait_controller_or_bypass() && input_bypassed() && input_connected() && wait_messages==1);
        sample(0,1,1); sample(0,1,30000); input_clear_events();
        assert(input_wait_controller_or_bypass() && wait_messages==1); /* Chosen once per game start. */
        input_set_bypass(false); assert(!input_connected() && !input_bypassed());
        inject_pin=18;                               /* A long press of PWR still returns to the menu. */
        assert(!input_wait_controller_or_bypass() && !input_bypassed() && wait_messages==2);
        sample(18,1,1); sample(18,1,30000); input_clear_events();
        inject_pin=-1; pad_connected=true; input_set_bypass(true);     /* A connected controller wins. */
        assert(input_wait_controller_or_bypass() && !input_bypassed() && wait_messages==2);
    } else if(!strcmp(argv[1],"console")) {''')
edit('tools/test_hardware_input.py',
     "['gestures','console','console_continuous','map','cancel','failure']",
     "['gestures','console','console_continuous','map','cancel','failure','bypass']")

edit('README.md',
     'Press **BOOT or PWR** to pause, save and return Home.',
     'Press **BOOT or PWR** to pause, save and return Home. If no controller is connected when you start a game, hold **BOOT** '
     'on the connect screen to play without one; the game runs and only the board buttons work.')


def main():
    root = Path.cwd()
    results, problems = {}, []
    for path, edits in EDITS.items():
        file = root / path
        if not file.exists():
            problems.append(f'{path}: file not found (run from the repository root)')
            continue
        text = file.read_text()
        for old, new in edits:
            first = old.splitlines()[0][:70]
            if new in text:
                problems.append(f'{path}: already applied: {first!r}')
            elif text.count(old) != 1:
                problems.append(f'{path}: expected exactly one match, found {text.count(old)}: {first!r}')
            else:
                text = text.replace(old, new)
        results[path] = text
    if problems:
        print('Nothing was changed:\n  ' + '\n  '.join(problems))
        return 1
    for path, text in results.items():
        (root / path).write_text(text)
        print(f'updated {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
