# VoLTE, Wi-Fi calling, RCS and MMS on the S9 (ROADMAP 8.39, 8.40, 8.41, 8.42)

Status 2026-11-12. The carrier is T-Mobile US, or Mint Mobile (an MVNO on
T-Mobile). No SIM was in the phone while this was written, so nothing here
has met the network. Every claim carries a label:

- **observed**: read on this phone, from its stock SYSTEM/VENDOR images
  (firmware G960FXXUHFVG4, Android 10, mounted read-only at `/run/android`);
- **from source**: a public document, with its address;
- **inferred**: reasoning from the above, not checked.

## Summary

| | Verdict | Size | Confidence |
|---|---|---|---|
| MMS (8.41) | **Feasible. Built** on the host: codec, daemon, modem hooks, Settings, Messaging. Not yet run on a network. | done to stage 3 of 4 | high that it can work. Medium that it works on T-Mobile without IPv6 (below) |
| VoLTE (8.39) | **Feasible in principle, very large.** No open stack exists for Shannon. | months | medium-low |
| Wi-Fi calling (8.42) | **Feasible in principle, very large.** It shares the IMS client and the media path with VoLTE. | months, after or with VoLTE | medium-low |
| RCS (8.40) | **Not feasible** in any useful form. | n/a | high |

**The most important finding for T-Mobile** (inferred, verify): T-Mobile US
switched off 3G (UMTS) in 2022 (from source, below). A phone without VoLTE
then gets calls only by falling back to 2G GSM. Where T-Mobile's GSM is
weak or switched off, this phone has **no voice calls at all** on T-Mobile
until VoLTE works. SMS on LTE without IMS goes over SGs, which the network
still offers to non-IMS phones (inferred). Our circuit-switched calls and
SMS were proven on Dialog in Sri Lanka, not on T-Mobile. So the first SIM
test, a plain CS call and SMS, decides how urgent 8.39 is.

## How Samsung does IMS on this phone (stock)

All observed in the stock images unless marked otherwise.

- **The IMS stack runs on the AP**, not in the modem:
  - `system/priv-app/imsservice` (com.sec.imsservice, 10 MB);
  - `system/lib64/libsec-ims.so` (8 MB). Its strings show a resiprocate
    (`resip::`) SIP stack: registration, `AKAv1-MD5`/`AKAv2-MD5` digest,
    `IPSecInterface::IPSecApplySAAssoc` / `IPSecApplySAPolicy` (kernel IPsec SAs
    for 3GPP sec-agree), SDP with `AMR`, `AMR_WB`, `EVS`, video, RTT, and
    `/dev/socket/imsd`;
  - `system/bin/imsd`; `libvtstack.so` and `libvtmanager.so` for video calls.
- **The modem's part** is in `vendor/lib64/libsec-ril.so` (`IpcProtocol41`),
  read from its symbol names (observed):
  - IMS PDN and P-CSCF: `IpcTxDefinePdpContext(apn, cid, DataProtocol, ...)`,
    `IpcTxSetDataProfile(..., PcscfViaPco, ...)`,
    `IpcTxSetLteAttachProfile(..., PcscfViaPco, ...)`, `IpcRxSdmPcscf`
    (the P-CSCF addresses from the network) and `IpcRxSdmImsSipPort`;
  - AKA through the SIM: `IpcRxSecImsAuth` / `IpcRxIilIsimAuth` (a challenge
    in) and `IpcTxImsAuthResp`; `IpcTxDoUiccAuth`,
    `MakeUiccAuthForIsimAuthentication` and `MakeUiccAuthFor3G`. These are
    the SIM AUTHENTICATE commands an open IMS or ePDG client needs;
  - registration state to the CP: `IpcTxImsReqSetRegState(ImsRegStatus,
    NetworkType, EcmpState, EpdgState, ...)` and
    `IpcTxImsReqDeregistration(E911)`. The CP must know IMS is registered,
    or it keeps sending calls and SMS the circuit-switched way (inferred);
  - calls: `IpcTxImsCallSetCallList` (the CP keeps a copy of IMS calls, for
    SRVCC handover to 2G/3G), `IpcRxIms`, `IpcRxImsModeChange`, and an
    `IMS_RTP_MEDIA` string.
