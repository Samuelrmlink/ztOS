#!/usr/bin/env python3
"""brcmfmac: tell the host when the BCM4361's firmware roams, the way Samsung's
driver (bcmdhd_100_15, wl_cfg80211.c) does.

Mainline reports a firmware roam only on E_ROAM with status success. Samsung's
driver turns E_ROAM off and reports the roam from E_LINK (link up while already
connected), or from E_BSSID. A roam the host never hears about leaves
wpa_supplicant on the old BSSID: the new AP's 4-way handshake can't complete
and 2.4 GHz goes silent (ROADMAP 1.7).

This adds, each one a writable parameter of the brcmfmac module:

  roam_link        report E_LINK up from a new BSSID while connected, and
                   E_BSSID success, as a roam (Samsung's path; E_ROAM still
                   counts). The same BSSID is only reported once.
  roam_eapol_hold  hold EAPOL from a BSSID the host isn't on until the roam is
                   reported, then pass it up. Samsung drops M1 in that window
                   (DHD_POST_EAPOL_M1_AFTER_ROAM_EVT). If no event reports the
                   roam, the firmware's BSSID is read and, if it is the frame's
                   sender, the roam is reported from that.
  roam_log         log the firmware's connection and roam events.

Usage: patch-brcmfmac-roam.py <kernel tree>
"""
from pathlib import Path
import sys

D = Path(sys.argv[1]) / 'drivers/net/wireless/broadcom/brcm80211/brcmfmac'
MARK = 'zboot: firmware roam'


def edit(name, pairs):
    p = D / name
    s = p.read_text()
    if MARK in s:
        return
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit(f'patch-brcmfmac-roam: {name}: anchor changed:\n{old}')
        s = s.replace(old, new, 1)
    p.write_text(s)


# --- cfg80211.h: state and the rx hook -------------------------------------
edit('cfg80211.h', [(
    '\tstruct completion vif_disabled;\n',
    '\tstruct completion vif_disabled;\n'
    '\t/* zboot: firmware roam: EAPOL held until the roam is reported */\n'
    '\tstruct mutex roam_lock;\n'
    '\tstruct sk_buff *roam_eapol;\n'
    '\tunsigned long roam_eapol_at;\n'
    '\tbool roam_eapol_seen;\n'
    '\tstruct delayed_work roam_work;\n',
), (
    'void brcmf_cfg80211_detach(struct brcmf_cfg80211_info *cfg);\n',
    'void brcmf_cfg80211_detach(struct brcmf_cfg80211_info *cfg);\n'
    '/* zboot: firmware roam */\n'
    'bool brcmf_cfg80211_roam_eapol(struct brcmf_if *ifp, struct sk_buff *skb);\n',
)])

