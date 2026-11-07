/* Build with the shell's PKGS and -DWLR_USE_UNSTABLE; run without a display. */
#define main ztmetro_main
#include "../ztmetro.c"
#undef main
#include <assert.h>

static void hold_dot(struct zt_server *s, int id) {
    double x = osk_dot_x(s);
    double y = s->height - NAV_H - s->osk_h + osk_dot_y(s);
    touch_begin(s, id, x, y, now_ms());
    assert(s->osk_cursor_touch && !s->osk_trackpad);
    s->osk_cursor_touch->down_ms = now_ms() - LONG_PRESS_MS;
    long_press_fired(s);
    assert(s->osk_trackpad && s->osk_pressed == -1 && !s->keypop);
}

int main(void) {
    struct zt_server s = { .width = 360, .height = 740, .scale = 1,
        .screen_on = true, .osk_visible = true, .osk_pressed = -1, .key_height = 56 };
    s.wl_display = wl_display_create();
    s.loop = wl_display_get_event_loop(s.wl_display);
    s.long_press_timer = wl_event_loop_add_timer(s.loop, long_press_fired, &s);
    s.seat = wlr_seat_create(s.wl_display, "test");
    wl_list_init(&s.toplevels);
    wlr_keyboard_init(&s.vkbd, &vkbd_impl, "test");
    set_default_keymap(&s.vkbd);
    for (int height = 42; height <= 72; height++) {
        s.key_height = height;
        for (int terminal = 0; terminal < 2; terminal++) {
            s.osk_terminal = terminal;
            osk_layout(&s);
            int base = terminal ? 8 : 0;
            assert(s.n_keys == base + 34);
            for (int col = 0; col < 9; col++) {
                assert(fabs(s.keys[base+col].w - s.keys[base+10+col].w) < 1e-8);
                assert(fabs(s.keys[base+col].h - s.keys[base+10+col].h) < 1e-8);
                if (col > 0 && col < 8) {
                    assert(fabs(s.keys[base+10+col].x - s.keys[base+19+col].x) < 1e-8);
                    assert(fabs(s.keys[base+10+col].w - s.keys[base+19+col].w) < 1e-8);
                }
            }
            assert(s.keys[base+19].x == KEY_GAP);
            assert(s.keys[base+28].x == KEY_GAP);
            int bottom_indices[] = {29, 30, 32};
            int aligned_indices[] = {20, 21, 26}; /* Z, X, M */
            for (int i = 0; i < 3; i++) {
                struct osk_key *key = &s.keys[base+bottom_indices[i]];
                struct osk_key *top = &s.keys[base+aligned_indices[i]];
                assert(fabs(key->x-top->x) < 1e-8);
                assert(fabs(key->w-top->w) < 1e-8);
                assert(fabs(key->h-top->h) < 1e-8);
            }
            assert(s.keys[base+31].w > 4*s.keys[base].w); /* full span between comma and dot */
            assert(s.keys[base+19].w > s.keys[base].w); /* shift */
            assert(s.keys[base+27].w > s.keys[base].w); /* backspace */
            assert(fabs(s.keys[base+27].x+s.keys[base+27].w-(s.width-KEY_GAP)) < 1e-8);
            assert(fabs(s.keys[s.n_keys-1].x+s.keys[s.n_keys-1].w-(s.width-KEY_GAP)) < 1e-8);
        }
    }
    s.osk_terminal = false; s.key_height = 56; osk_layout(&s);
    /* A number pad: the same height, whatever the settings say, and no dot. */
    static const enum osk_mode pads[] = { OSK_MODE_DIGITS, OSK_MODE_NUMBER, OSK_MODE_PHONE };
    double letters_h = s.osk_h;
    for (int i = 0; i < 3; i++) {
        s.osk_field_mode = pads[i];
        osk_mode_refresh(&s);
        assert(s.osk_mode == pads[i] && s.osk_h == letters_h && !s.osk_cursor_touch);
        touch_begin(&s, 3, osk_dot_x(&s), s.height - NAV_H - s.osk_h + osk_dot_y(&s), now_ms());
        assert(!s.osk_cursor_touch && s.osk_pressed >= 0);
        /* the field changes under the finger: it presses nothing when it lifts */
        s.osk_field_mode = pads[(i + 1) % 3];
        osk_mode_refresh(&s);
        assert(s.osk_pressed == -1);
        touch_end(&s, 3, now_ms(), false);
        /* the letters and back, where the pad has the key */
        if (s.osk_mode != OSK_MODE_DIGITS) {
            for (int k = 0; k < s.n_keys; k++) {
                if (s.keys[k].action != ACT_PAGE) continue;
                osk_activate(&s, k);
                break;
            }
            assert(s.osk_mode == OSK_MODE_FULL && s.osk_letters);
            int page = -1;
            for (int k = 0; k < s.n_keys; k++) {
                if (s.keys[k].action == ACT_PAGE) page = k;
            }
            assert(page >= 0 && !strcmp(s.keys[page].label, "123"));
            osk_activate(&s, page);
            assert(s.osk_mode == s.osk_wanted && !s.osk_letters);
        }
    }
    s.osk_field_mode = OSK_MODE_FULL;
    osk_mode_refresh(&s);
    assert(s.osk_mode == OSK_MODE_FULL && s.osk_h == letters_h);
    hold_dot(&s, 1);
    struct touch_point *p = s.osk_cursor_touch;
    double x = p->x, y = p->y;
    s.osk_ctrl = s.osk_alt = true;
    touch_update(&s, 1, x+6, y+8, now_ms());
    assert(p->lx == x && p->ly == y);
    touch_update(&s, 1, x+25, y+20, now_ms());
    assert(p->lx == x+20 && p->ly == y+18);
    assert(s.osk_ctrl && s.osk_alt);
    touch_begin(&s, 2, 180, y, now_ms());
    assert(touch_find(&s, 2, false)->role == ROLE_NONE);
    touch_end(&s, 2, now_ms(), false);
    assert(s.osk_trackpad);
    touch_end(&s, 1, now_ms(), false);
    assert(!s.osk_trackpad && !s.osk_cursor_touch && s.osk_pressed == -1);
    hold_dot(&s, 3);
    touch_end(&s, 3, now_ms(), true);
    assert(!s.osk_trackpad && !s.osk_cursor_touch);
    x = osk_dot_x(&s); y = s.height-NAV_H-s.osk_h+osk_dot_y(&s);
    touch_begin(&s, 4, x, y, now_ms());
    touch_end(&s, 4, now_ms(), false);
    assert(!s.osk_trackpad && !s.osk_cursor_touch);
    touch_begin(&s, 5, x, y, now_ms());
    touch_update(&s, 5, x+30, y, now_ms());
    s.osk_cursor_touch->down_ms = now_ms()-LONG_PRESS_MS;
    long_press_fired(&s);
    assert(s.osk_trackpad);	/* a drag off the dot starts it too, without the hold */
    touch_end(&s, 5, now_ms(), false);
    assert(!s.osk_trackpad);
    hold_dot(&s, 6);
    osk_show(&s, false);
    assert(!s.osk_trackpad && !s.osk_cursor_touch);
    touch_end(&s, 6, now_ms(), false);
    puts("PASS: aligned rows at all heights, hold, fractional cursor movement, modifiers, extra finger, release, cancel, short tap, drag off the dot, keyboard hide, number pads");
    return 0;
}
