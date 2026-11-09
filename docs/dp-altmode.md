# DisplayPort Alt Mode (USB-C video out)

Goal: a USB-C monitor, dock or USB-C-to-HDMI/DP adapter gives the shell a
second output, an extended desktop like Samsung DeX, not a mirror. This page
covers the design, the device tree (all board wiring lives there), and where
each value comes from. A port to another Exynos 9810 phone (Galaxy Note 9,
N960F) should only need a device tree.

## Status

| Milestone | What | State |
|---|---|---|
| M1 | Alt Mode entered, HPD in the typec class | `max77705_usbc.dp_altmode=1`, image chg12 (in test) |
| M2 | SBU/AUX switch, combo PHY in DP mode, AUX, EDID read | switch and DT done (compile-tested); PHY/AUX driver not yet |
| M3 | DECON2 + DP link training, a fixed 1080p mode lit | not started |
| M4 | the second output in wlroots/ztMETRO | not started |

## The pieces

```
MAX77705 USB-C MCU (FW 0x32)          kernel/max77705-usbc.c, dp_altmode=1
  Discover ID/SVIDs/Modes (firmware) ─┐
  Enter Mode, DP Configure (0x48) ◄───┤ typec class: port altmode 0xff01 (from the DT),
  DP Status, Attention (PD_INT, 0x4b) ┘ partner altmode → typec_displayport driver
                                           │ orientation, DP pin state
                                           ▼
                           usb-c-connector graph (port@2 = SBU)
                                           ▼
                           gpio-sbu-mux: PCAL6524 pins 4 (AUX_SW_OE), 1 (SBU_SW_SEL)
typec_displayport ── drm_connector_oob_hotplug_event(HPD) ──► DP controller (M2/M3, to write)
                                                             combo PHY 0x110a0000 in DP mode
                                                             DECON2 0x16050000 → DP
```

### Order before Enter Mode (as stock)

On the S9, a dock raised HPD for stock while ours stayed at HPD low (dp3).
Stock's DP PHY and AUX are powered only after HPD, so they are not the
difference. What stock does before Enter Mode is (`max77705_alternate.c:350-413`,
`displayport_drv.c:2960-2975`):

1. When Discover SVIDs finds 0xff01, it turns the SBU switch on (select
   from the orientation, then output enable) and waits 100 ms.
2. It forces the USB host on and waits up to 10 s for it.
3. Only then does it act on Discover Modes and send Enter Mode.

The SBU switch then stays on through Enter Mode and Configure until unplug.

We do the same:

- At SVID discovery, max77705-usbc puts the port's mux in a DP state through
  the typec class.
- It registers the partner's DP altmode, which is what makes
  typec_displayport send Enter Mode, only once 100 ms have passed and the USB
  host is up (`usbc_otg_host_up()`: role host and the xHCI bound), or after
  10 s anyway, as stock.
- `kernel/patch-gpio-sbu-mux-safe.py` (`--dp-altmode` builds) makes
  gpio-sbu-mux leave the switch as it is in the USB Safe State, which
  typec_displayport sets around Enter and Configure.

The DP TX bring-up driver (`kernel/exynos9810-dp.c`: combo PHY DP mode and
AUX, phases run by hand through debugfs `exynos9810-dp/step`, every PHY
access behind the PMU isolation check) is built with `--dp-tx` only. With it
come its DT node and the connector's SuperSpeed port (port@1). That port
would otherwise wait for a switch that never comes.

The MAX77705 side follows Samsung's S9 code byte for byte
(`drivers/ccic/max77705_alternate.c`). The firmware runs discovery itself
once 0x55 = 3. The AP sends Enter Mode and DP Configure with 0x48. The
firmware sends DP Status Update by itself and reports it, the Configure ACK
and each Attention on PD_INT b2/b3/b4, read with 0x4b sub-ids 0x10/0x11/6.
The opcodes and their data are on the driver's allowlist; nothing else is
sent.

## Device tree

`kernel/make-dts.py --dp-altmode` (with `--power`) adds the nodes below. The
build switch is `scripts/build-boot.sh --dp-altmode`, which also builds in
`CONFIG_TYPEC_DP_ALTMODE` and `CONFIG_TYPEC_MUX_GPIO_SBU`, and must run in a
private kernel tree (`KSRC=`). Its release is `<base>-dp`;
`--release-suffix -dp2` (etc.) gives a test kernel whose ABI differs from
one on the phone its own release and module set, so a running kernel's
modules are never replaced. The drivers carry SoC- and protocol-level
logic only. A tree that lacks a node makes its driver log an error and stay
off. They do not fall back to S9 values.