# --- cfg80211.c --------------------------------------------------------------
HELPERS = r'''
/* zboot: firmware roam reporting, after Samsung's bcmdhd (see
 * kernel/patch-brcmfmac-roam.py in ztOS).
 */
static bool brcmf_roam_link = true;
module_param_named(roam_link, brcmf_roam_link, bool, 0644);
MODULE_PARM_DESC(roam_link, "Report E_LINK up / E_BSSID while connected as a roam");

static bool brcmf_roam_eapol_hold = true;
module_param_named(roam_eapol_hold, brcmf_roam_eapol_hold, bool, 0644);
MODULE_PARM_DESC(roam_eapol_hold, "Hold EAPOL from a new BSSID until the roam is reported");

#define BRCMF_FW_E_BSSID	125	/* WLC_E_BSSID: BSSID changed while roaming */
#define BRCMF_ROAM_EAPOL_MAX	(3 * HZ)
#define BRCMF_ROAM_PROBE_MS	300
#define BRCMF_ROAM_REPLAY_MS	150

static s32 brcmf_bss_roaming_done(struct brcmf_cfg80211_info *cfg,
				  struct net_device *ndev,
				  const struct brcmf_event_msg *e);

/* Report a roam to @addr once. Call with cfg->roam_lock held. */
static void brcmf_roam_report_locked(struct brcmf_cfg80211_info *cfg,
				     struct brcmf_if *ifp, const u8 *addr,
				     const char *why)
{
	struct brcmf_cfg80211_profile *profile = &ifp->vif->profile;
	struct brcmf_event_msg e = {};

	if (ether_addr_equal(addr, profile->bssid)) {
		pr_info("brcmfmac: roam: %s for %pM, already reported\n", why, addr);
		return;
	}
	pr_info("brcmfmac: roam: %pM -> %pM, reported from %s\n",
		profile->bssid, addr, why);
	memcpy(e.addr, addr, ETH_ALEN);
	WRITE_ONCE(cfg->roam_eapol_seen, false);
	brcmf_bss_roaming_done(cfg, ifp->ndev, &e);
	brcmf_net_setcarrier(ifp, true);
	if (READ_ONCE(cfg->roam_eapol))
		mod_delayed_work(system_wq, &cfg->roam_work,
				 msecs_to_jiffies(BRCMF_ROAM_REPLAY_MS));
}

static void brcmf_roam_report(struct brcmf_cfg80211_info *cfg,
			      struct brcmf_if *ifp, const u8 *addr,
			      const char *why)
{
	mutex_lock(&cfg->roam_lock);
	brcmf_roam_report_locked(cfg, ifp, addr, why);
	mutex_unlock(&cfg->roam_lock);
}

/* rx path, any context: hold EAPOL from a BSSID the host isn't on. */
bool brcmf_cfg80211_roam_eapol(struct brcmf_if *ifp, struct sk_buff *skb)
{
	struct brcmf_cfg80211_info *cfg = ifp->drvr->config;
	struct brcmf_cfg80211_vif *vif = ifp->vif;
	struct sk_buff *old;
	const u8 *src;

	if (!cfg || !vif || skb->protocol != htons(ETH_P_PAE) ||
	    vif->wdev.iftype != NL80211_IFTYPE_STATION ||
	    vif->profile.use_fwsup != BRCMF_PROFILE_FWSUP_NONE ||
	    !test_bit(BRCMF_VIF_STATUS_CONNECTED, &vif->sme_state) ||
	    ifp->ndev != cfg_to_ndev(cfg))
		return false;
	src = eth_hdr(skb)->h_source;
	if (ether_addr_equal(src, vif->profile.bssid)) {
		WRITE_ONCE(cfg->roam_eapol_seen, true);
		return false;
	}
	pr_info_ratelimited("brcmfmac: roam: EAPOL from %pM while on %pM%s\n",
			    src, vif->profile.bssid,
			    brcmf_roam_eapol_hold ? ", held" : "");
	if (!brcmf_roam_eapol_hold)
		return false;
	WRITE_ONCE(cfg->roam_eapol_at, jiffies);
	old = xchg(&cfg->roam_eapol, skb);
	if (old)
		brcmu_pkt_buf_free_skb(old);
	mod_delayed_work(system_wq, &cfg->roam_work,
			 msecs_to_jiffies(BRCMF_ROAM_PROBE_MS));
	return true;
}

/* Replay a held frame once its sender is the BSSID the host is on, or work out
 * from the firmware whether it roamed there without telling us.
 */
static void brcmf_roam_worker(struct work_struct *work)
{
	struct brcmf_cfg80211_info *cfg =
		container_of(to_delayed_work(work), struct brcmf_cfg80211_info,
			     roam_work);
	struct net_device *ndev = cfg_to_ndev(cfg);
	struct brcmf_if *ifp;
	struct sk_buff *skb;
	u8 src[ETH_ALEN], fw[ETH_ALEN];
	bool connected;

	if (!ndev)
		return;
	ifp = netdev_priv(ndev);
	mutex_lock(&cfg->roam_lock);
	skb = xchg(&cfg->roam_eapol, NULL);
	if (!skb)
		goto out;
	connected = test_bit(BRCMF_VIF_STATUS_CONNECTED, &ifp->vif->sme_state);
	memcpy(src, eth_hdr(skb)->h_source, ETH_ALEN);
	if (!connected ||
	    time_after(jiffies, READ_ONCE(cfg->roam_eapol_at) + BRCMF_ROAM_EAPOL_MAX)) {
		pr_info("brcmfmac: roam: held EAPOL from %pM dropped\n", src);
		brcmu_pkt_buf_free_skb(skb);
		goto out;
	}
	if (ether_addr_equal(src, ifp->vif->profile.bssid)) {
		if (READ_ONCE(cfg->roam_eapol_seen)) {
			pr_info("brcmfmac: roam: held EAPOL from %pM superseded\n", src);
			brcmu_pkt_buf_free_skb(skb);
		} else {
			pr_info("brcmfmac: roam: held EAPOL from %pM passed up\n", src);
			brcmf_netif_rx(ifp, skb);
		}
		goto out;
	}
	/* No event has reported a roam yet: ask the firmware where it is. */
	if (!brcmf_fil_cmd_data_get(ifp, BRCMF_C_GET_BSSID, fw, ETH_ALEN) &&
	    ether_addr_equal(fw, src)) {
		/* keep it for the replay, unless a newer frame took its place */
		if (cmpxchg(&cfg->roam_eapol, NULL, skb))
			brcmu_pkt_buf_free_skb(skb);
		brcmf_roam_report_locked(cfg, ifp, src, "EAPOL + firmware BSSID");
		goto out;
	}
	/* Not there (yet): keep it until it is too old. */
	if (!cmpxchg(&cfg->roam_eapol, NULL, skb))
		mod_delayed_work(system_wq, &cfg->roam_work,
				 msecs_to_jiffies(BRCMF_ROAM_PROBE_MS));
	else
		brcmu_pkt_buf_free_skb(skb);
out:
	mutex_unlock(&cfg->roam_lock);
}

/* Connection and roam events, logged when brcmfmac.roam_log is set. Samsung
 * enables these; the firmware only sends events a handler is registered for.
 */
static s32 brcmf_notify_roam_trace(struct brcmf_if *ifp,
				   const struct brcmf_event_msg *e, void *data)
{
	return 0;
}
'''