- **Media (RTP) is in the CP** (inferred, strongly): libsec-ims logs
  `#CpMediaStats RTP RX:SSRC[...]`, `RTCP TX`, `RTPtimedout`, and has
  `ImsSetRtpMedia`, `REQUEST_RTP_STATS_TO_STACK` and
  `DispatchSendMediaEvent`. The AP negotiates SDP and hands ports and codecs
  to the CP. The CP runs RTP/AMR/EVS. Sound then takes the same VSS/ABOX
  path as a circuit-switched call (`audio/CALL-AUDIO.md`), which we already
  drive. The dedicated bearer (QCI 1) is the network's and the CP's;
  libsec-ims only sees `DedicatedBearerData`.
- **Wi-Fi calling (ePDG) is also on the AP** (observed):
  - `system/priv-app/EpdgService`, `system/app/UnifiedWFC`, `system/bin/charon`;
  - `liberis_charon.so` and `liberis_strongswan.so`, Samsung's "eris" fork of
    strongSwan, as well as a stock `libstrongswan.so`/`libcharon.so`;
  - `system/etc/epdg_apns_conf.xml` has T-Mobile US tunnels (the
    `TMobile_US` entries) with these settings:
    - APNs `ims`, `mms` and `xcap`;
    - IKEv2 with EAP-AKA;
    - IKE and ESP: AES-CBC-256, HMAC-SHA1-96, MODP-1024;
    - inner IPv6, the P-CSCF from the `P-CSCF-V6` configuration attribute;
    - DPD every 60 s, NAT keepalive every 120 s, interface `epdg0`;
    - Wi-Fi calling enabled, cellular preferred.
  - Media on Wi-Fi cannot go through the CP's radio. Inferred: either the AP
    relays RTP to the CP's voice engine, or the AP runs the codecs itself.
    Not determined.
- **Carrier configuration in imsservice** (`res/raw/*.json`, observed):
  - `mnomap.json` maps 310-160/200/210/220/230/240/250/260/270/660/800 to
    `TMobile_US`, and three Tracfone GID1s to `TMobile_US:TFN`. There is **no
    Mint entry**: a Mint SIM (310-260 or 310-240) is treated as T-Mobile;
  - `imsprofile.json`, profile "TMobile LTE/WiFi":
    - PDN `ims`, IPv6 only, `support_ipsec`;
    - services `mmtel`, `mmtel-video`, `smsip`, presence and RCS;
    - codecs `EVS, AMR-WB, AMR`;
    - user agent `T-Mobile [SUPPORT] SEC/[PRODUCT_MODEL] [BUILD_VERSION]`;
    - also a "TMobile E911" profile on PDN `emergency`;
  - `globalsettings.json` for `TMobile_US`:
    - supplementary services only over IMS (`ss_domain_setting
      PS_ONLY_VOLTEREGIED`) and emergency calls over PS;
    - XCAP through `xcap.msg.pc.t-mobile.com` and GBA through
      `bsf.msg.pc.t-mobile.com`;
  - `mobilityupdate.json` (Samsung "SIM mobility", with
    `system/app/SimMobilityKit`) turns on `mmtel, mmtel-video, smsip` over LTE
    and Wi-Fi for `TMobile_US` on this open-market firmware.

  Inferred: stock One UI on this international SM-G960F does enable VoLTE
  and Wi-Fi calling for a T-Mobile US SIM, and by the GID-less mapping for
  Mint too. Whether **T-Mobile's network** provisions IMS for this IMEI is
  a separate question (next section).

## T-Mobile and Mint: will the network allow it?

From public sources, gathered 2026-11-12:

