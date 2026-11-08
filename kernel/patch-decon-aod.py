#!/usr/bin/env python3
"""Blank without darkening the panel: the always-on display's stage 2.

  patch-decon-aod.py <kernel tree>

NOT in build-boot.sh yet: prepared for power/aod/README.md's stage 2, untested.

With the module parameter aod_keep=Y (default N, so nothing changes until
it is set), disabling the pipeline leaves the panel as it is: no black frame,
no display-off, no sleep-in. The last frame stays in the panel's own memory
and on the glass. The hardware trigger is masked and stays masked, so DECON
starts no frame transfer on a TE edge while the pipeline is "off": the panel
is awake and its TE keeps coming, and a transfer starting as the SoC goes to
sleep is a new way to hang it (7.4 report, "TE keeps running").

The output then reads disabled, so zrn-sleepd may put the SoC to sleep with
the time still showing; whether the panel keeps its picture through that
sleep (its supplies, the DSIM lanes in an unplanned state) is the stage-2
experiment, not a known.

Enabling again skips sleep-out (the panel never slept); the rest is as
before: the client's frame is programmed (which unmasks the trigger), the
brightness sent, display-on (already on: harmless). aod_keep is read once,
at the blank; changing it while blanked takes effect at the next blank.

Only display-off and sleep-in are left out here; no panel command is added.
Applied after patch-decon-panel-io.py.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1]) / "drivers/gpu/drm/s9p/s9p-decon.c"
s = path.read_text()
if "zboot: aod keep" in s:
    print("patch-decon-aod: already applied")
    sys.exit(0)

edits = [
    ("""MODULE_PARM_DESC(panel_sleep, "also put the panel into sleep mode when blanking");
""", """MODULE_PARM_DESC(panel_sleep, "also put the panel into sleep mode when blanking");

/* zboot: aod keep. The always-on display's blank: the panel keeps its
 * picture and stays awake, the trigger stays masked (patch-decon-aod.py). */
static bool s9p_aod_keep;
module_param_named(aod_keep, s9p_aod_keep, bool, 0644);
MODULE_PARM_DESC(aod_keep, "blank without darkening the panel (always-on display)");
"""),
    ("""	unsigned long bl_on_at;		/* jiffies of the last display-on */
};""", """	unsigned long bl_on_at;		/* jiffies of the last display-on */
	bool kept;			/* blanked by aod_keep: the panel is awake */
};"""),
    ("""	mutex_lock(&priv->panel_lock);
	s9p_panel_up(priv);
	obj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);""", """	mutex_lock(&priv->panel_lock);
	if (priv->kept)
		priv->kept = false;	/* awake all along: no sleep-out */
	else
		s9p_panel_up(priv);
	obj = to_drm_gem_dma_obj(plane_state->fb->obj[0]);"""),
    ("""	if (!priv->black_dma)
		return;
	if (s9p_wait_shadow_idle(priv))
		return;
	s9p_decon_program_address(priv, priv->black_dma);""", """	if (s9p_aod_keep) {
		/* the last flip has latched (pipe_update waited for it); one
		 * refresh more for it to reach the panel, then no more */
		s9p_wait_shadow_idle(priv);
		msleep(PANEL_FRAME_DELAY_MS);
		s9p_hw_trigger_mask(priv);
		priv->kept = true;
		return;
	}
	if (!priv->black_dma)
		return;
	if (s9p_wait_shadow_idle(priv))
		return;
	s9p_decon_program_address(priv, priv->black_dma);"""),
]
for old, new in edits:
    if s.count(old) != 1:
        sys.exit("patch-decon-aod: the driver changed near:\n" + old[:80])
    s = s.replace(old, new)
path.write_text(s)
print("patch-decon-aod: applied")