### USB-C connector (under the MAX77705 USB-C node)

Standard bindings: `usb/usb-c-connector.yaml`, and `altmodes` as in
`usb/typec-altmode` (the mainline example uses the same DP VDO).

```dts
usb-c@25 {
	compatible = "maxim,max77705-usbc";
	...
	connector {
		compatible = "usb-c-connector";
		label = "USB-C";
		power-role = "dual";
		data-role = "dual";
		altmodes {
			displayport {
				svid = /bits/ 16 <0xff01>;
				vdo = <0x00001c46>;	/* DFP_D, DP signalling, receptacle, pins C/D/E */
			};
		};
		ports {
			#address-cells = <1>;
			#size-cells = <0>;
			port@2 {			/* SBU */
				reg = <2>;
				endpoint { remote-endpoint = <&sbu_mux_in>; };
			};
		};
	};
};
```

- **What max77705-usbc does with it:** it registers the typec port on the
  connector node, so the class finds the orientation and mode switches
  through the graph.
- **Required properties:** with `dp_altmode=1`, the driver requires
  `altmodes/displayport` with svid 0xff01 and a vdo that:
  - is DFP_D capable;
  - has DP signalling;
  - lists DFP_D pin assignments within C/D/E.

  If any of this is missing it logs `dp_altmode=1, but the device tree does
  not describe DP (...)`. It then writes no DP-related mask or opcode to the
  chip.
- **Pin mask:** the vdo's pin mask is the board's statement of which lane
  configurations its wiring carries. The DP Configure that the kernel sends
  is checked against it.
- **`orientation-sense-gpios`** (on the USB-C node, gpg0-4 on the S9): the
  board's own orientation input, the one stock selects the SBU switch from.
  max77705-usbc only reads it (`GPIOD_ASIS`: its configuration is left as it
  is). It logs `orientation: MCU CCPinStat ..., orientation-sense N` at each
  orientation change, and debugfs `max77705-usbc/orientation` shows both.
  This is how the select polarity gets settled. It is optional, and a board
  without it loses only the log.
- **Node order:** make-dts.py adds these at the end of the USB-C node, so they
  compose with the OTG branch's `vbus-supply` / `usb-role-switch` properties
  on the same node (dtc wants properties before child nodes). Both orders
  were checked with dtc.
- **A later DP controller:** when one exists, the connector gets a
  `displayport = <&dp>` property. typec_displayport then delivers HPD to that
  DRM connector.

### Expander bus and the SBU/AUX switch

```dts
expander_i2c: i2c-expander {
	compatible = "i2c-gpio";
	sda-gpios = <&gpp6 3 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN)>;
	scl-gpios = <&gpp6 2 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN)>;
	i2c-gpio,delay-us = <5>;
	expander: gpio@22 {
		compatible = "nxp,pcal6524";	/* gpio-pca953x */
		reg = <0x22>;
		gpio-controller;
		#gpio-cells = <2>;
	};
};

bluetooth-board {
	compatible = "ztos,zrn-bt";
	bt-en-gpios = <&expander 7 GPIO_ACTIVE_HIGH>;
};

usb-sbu-mux {
	compatible = "gpio-sbu-mux";
	enable-gpios = <&expander 4 GPIO_ACTIVE_LOW>;	/* AUX_SW_OE */
	select-gpios = <&expander 1 GPIO_ACTIVE_HIGH>;	/* SBU_SW_SEL: 1 = reverse */
	orientation-switch;
	mode-switch;
	port { sbu_mux_in: endpoint { remote-endpoint = <&usbc_sbu>; }; };
};
```

- **gpio-sbu-mux behaviour:** the mainline driver turns the switch off at
  probe and when the cable is unplugged. It sets select from the CC
  orientation the MAX77705 reports, and enables the switch only in DP pin
  states C/D/E. Polarity lives in the GPIO flags only.
- **zrn-bt:**
  - **With the node:** it takes BT_EN from `bt-en-gpios` and creates no bus of
    its own.
  - **If the GPIO cannot be had:** zrn-bt fails rather than guessing.
  - **Without the node:** a tree with no `ztos,zrn-bt` node (images built
    without `--dp-altmode`) keeps zrn-bt's old bit-banged bus.
  - **No mixing:** one image never uses both, because they share the pins.
- **i2c-gpio is a module:** so the expander, the switch and with them the
  typec port appear only once it is loaded. max77705-usbc retries the port
  registration every second, for up to 2 minutes; charging does not wait for
  it.