- **T-Mobile:** XDA reports no VoLTE device allow-list, unlike AT&T. T-Mobile
  blocked new activations of 2G/3G-only phones from January 2021
  (from source, uncertain: https://www.xda-developers.com/t-mobile-att-require-volte-phone-calls-shut-down-3g).
  T-Mobile's own policy page refused automated reads, so it is not read.
- **Mint:** Mint requires a VoLTE-capable unlocked phone and checks the IMEI
  on its compatibility page (from source:
  https://www.mintmobile.com/device-compatibility,
  https://www.mintmobile.com/blog/what-is-volte/). Wi-Fi calling must first
  be enabled with a 911 address in the Mint app or on Mint's website (from
  source, uncertain: https://www.androidcentral.com/does-wi-fi-calling-work-mint-mobile).
  So Mint's rules may be **stricter** than T-Mobile's: the IMEI check
  decides. The owner can run that check; we never put the IMEI anywhere.
- **The SM-G960F on US networks:** a user report says an international S9
  (G965F/DS) works with VoLTE on Mint. Another says international Samsungs
  get no VoLTE in the US. Nothing confirms the G960F on T-Mobile
  (from source, uncertain: https://community.e.foundation/t/samsung-s9-currently-unusable-in-usa-without-volte/39255).
- **Bands** (inferred, from the G960F's published band list, not checked
  here): LTE bands 2, 4, 12 and 66 are on the G960F. Band 71 (600 MHz) is
  not. Coverage that is band-71-only is lost whatever the stack.
- **The E911 address for Wi-Fi calling:** T-Mobile takes it on the account
  web page (Profile > line > E911 address), as well as in the phone's own
  dialog (from source:
  https://www.t-mobile.com/support/account/change-your-billing-and-e911-address).
  So it can be set without Samsung's app. For Mint it is the Mint app or
  website.
- **ePDG address:** `epdg.epc.mnc260.mcc310.pub.3gppnetwork.org` (the 3GPP
  TS 23.003 form; it resolves to T-Mobile addresses per
  https://www.netify.ai/resources/hostnames/epdg.epc.mnc260.mcc310.pub.3gppnetwork.org;
  from source, uncertain). For a Mint SIM reporting 310-240 the `mnc240`
  form would be tried first (inferred).

## VoLTE (8.39) on our own stack

What exists in the open (from source, via the web research of 2026-11-12):

- **postmarketOS VoLTE** works only on Qualcomm phones. There the IMS stack
  is inside the modem, and `81voltd` (QMI) just switches it on
  (https://packages.debian.org/sid/81voltd). ModemManager has no IMS
  support (https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/issues/378).
  None of it applies to Shannon, whose IMS is on the AP.
- **Osmocom's FOSS IMS client** (a modified doubango) registers with
  sec-agree against a Kamailio IMS core. It is work in progress
  (https://projects.osmocom.org/projects/foss-ims-client/wiki).
- **fasferraz/SWu-IKEv2** is a Python ePDG (SWu) client with EAP-AKA; it is
  only the tunnel (https://fasferraz.github.io/SWu-IKEv2).
- **osmo-epdg** is the network side, not useful here.
- **Samsung's imsservice in Waydroid:** not viable (inferred). It is built
  against Samsung's framework and RIL extensions, which Waydroid's
  LineageOS lacks. One project patches it for a single device
  (https://github.com/jameskdev/android_samsung_imsservice). Our modem has
  no RIL inside the container either.
- Osmocom has a page on this very phone's VoWiFi, which describes Samsung's
  userspace "eris" IKEv2/ESP stack
  (https://projects.osmocom.org/projects/cellular-modems/wiki/Samsung_GalaxyS9_VoWiFi).
  Its server blocks automated reads, so it is unread here. Worth reading in a
  browser.

**What we would have to build:**

1. **IMS PDN.** A second context (cid 2, which should be rmnet1, inferred
   from "context 1 is rmnet0") with APN `ims`, PDP type IPv6. The
   `DataProtocol` value for IPv6 and the P-CSCF-via-PCO flag are still to be
   read from `IpcTxDefinePdpContext` / `IpcTxSetDataProfile`. IPv6 on rmnet
   is switched off today (`modem/README.md`).
2. **P-CSCF** from `IpcRxSdmPcscf` (layout to read), or from the 0d09
   context report.
3. **IMS AKA:**
   - the SIM's answer to RAND/AUTN through `IpcTxDoUiccAuth` (ISIM, else
     USIM), or `AT+CSIM` if the modem allows it (libsec-ril has a
     `CscFeature_RIL_SupportCSIM` switch);
   - then SIP REGISTER with the security-client headers and two IPsec SAs
     through the kernel's xfrm, which mainline has.
4. **Tell the CP:** `IpcTxImsReqSetRegState`, so that it routes calls and SMS
   through IMS. Layout to read.
5. **Calls:**
   - SIP INVITE with SDP preconditions (T-Mobile: `use_precondition`);
   - media in the CP through the `IMS_RTP_MEDIA` IPC, layout unknown. The
     alternative is RTP and AMR-WB on the AP, with the audio moved off VSS
     onto our own ALSA path: a new audio route, no EVS, and echo
     cancellation to do. Both are large.
6. **SMS over IMS** (`smsip`), once registered: the network may stop SMS
   over SGs (inferred).

**Size:** months of work, most of it reverse engineering libsec-ril and
libsec-ims. The client can grow from the Osmocom/doubango client.
**Confidence that it can be done:** medium-low. The risky part is the CP's
media IPC.

**Smallest safe first experiment** (needs approval; it reaches the network):
bring up the IMS PDN only, look at what comes back, and bring it down.
Nothing is sent over it.

- PDN: cid 2. APN `ims`. PDP type IPv6 (the value read from libsec-ril
  first; if it cannot be read, IPv4v6).
- Sent to the modem: `0d01` define and `0d04` up for cid 2, then `0d04` down.
  The same messages as for data, so `ipc 0d 01 3 <hex>` on the control
  pipe, by hand.
- Watched: the `8001` results, `0d10` state, `0d09` addresses and P-CSCF,
  and any `IpcRxSdmPcscf` message. rmnet1 stays down; IPv6 stays off; no
  SIP, no DNS, nothing on the link.
- Risk: the network may refuse (no IMS subscription), or the modem may drop
  LTE. A modem restart recovers it (`modem/README.md`).

## Wi-Fi calling (8.42) on our own stack

- **Route:** strongSwan (or SWu-IKEv2) to the ePDG with the settings from
  `epdg_apns_conf.xml` above: EAP-AKA with the SIM's answers from the modem
  (the same `IpcTxDoUiccAuth`, or `AT+CSIM`). Then the same IMS client as
  VoLTE, through the tunnel.
- **What it saves:** none of the CP's IMS IPC is needed for registration,
  because the CP is not involved.
- **What it costs:** media must be on the AP (inferred): RTP, AMR-WB, and
  audio through our own ALSA path. VoLTE would get that path too, if the
  CP's media IPC stays closed.
- **Why it matters:** it works where LTE coverage is poor, and where only
  band 71 reaches.
- **First experiment** (needs approval: it sends to the SIM and to T-Mobile):
  1. Read whether the modem accepts `AT+CSIM` at all, by `AT+CSIM=?`, a
     test command with no SIM access.
  2. Then, approved separately: one IKEv2 IKE_SA_INIT and IKE_AUTH exchange
     to `epdg.epc.mnc260.mcc310.pub.3gppnetwork.org` with identity
     `0<IMSI>@nai.epc.mnc260.mcc310.3gppnetwork.org`, APN `ims`. The one
     EAP-AKA challenge is answered by one SIM AUTHENTICATE.
  3. Success is a tunnel with an inner IPv6 address and a P-CSCF. It is
     torn down at once. No SIP.
- **Size:** months, shared with VoLTE. **Confidence:** medium-low.

## RCS (8.40)

- **Google Jibe through Google Messages** needs:
  - Play services and Play Integrity device integrity. Rooted and custom-ROM
    phones fail it (from source, uncertain:
    https://9to5google.com/2024/02/29/google-messages-rcs-rooted/);
  - Messages as the default SMS app on a phone with a SIM. It verifies the
    number by SMS.

  Our Waydroid runs microG (`android/waydroid/MICROG-TASK.md`), which does
  not pass Play Integrity. It has no SIM and no telephony, so Messages cannot
  even start RCS setup. Bridging SMS into it would not give it a SIM
  identity (inferred). **Not feasible.**
- **T-Mobile's RCS:**
  - imsservice's T-Mobile profile carries the RCS services (`im`, `ft`,
    `ft_http`, `chatbot-communication`) on the IMS registration (observed);
  - T-Mobile's legacy "Advanced Messaging" is retired in favour of Jibe
    (inferred, unconfirmed);
  - a client of our own would need the whole VoLTE IMS client first, then
    GSMA RCS provisioning (ACS), which carriers accept only from approved
    clients. **Not feasible** in practice.
- **Open clients:** none found for Linux phones (from source, uncertain).
- **Verdict:** no RCS. Group texts and pictures go by MMS (8.41), and RCS
  users' phones fall back to SMS/MMS for us.

## MMS (8.41): what it needs, and what was built

**What MMS needs** (from the standards: OMA-TS-MMS-ENC 1.3, WAP-230-WSP):

- An **m-notification-ind** arrives as an SMS: 8-bit data to WAP push port
  2948, often in two parts. It carries the message's address on the
  carrier's MMSC.
- The message is fetched with **HTTP GET** through the access point that
  carries MMS, and answered with an **m-notifyresp-ind**.
- Sending is an **HTTP POST** of an m-send-req to the MMSC. The answer is an
  m-send-conf.

**The carriers** (observed in the stock `apns-conf.xml`, and LineageOS):

| SIM | Matched by | APN (carries MMS) | MMSC | Proxy |
|---|---|---|---|---|
| T-Mobile US | 310-260, no MVNO rule | `fast.t-mobile.com`, IPv6 | `http://mms.msg.eng.t-mobile.com/mms/wapenc` | none |
| Mint | 310-240 (LineageOS), GID1 `756D` (from source); some sources say 310-260 | `Wholesale`, IPv4v6 | `http://wholesale.mmsmvno.com/mms/wapenc` | none |

Both carry MMS on the data access point, so no second PDN is needed. The
stock file has no Mint entry, so ours (`ztmetro/mms/apns-extra.xml`) adds it
under both MNCs, behind its GID1. Entries that are Wi-Fi only
(`bearer_bitmask 18`, such as T-Mobile's `tmus`) are left out.

**One risk for T-Mobile** (inferred): `fast.t-mobile.com` is IPv6 in every
list, and our data context asks for IPv4 only (PDP type 2). If T-Mobile
refuses IPv4 there, or the MMSC has no IPv4 address, MMS needs IPv6 in
modemd first: IPv6 or IPv4v6 PDP type, the rmnet IPv6 address, and probably
CLAT/464XLAT for IPv4-only apps. That is a roadmap item of its own. The
first data test on the SIM will show it.

### Built (branch `research/telephony`)

1. **`ztmetro/mms/mmspdu.c`**, the codec:
   - decodes the WAP push SMS (ports, concatenation), the WSP push, and
     m-notification-ind / m-retrieve-conf (multipart, charsets, Content-ID,
     names) / m-send-conf / delivery-ind;
   - encodes m-send-req (multipart/related with SMIL, UTF-8 text, pictures,
     several recipients), m-notifyresp-ind and m-acknowledge-ind.
2. **`ztmetro/mms/apnconf.c`**, the auto-fill from `apns-conf.xml`:
   - matches by MCC/MNC (3 digits, then 2), with GID1 prefix, SPN and IMSI
     rules winning over plain entries, as Android does;
   - reads the stock file (`/run/android/system/etc/apns-conf.xml`, readable
     by all) and then ours.
3. **`ztmetro/mms/ztmmsd.c`**, the daemon (`ztmmsd.service`, root):
   - takes notifications from `/var/lib/ztoscorenetworking/mms/push` and
     joins their parts;
   - asks modemd for the connection (`mms on`), fetches with its own HTTP
     client bound to the interface (SO_BINDTODEVICE), resolving through the
     context's own name servers;
   - answers notifyresp, stores the parts in `.../mms/store/<id>/` with a
     `meta` file, and writes `in <t> <from> <text> mms:<id>` into the SMS log
     and a banner;
   - sends: `send NUMBERS FILE TEXT` on `/run/ztmmsd.ctl`;
   - **does nothing on the network unless `enabled = 1`** in
     `/etc/ztoscorenetworking/mms`, which is off by default. Without it,
     notifications wait in `.../mms/waiting` and sends fail at once;
   - files are sent only from its sticky outbox, owned by a user, not linked,
     so that nobody can mail out what they could not read;
   - nothing about a message (numbers, text, addresses) goes to the journal.
4. **modemd** (`ztoscorenetworkingd.c`, its own commit, a6efd98):
   - an SMS to port 2948 goes to the push directory instead of the text log;
   - `mms on|off` uses context 1. When data is off, rmnet0 gets its address
     with **no default route**, so only ztmmsd's bound sockets use it. That
     state is written to `/run/ztoscorenetworking/mms`. An "mms on" not taken
     back is let go after 5 minutes;
   - GID1 and SPN are read by `AT+CRSM` READ BINARY after `AT+CIMI` into
     `/run/ztoscorenetworking/sim`, for the auto-fill. The IMSI itself is not
     written;
   - the hooks are one or two lines each in `sms_arrived`, `on_line` (CIMI,
     +CRSM), `ipc_message` (the 0d01 answer also for MMS), `command` and the
     main loop. Everything else is a new section. **Coordinate with
     `feature/servicemode` before merging**, since it edits the same file.
5. **Settings** > cellular + SIM > access point: a picture messages switch;
   MMSC, MMS proxy, MMS port and MMS APN boxes; and "fill in for this SIM".
   The last fills both the data and the MMS fields from the SIM's carrier,
   and they are saved with "save".
6. **Messaging:**
   - pictures shown in bubbles, both ones received (from the store) and ones
     sent;
   - "..." > attach a picture takes the newest picture in `~/Pictures`. Again
     takes the next older; past the oldest, none;
   - the picture is scaled to a JPEG of 600 KB or less (1280, 960 or 640 px),
     put in the outbox, and sent with the text.
7. **Tests:** `make -C ztmetro check-mms`, all on the host:
   - 126 codec and auto-fill checks, including every truncation and 20 000
     random PDUs;
   - the daemon's offline tools: two-part notification, send request, size
     limit, T-Mobile/Mint lookup;
   - modemd's SMS fixtures, with two new cases (a notification is not a text,
     and is kept for ztmmsd).

   Against the real stock file, `ztmmsd --apn` gives:
   - 310260: `fast.t-mobile.com` and T-Mobile's MMSC;
   - 310260 + GID1 6D38: MetroPCS;
   - 310240 + GID1 756D: Mint `Wholesale`;
   - 23430: EE with its MMS APN and proxy.

### Left to do

- **Install and check on the phone with no SIM:**
  - the daemon starts, Settings shows the fields, Messaging still opens and
    draws;
  - not done: the phone was in its quiet hours (22:00-07:00) during this
    work, and its uploads were very slow;
  - the binaries were built on the phone in `~/ztm-telephony`.
- **First network run** (needs a SIM and approval, below).
- **Not done yet:**
  - MMS on an access point of its own (a second context on rmnet1), for
    carriers whose MMS APN differs from data (not T-Mobile or Mint);
  - IPv6 contexts (see the risk above);
  - group MMS shown as a group (the other recipients are stored in `meta`,
    but threads are per number);
  - delivery reports;
  - video and audio attachments in Messaging (the daemon handles them);
  - retry on HTTP failures beyond the ten-minute rescan.
- **The UAProf header:** ztmmsd sends `x-wap-profile:
  http://wap.samsungmobile.com/uaprof/SM-G960F.xml` and `User-Agent:
  SAMSUNG-SM-G960F` by default (both can be changed in the conf file). Some
  MMSCs refuse requests without them (inferred). The URL is not verified.

## Experiments that need approval, each exactly

None of these has been run. Insert the SIM first.

1. **Read the SIM's identity** (no network traffic): with the SIM in, the
   modem daemon as built here sends `AT+CRSM=176,28478,0,0,0` (EF_GID1)
   and `AT+CRSM=176,28486,0,0,17` (EF_SPN) once after `AT+CIMI`. These are
   READ BINARY of two SIM files, not AUTHENTICATE. They are listed because
   they are new commands to the SIM.
2. **Plain data and CS on T-Mobile/Mint** (the existing paths): data on the
   carrier's APN (filled in by Settings), one CS call and one SMS to the
   owner's own second phone. This shows whether the IPv4 context is
   accepted on `fast.t-mobile.com` / `Wholesale`, and whether CS voice still
   exists here.
3. **MMS receive:** the owner sends one picture MMS from another phone to
   this one, with `enabled = 1`.
   - PDN: none new. Context 1, the data APN, already up (or brought up by
     `mms on` when data is off).
   - Sent by us: one HTTP GET to the Content-Location in the notification,
     on the T-Mobile/Mint MMSC host, and one HTTP POST of m-notifyresp-ind
     (about 20 bytes) to the MMSC.
4. **MMS send:** one picture (under 600 KB) with a short text to the owner's
   second phone.
   - PDN: context 1, as above.
   - Sent: one HTTP POST of m-send-req to the MMSC: one recipient, a SMIL
     part, the JPEG and the text.
5. **IMS PDN only** (VoLTE first step): cid 2, APN `ims`, IPv6; `0d01` and
   `0d04` up, observe, `0d04` down. Nothing on the link. See above.
6. **ePDG tunnel** (Wi-Fi calling first step): `AT+CSIM=?` first. Then,
   separately, one IKEv2 exchange to T-Mobile's ePDG with one SIM
   AUTHENTICATE. See above.

## The owner's checklist

1. **Insert the T-Mobile or Mint SIM.** It comes before every network test.
2. Say which carrier: T-Mobile or Mint. Settings > access point > "fill in
   for this SIM" should then show it (it reads MCC/MNC and GID1, never
   shown in full).
3. A test number: the owner's second phone, for experiments 2-4. Not
   written into the repo or any report.
4. Whether a test MMS may be sent (experiment 4) and received (3).
5. For Mint: put the IMEI into Mint's compatibility checker
   (mintmobile.com/device-compatibility) yourself, and tell us only the
   result. For Wi-Fi calling, an E911 address on the account (T-Mobile:
   account page; Mint: app or website).
6. Whether T-Mobile 2G/3G calls still work where you are: if experiment 2's
   call fails, VoLTE becomes the priority.
