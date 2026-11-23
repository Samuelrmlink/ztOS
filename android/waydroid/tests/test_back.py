"""Back on an Android app is Android's Back (ROADMAP 8.63), checked on the host.

Compiles the shell's real waydroid_window, vkbd_send and app_back into a
harness with stubbed wlroots calls, and checks the key events each kind of
app in front gets from Back, and that every Back path goes through app_back.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = (ROOT / 'ztmetro/ztmetro.c').read_text()


def cut(start, end):
    i = SOURCE.index(start)
    return SOURCE[i:SOURCE.index(end, i)]


def body(name):
    """The text of a C function, from its definition to the closing brace at column 0."""
    i = SOURCE.index(name)
    return SOURCE[i:SOURCE.index('\n}\n', i)]


class BackTests(unittest.TestCase):
    def test_back_events_per_app(self):
        program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <linux/input.h>
#define XKB_MOD_NAME_SHIFT "Shift"
#define XKB_MOD_NAME_CTRL "Control"
#define XKB_MOD_NAME_ALT "Mod1"
#define WL_KEYBOARD_KEY_STATE_PRESSED 1
#define WL_KEYBOARD_KEY_STATE_RELEASED 0
struct xkb_keymap { int unused; };
struct wlr_keyboard_modifiers { uint32_t depressed; };
struct keyboard { struct xkb_keymap *keymap; };
struct xdg { const char *app_id; };
struct zt_toplevel { struct xdg *xdg_toplevel; bool android_runtime, background_hidden, background; };
struct zt_server {
    struct keyboard vkbd;
    void *seat;
    bool osk_ctrl, osk_alt, osk_visible;
    struct zt_toplevel *shown;
};
static unsigned codes[16], states[16], count, osk_hides, hides;
static uint32_t now_ms(void) { return 1; }
static unsigned xkb_keymap_mod_get_index(struct xkb_keymap *map, const char *name) { return 0; }
static void wlr_seat_set_keyboard(void *seat, struct keyboard *keyboard) {}
static void wlr_seat_keyboard_notify_modifiers(void *seat, struct wlr_keyboard_modifiers *m) {}
static void wlr_seat_keyboard_notify_key(void *seat, uint32_t t, unsigned code, unsigned state) {
    assert(count < 16); codes[count] = code; states[count++] = state;
}
static void osk_show(struct zt_server *s, bool show) { assert(!show); s->osk_visible = false; osk_hides++; }
static void show_toplevel(struct zt_server *s, struct zt_toplevel *t) { assert(!t); s->shown = NULL; hides++; }
static bool background_app(struct zt_toplevel *t) { return t->background; }
'''
        program += cut('static bool waydroid_window(', '\nstatic void waydroid_visible(')
        program += cut('static void vkbd_send(', '\n/* Text-input clients')
        program += body('static void app_back(') + '\n}\n'
        program += r'''
static void run(const char *id, bool atl, bool background, bool osk) {
    struct xdg xdg = { .app_id = id };
    struct zt_toplevel top = { .xdg_toplevel = &xdg, .android_runtime = atl, .background = background };
    struct zt_server s = { .shown = &top, .osk_visible = osk };
    count = osk_hides = hides = 0;
    app_back(&s);
}
int main(void) {
    /* Android: KEY_BACK (158 -> KEYCODE_BACK) and nothing else, the window
     * left alone, a background app or not, keyboard up or not */
    const char *android[] = { "waydroid.com.spotify.music", "waydroid.org.example", "Waydroid" };
    for (unsigned a = 0; a < 3; a++) {
        for (unsigned m = 0; m < 4; m++) {
            run(android[a], m & 1, m & 2, false);
            assert(count == 2 && codes[0] == KEY_BACK && states[0] == 1 &&
                   codes[1] == KEY_BACK && states[1] == 0);
            assert(hides == 0 && osk_hides == 0);
            run(android[a], m & 1, m & 2, true);
            assert(count == 2 && codes[0] == KEY_BACK && codes[1] == KEY_BACK);
            assert(hides == 0 && osk_hides == 1);
        }
    }
    assert(KEY_BACK == 158);
    /* Linux apps: Escape */
    run("ztcontacts", false, false, false);
    assert(count == 2 && codes[0] == KEY_ESC && codes[1] == KEY_ESC && hides == 0);
    run("ztcontacts", false, false, true);
    assert(count == 2 && codes[0] == KEY_ESC && hides == 0);
    /* a Linux background app: hidden, no key */
    run("ztmusic", false, true, false);
    assert(count == 0 && hides == 1);
    /* ATL: the keyboard goes, no key; without it, Escape */
    run("org.example.atl", true, false, true);
    assert(count == 0 && osk_hides == 1 && hides == 0);
    run("org.example.atl", true, false, false);
    assert(count == 2 && codes[0] == KEY_ESC);
    run("org.example.atl", true, true, false);
    assert(count == 0 && hides == 1);
    /* not Waydroid by name */
    run("waydroidish", false, false, false);
    assert(codes[0] == KEY_ESC);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='.back-', dir=Path(__file__).parent) as directory:
            work = Path(directory)
            path = work / 'back.c'
            path.write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-parameter',
                            '-Wno-unused-function', str(path), '-o', str(work / 'back')], check=True)
            subprocess.run([str(work / 'back')], check=True)

    def test_every_back_path_uses_app_back(self):
        nav = body('static void nav_activate(')
        back = nav[nav.index('} else if (index == NAV_BACK) {'):]
        self.assertIn('app_back(server);', back)
        self.assertNotIn('KEY_ESC', back)
        self.assertNotIn('background_hidden', back)
        ctl = SOURCE[SOURCE.index('strcmp(line, "back") == 0'):SOURCE.index('strcmp(line, "restart") == 0')]
        self.assertIn('app_back(server);', ctl)
        self.assertNotIn('KEY_ESC', ctl)
        # the left-edge swipe: Android gets Back before any close or hide
        close = SOURCE[SOURCE.index('\tcase ROLE_CLOSE:'):SOURCE.index('\tcase ROLE_NAV:')]
        android = close.index('waydroid_window(server->shown->xdg_toplevel->app_id)')
        self.assertLess(android, close.index('app_back(server);'))
        self.assertLess(close.index('app_back(server);'), close.index('wlr_xdg_toplevel_send_close'))
        self.assertRegex(close, re.compile(r'app_back\(server\);\s*break;'))


if __name__ == '__main__':
    unittest.main()