- **The compatible:** the binding lists a vendor part before
  `gpio-sbu-mux`. The S9's switch part is not known, so the tree has the
  generic compatible only.

### Still to describe (M2/M3)

These are listed so that the next drivers take them from the DT as well:

| What | S9 value | Source |
|---|---|---|
| DP controller | reg 0x11090000 size 0x10000, SPI 248, pd-dpu, iommus = the DPU SysMMUs | stock DT `displayport@0x11090000` |
| combo PHY (USB3/DP) | reg 0x110a0000 (PMA), 0x110b0000 (PCS) | stock DT `usbdrd_phy0` reg[1..2] |
| PHY isolation | PMU 0x704 bit 0 (also set today via `s9p,or-registers`) | stock DT `isolation = <0x704>`, `pmu_offset_dp` |
| orientation input | gpg0-4, input, pull-up | stock DT `dp,usb_con_sel`, `phy,gpio_phy_port` |
| lane map | C/D/E/F: (3,2,0,1) with gpg0-4 high, else (0,1,3,2) | stock `displayport_reg.c:1193-1238` |
| max link | HBR2, 4 lanes (2 with pin D) | stock `displayport.h:191-193` |
| external DECON | DECON2 0x16050000, SPI 194/195/196, pd-dpu, out_type DP | stock DT `decon_t` |

## Where each value comes from

Verified on the phone (read-only, chg11/chg12/dp2):

- **MAX77705 USB-C firmware:** 0x32. PD_INT_M 0x1c (DP bits masked) and
  VDM_INT_M 0xff before the driver touches them.
- **PCAL6524 at 0x22:** answers on the bit-banged gpp6-2/gpp6-3 bus (zrn-bt
  has used it since the Bluetooth work). Its registers:
  - output 0xff 0xff 0xff;
  - polarity 0x00 0x00 0x00, so gpio-pca953x's "clear polarity inversion"
    at probe writes what is already there;
  - configuration 0x7f 0xff 0xff: every pin is an input except pin 7
    (BT_EN). Pins 1 and 4 were not driven before this work, and pin 8
    (WPC_EN) is an input that nothing in the tree claims.
- **Orientation input:** gpg0-4 reads 1 with CC1 (normal) and 0 with CC2
  (reverse).
- **M1 on the user's powered dock (VIA Labs 2109:0102, HDMI out), on dp2:**
  the dock DR-swaps the phone to DFP while it sinks. Discover Identity,
  SVIDs and Modes come back (first SVID ff01, mode 0x0c05: UFP_D, pins
  C/D). Enter Mode is ACKed, DP Status is 0x1a, and Configure pin D
  (0x806) is ACKed.
- **Typec port and DP altmode (chg12):** `/sys/class/typec/port0` exists, and
  the DP port altmode registers (svid ff01).

From Samsung's stock G960F device tree (rev 26) and kernel, not yet
verified on the phone:

- **SBU/AUX pins:** pin 4 AUX_SW_OE, pin 1 SBU_SW_SEL, gpg0-4 USB_CON_SEL
  (`dp,aux_sw_oe`, `dp,sbu_sw_sel`, `dp,usb_con_sel`).
- **AUX_SW_OE polarity:** active low. Stock drives 1 at probe and on
  detach, and 0 to turn the switch on (`displayport_drv.c:2784-2792`,
  `:2867-2901`, `:2953`).
- **SBU_SW_SEL level:** stock sets it to `!gpg0-4`. Settled on the phone
  (dp2, `orientation-sense`, one plug each way round): gpg0-4 reads 1 with
  CC1 (normal) and 0 with CC2 (reverse). Stock's select is therefore 1 for
  reverse, which is gpio-sbu-mux with `GPIO_ACTIVE_HIGH` on select.
- **The VDMs, pin choice and read lengths:** from stock
  `max77705_alternate.c`.

## For another board (e.g. Note 9, N960F)

1. Check, in its stock DT, the `displayport` node's `dp,aux_sw_oe`,
   `dp,sbu_sw_sel` and `dp,usb_con_sel`, and the expander they point to. Also
   check the `usbdrd_phy0` node for its combo PHY.
2. Write the same nodes with its pins, bus and flags. If its switch is not on
   an expander, the `gpio-sbu-mux` GPIOs simply point at SoC pins.
3. Set the connector's DP vdo to the pin assignments its lanes carry.
4. Build with its own make-dts equivalent and `--dp-altmode`. No driver
   change should be needed. If one is, the driver has board data in it, and
   that is a bug to fix.