edit('cfg80211.c', [(
    '#include <linux/vmalloc.h>\n',
    '#include <linux/vmalloc.h>\n#include <linux/workqueue.h>\n',
), (
    'static bool brcmf_is_linkup(struct brcmf_cfg80211_vif *vif,\n',
    HELPERS + '\nstatic bool brcmf_is_linkup(struct brcmf_cfg80211_vif *vif,\n',
), (
    # Samsung: link up while connected is a roam (wl_cfg80211.c, WLC_E_LINK).
    '''		err = brcmf_notify_connect_status_ap(cfg, ndev, e, data);
	} else if (brcmf_is_linkup(ifp->vif, e)) {''',
    '''		err = brcmf_notify_connect_status_ap(cfg, ndev, e, data);
	} else if (brcmf_roam_link && e->event_code == BRCMF_E_LINK &&
		   (e->flags & BRCMF_EVENT_MSG_LINK) &&
		   e->status == BRCMF_E_STATUS_SUCCESS &&
		   !brcmf_is_ibssmode(ifp->vif) &&
		   test_bit(BRCMF_VIF_STATUS_CONNECTED, &ifp->vif->sme_state)) {
		/* zboot: firmware roam reported by link up */
		brcmf_roam_report(cfg, ifp, e->addr, "E_LINK");
	} else if (brcmf_is_linkup(ifp->vif, e)) {''',
), (
    '''	if (event == BRCMF_E_ROAM && status == BRCMF_E_STATUS_SUCCESS) {
		if (test_bit(BRCMF_VIF_STATUS_CONNECTED,
			     &ifp->vif->sme_state)) {
			brcmf_bss_roaming_done(cfg, ifp->ndev, e);
		} else {''',
    '''	if (event == BRCMF_FW_E_BSSID) {
		/* zboot: firmware roam: Samsung's roam-complete event */
		if (brcmf_roam_link && status == BRCMF_E_STATUS_SUCCESS &&
		    test_bit(BRCMF_VIF_STATUS_CONNECTED, &ifp->vif->sme_state))
			brcmf_roam_report(cfg, ifp, e->addr, "E_BSSID");
		return 0;
	}
	if (event == BRCMF_E_ROAM && status == BRCMF_E_STATUS_SUCCESS) {
		if (test_bit(BRCMF_VIF_STATUS_CONNECTED,
			     &ifp->vif->sme_state)) {
			brcmf_roam_report(cfg, ifp, e->addr, "E_ROAM");
		} else {''',
), (
    '''	brcmf_fweh_register(cfg->pub, BRCMF_E_RSSI, brcmf_notify_rssi);
''',
    '''	brcmf_fweh_register(cfg->pub, BRCMF_E_RSSI, brcmf_notify_rssi);
	/* zboot: firmware roam: events Samsung's driver enables */
	brcmf_fweh_register(cfg->pub, (enum brcmf_fweh_event_code)BRCMF_FW_E_BSSID,
			    brcmf_notify_roaming_status);
	brcmf_fweh_register(cfg->pub, BRCMF_E_REASSOC, brcmf_notify_roam_trace);
	brcmf_fweh_register(cfg->pub, BRCMF_E_ROAM_PREP, brcmf_notify_roam_trace);
	brcmf_fweh_register(cfg->pub, BRCMF_E_ROAM_START, brcmf_notify_roam_trace);
	brcmf_fweh_register(cfg->pub, BRCMF_E_AUTH, brcmf_notify_roam_trace);
	brcmf_fweh_register(cfg->pub, BRCMF_E_ASSOC, brcmf_notify_roam_trace);
	brcmf_fweh_register(cfg->pub, BRCMF_E_DISASSOC, brcmf_notify_roam_trace);
''',
), (
    '''	mutex_init(&cfg->usr_sync);
	brcmf_init_escan(cfg);''',
    '''	mutex_init(&cfg->usr_sync);
	mutex_init(&cfg->roam_lock);
	INIT_DELAYED_WORK(&cfg->roam_work, brcmf_roam_worker);
	brcmf_init_escan(cfg);''',
), (
    '''	wl_deinit_priv(cfg);
	cancel_work_sync(&cfg->escan_timeout_work);''',
    '''	wl_deinit_priv(cfg);
	cancel_work_sync(&cfg->escan_timeout_work);
	cancel_delayed_work_sync(&cfg->roam_work);
	if (cfg->roam_eapol)
		brcmu_pkt_buf_free_skb(cfg->roam_eapol);''',
)])

# --- core.c: the rx hook ----------------------------------------------------
edit('core.c', [(
    '''	ifp->ndev->stats.rx_bytes += skb->len;
	ifp->ndev->stats.rx_packets++;

	brcmf_dbg(DATA, "rx proto=0x%X\\n", ntohs(skb->protocol));''',
    '''	/* zboot: firmware roam: EAPOL from a BSSID the host isn't on yet */
	if (brcmf_cfg80211_roam_eapol(ifp, skb))
		return;

	ifp->ndev->stats.rx_bytes += skb->len;
	ifp->ndev->stats.rx_packets++;

	brcmf_dbg(DATA, "rx proto=0x%X\\n", ntohs(skb->protocol));''',
)])

# --- fweh.c: the event log ----------------------------------------------------
edit('fweh.c', [(
    '''static void brcmf_fweh_event_worker(struct work_struct *work)
{''',
    '''/* zboot: firmware roam: log connection and roam events */
static bool brcmf_roam_log = true;
module_param_named(roam_log, brcmf_roam_log, bool, 0644);
MODULE_PARM_DESC(roam_log, "Log the firmware's connection and roam events");

static bool brcmf_roam_logged(u32 fw_code)
{
	switch (fw_code) {
	case 0: case 3: case 5: case 6: case 7: case 9: case 11: case 12:
	case 16: case 19: case 32: case 37: case 46: case 125:
		return true;
	}
	return false;
}

static void brcmf_fweh_event_worker(struct work_struct *work)
{''',
), (
    '''		brcmf_dbg(EVENT, "  version %u flags %u status %u reason %u\\n",''',
    '''		if (brcmf_roam_log && brcmf_roam_logged(event->code))
			pr_info("brcmfmac: fw event %u (%s) status %u reason %u flags %#x addr %pM\\n",
				event->code, brcmf_fweh_event_name(code),
				emsg.status, emsg.reason, emsg.flags, emsg.addr);
		brcmf_dbg(EVENT, "  version %u flags %u status %u reason %u\\n",''',
)])
print('patch-brcmfmac-roam: applied')
