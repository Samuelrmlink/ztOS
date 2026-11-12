#!/usr/bin/env python3
"""Derive the Galaxy S9 (starlte, SM-G960) device tree from the S9+ port's.

  make-dts.py <exynos9810-star2lte.dts> <out.dts> [--ufs] [--power] [--touch]
              [--gpu] [--decon] [--wlan] [--fallback boot|recovery]

The two phones share the board design; what differs is listed here, and every
edit is asserted, so a changed upstream file fails the build instead of
silently producing an S9+ tree.

  memory     4 GB: the bank at 0x9_00000000 does not exist (bootloader FDT)
  battery    3000 mAh, 4.35 V float (stock battery,chg_float_voltage = 4350)
  fallback   which INFORM3 magic the kernel arms first thing. "boot" means a
             watchdog reset or panic comes up from BOOT; "recovery" from
             RECOVERY. Point it at whichever one holds TWRP.

Blocks are off unless asked for, because a read into a gated or unpowered
block on this SoC is a silent bus hang rather than an error:

  --ufs      the UFS host (the root filesystem lives there)
  --power    MAX77705 PMIC, charger and fuel gauge
  --touch    the S6SY761 touch controller (its rails are cycled by s9p-acpm)
  --gpu      Mali-G72, its power domain and the G3D clock provider
  --decon    DECON scanout, taking the display from the bootloader framebuffer
  --wlan     the PCIe root complex the BCM4361 sits on
  --dp-altmode  (with --power) the USB-C connector and what DisplayPort Alt
             Mode needs of the board: the PCAL6524 expander on its bit-banged
             bus as a gpiochip (shared with zrn-bt's BT_EN), the SBU/AUX
             switch on two of its pins, the connector's DP altmode and graph.
             docs/dp-altmode.md has the binding notes and where each value
             comes from
  --hall     the flip cover's hall sensor as a lid switch (SW_LID) on
             gpio-keys: gpa3-6, low while closed, both edges wake (stock
             DT node "hall"; sensors/COVERS.md). Until a boot image with it
             is flashed, sensors/hall/zrn-hall.ko adds the same node at run
             time
"""
import argparse
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("src")
ap.add_argument("out")
ap.add_argument("--ufs", action="store_true")
ap.add_argument("--power", action="store_true")
ap.add_argument("--touch", action="store_true")
ap.add_argument("--gpu", action="store_true")
ap.add_argument("--decon", action="store_true")
ap.add_argument("--wlan", action="store_true")
ap.add_argument("--dp-altmode", action="store_true")
ap.add_argument("--dp-tx", action="store_true")
ap.add_argument("--hall", action="store_true")
ap.add_argument("--fallback", choices=("boot", "recovery"), default="boot")
args = ap.parse_args()

s = Path(args.src).read_text()


def sub(old, new, count=1):
    global s
    if s.count(old) != count:
        sys.exit(f"make-dts: expected {count}x {old!r}, found {s.count(old)}")
    s = s.replace(old, new)


def enable(marker, count=1):
    """Turn on a node the S9+ build switches with a sed of its own."""
    sub(f'status = "disabled"; /* {marker}-STATUS */',
        f'status = "okay"; /* {marker}-STATUS */', count)


def disable(anchor, count=1):
    """Turn off a node that is on in the source, after its last property."""
    sub(anchor, anchor + '\n\t\t\tstatus = "disabled";', count)


sub('model = "Samsung Galaxy S9+ (SM-G965F)";', 'model = "Samsung Galaxy S9 (SM-G960)";')  # one tree for F and N: ztos-variant knows which
sub('compatible = "samsung,star2lte", "samsung,exynos9810";',
    'compatible = "samsung,starlte", "samsung,exynos9810";')

sub("""		      <0x8 0x80000000 0x80000000>,
		      <0x9 0x00000000 0x80000000>;""",
    """		      <0x8 0x80000000 0x80000000>;""")

sub("energy-full-design-microwatt-hours = <13480000>;", "energy-full-design-microwatt-hours = <11550000>;")
sub("charge-full-design-microamp-hours = <3500000>;", "charge-full-design-microamp-hours = <3000000>;")
sub("voltage-max-design-microvolt = <4400000>;", "voltage-max-design-microvolt = <4350000>;")
sub("constant-charge-current-max-microamp = <2150000>;", "constant-charge-current-max-microamp = <1500000>;")
sub("constant-charge-voltage-max-microvolt = <4400000>;", "constant-charge-voltage-max-microvolt = <4350000>;")

if args.fallback == "boot":
    sub("<0x1406080c 0x12345674>,	/* PMU INFORM3 = recovery */",
        "<0x1406080c 0x12345670>,	/* PMU INFORM3 = normal boot: TWRP sits on BOOT */")

if args.ufs:
    enable("UFS", count=2)
if args.gpu:
    enable("GPU")
if args.decon:
    enable("DECON")
if args.wlan:
    enable("WLAN")
    s += '''
/* PCI restores endpoint configuration in resume_noirq. Restore its GPIO
 * providers first, including WL_REG_ON and PERST# mux/data registers. */
&pinctrl_fsys1 { samsung,noirq-resume; };
&pinctrl_peric0 { samsung,noirq-resume; };
'''

if not args.power:
    disable("""			interrupt-controller;
			#interrupt-cells = <1>;""")
    disable("""			interrupt-parent = <&max77705>;
			interrupts = <0>;""")
    disable("""			interrupt-parent = <&max77705>;
			interrupts = <2>;""")

if args.power:
    # --- MAX77705 charger: the wireless input (charger agent's block) ---
    # kernel/patch-max77705-charger.py registers max77705-wireless (WCIN) on a
    # "wireless" child node of the charger, so the fuel gauge can list it as a
    # supplier and read "Charging" on a pad. Stock's pull-down on WPC_DET
    # (gpa2-0) is set by the P9320 receiver's wpc_det pins below (one owner).
    sub("""			monitored-battery = <&battery>;
			interrupt-parent = <&max77705>;
			interrupts = <0>;""", """			monitored-battery = <&battery>;
			interrupt-parent = <&max77705>;
			interrupts = <0>;

			max77705_wireless: wireless {
			};""")
    sub("""			power-supplies = <&max77705_charger>;""",
        """			power-supplies = <&max77705_charger>, <&max77705_wireless>;""")

    # ---- wireless charging: the IDT P9320 receiver (kernel/p9320_wireless.c) ----
    # Its own bus is hsi2c@14370000, the I2C-only half of USI_CMGP03 (stock
    # hsi2c41; the USI half at 0x14360000 is the PMIC bus): GIC SPI 7, 100 kHz,
    # pins gpm16-0/gpm17-0 function 3. Clocked from the same mux as the PMIC
    # bus (MUX_CLK_I2C_CMGP = CMGP bus, DIV 1, read on the phone), so the
    # 200 MHz stub holds. Its gates (cmucal-sfr, CMGP base 0x14200000) are
    # forced on like the PMIC bus's, before its USI_CON reset is released
    # (stock's "samsung,usi-i2c-v2" does that in the I2C driver).
    # WPC_DET gpa2-0 gets stock's pull-down (it floats without one), WPC_INT
    # gpa3-1 stock's no-pull. WPC_EN (PCAL6524 pin 8, high = receiver off)
    # is not described: the expander is zrn-bt's, and the pin must stay an
    # input reading low, as stock leaves it at boot.
    sub("""				     <0x142020ac>,	/* USI_CMGP03 PCLK port */
""", """				     <0x142020ac>,	/* USI_CMGP03 PCLK port */
				     <0x14202004>,	/* GATE_CLK_I2C_CMGP */
				     <0x14202044>,	/* I2C_CMGP03 IPCLK port */
				     <0x14202048>,	/* I2C_CMGP03 PCLK port */
""")
    sub("""				    <0x143600c4 0x0>;	/* USI_CMGP03 USI_CON: reset off */""",
        """				    <0x143600c4 0x0>,	/* USI_CMGP03 USI_CON: reset off */
				    <0x143700c4 0x0>;	/* I2C_CMGP03 USI_CON: reset off */""")
    sub("""				   <0x143600c8 0x2>;	/* USI_CMGP03 USI_OPTION: CLKREQ_ON */""",
        """				   <0x143600c8 0x2>,	/* USI_CMGP03 USI_OPTION: CLKREQ_ON */
				   <0x143700c8 0x2>;	/* I2C_CMGP03 USI_OPTION: CLKREQ_ON */""")
    s += '''
/* ---- wireless charging: IDT P9320 receiver (make-dts.py --power) ---- */
/ {
	/* the new bus must not take i2c-1 (the PMIC's) if a probe is deferred */
	aliases {
		i2c0 = &hsi2c_tsp;
		i2c1 = &hsi2c_pmic;
		i2c2 = &hsi2c_wpc;
	};
};

&soc {
	hsi2c_wpc: i2c@14370000 {
		compatible = "samsung,exynosautov9-hsi2c";
		reg = <0x14370000 0x1000>;
		interrupts = <GIC_SPI 7 IRQ_TYPE_LEVEL_HIGH>;
		#address-cells = <1>;
		#size-cells = <0>;
		clocks = <&hsi2c_clk_stub>, <&hsi2c_clk_stub>;
		clock-names = "hsi2c", "hsi2c_pclk";
		clock-frequency = <100000>;	/* stock */
		pinctrl-names = "default";
		pinctrl-0 = <&hsi2c41_bus>;
		status = "okay";

		/*
		 * Powered from the coil: answers only on a pad. FOD tables
		 * are stock rev 18-26 (fod_wpc_data, fod_wpc_data_cv,
		 * fod_hero_5v_data); RPP is stock's wc_hv_rpp default.
		 */
		p9320: wireless-charger@3b {
			compatible = "idt,p9320";
			reg = <0x3b>;
			pinctrl-names = "default";
			pinctrl-0 = <&wpc_int &wpc_det>;
			det-gpios = <&gpa2 0 GPIO_ACTIVE_HIGH>;
			int-gpios = <&gpa3 1 GPIO_ACTIVE_LOW>;
			idt,fod-cc = /bits/ 8 <0x96 0x34 0x8e 0x2c 0x96 0x1e 0x96 0x1e 0xa5 0xf5 0x9c 0x13>;
			idt,fod-cv = /bits/ 8 <0x96 0x52 0x8e 0x4a 0x96 0x3c 0x96 0x3c 0xa5 0xff 0x9c 0x31>;
			idt,fod-stand-5v = /bits/ 8 <0x96 0xe4 0x8e 0xdc 0x96 0xce 0x96 0xce 0xa5 0xa5 0x9c 0xc3>;
			idt,rpp-scale = <0x40>;
		};
	};
};

&pinctrl_cmgp {
	hsi2c41_bus: hsi2c41-bus-pins {
		samsung,pins = "gpm16-0", "gpm17-0";
		samsung,pin-function = <EXYNOS_PIN_FUNC_3>;
		samsung,pin-pud = <EXYNOS_PIN_PULL_NONE>;
		samsung,pin-drv = <EXYNOS5420_PIN_DRV_LV1>;
	};
};

&pinctrl_alive {
	wpc_int: wpc-int-pins {
		samsung,pins = "gpa3-1";
		samsung,pin-function = <EXYNOS_PIN_FUNC_EINT>;
		samsung,pin-pud = <EXYNOS_PIN_PULL_NONE>;
		samsung,pin-drv = <EXYNOS5420_PIN_DRV_LV1>;
	};

	wpc_det: wpc-det-pins {
		samsung,pins = "gpa2-0";
		samsung,pin-function = <EXYNOS_PIN_FUNC_EINT>;
		samsung,pin-pud = <EXYNOS_PIN_PULL_DOWN>;
	};
};
'''
    # ---- end of wireless charging ----
# ---- MAX77705 USB-C controller (usbc agent, kernel/max77705-usbc.c) --------
# The USB-C MCU at 0x25 gets the IRQ-driven driver instead of the port's
# polling s9p-max77705-muic: hwirq 3 of the MFD's domain is its line (USBC
# in INTSRC). Its max77705-usbc power_supply names max77705-charger in
# supplied_to itself: a power-supplies phandle on the charger node would make
# the charger (and the gauge behind it) defer until this driver probes.
# Without --power the MFD's interrupt controller is off, so the old node and
# driver stay as they are.
if args.power:
    sub("""		muic@25 {
			compatible = "samsung,s9p-max77705-muic";
			reg = <0x25>;
		};""", """		max77705_usbc: usb-c@25 {
			compatible = "maxim,max77705-usbc";
			reg = <0x25>;
			interrupt-parent = <&max77705>;
			interrupts = <3>;
		};""")
# ---- end MAX77705 USB-C controller -----------------------------------------
# what goes inside the USB-C node: properties first, then the connector child.
# Inserted before the node's closing brace, so it composes with other edits
# that add properties after "interrupts = <3>;" (the OTG branch's
# vbus-supply / usb-role-switch): dtc wants properties before subnodes, and
# those land above this either way.
USBC_INSIDE = """
			/*
			 * Read-only: the board's CC-orientation input (stock
			 * dp,usb_con_sel / phy,gpio_phy_port, gpg0-4). Logged
			 * next to the orientation the MCU reports, to settle the
			 * SBU select polarity; never driven.
			 */
			orientation-sense-gpios = <&gpg0 4 GPIO_ACTIVE_HIGH>;

			usbc_connector: connector {
				compatible = "usb-c-connector";
				label = "USB-C";
				power-role = "dual";
				data-role = "dual";

				altmodes {
					displayport {
						svid = /bits/ 16 <0xff01>;
						vdo = <0x00001c46>;
					};
				};

				ports {
					#address-cells = <1>;
					#size-cells = <0>;

@PORT1@					port@2 {	/* SBU */
						reg = <2>;
						usbc_sbu: endpoint {
							remote-endpoint = <&sbu_mux_in>;
						};
					};
				};
			};
"""


def usbc_append(inside):
    """Add to the end of the max77705_usbc node, before its closing brace."""
    global s
    head = "		max77705_usbc: usb-c@25 {\n"
    if s.count(head) != 1:
        sys.exit("make-dts: the max77705_usbc node is not there once")
    i = s.index(head)
    close = s.index("\n		};", i)
    s = s[:close] + "\n" + inside.rstrip("\n") + s[close:]


# --dp-tx: the DP TX (kernel/exynos9810-dp.c) as a mode/orientation switch on
# the connector's SuperSpeed port. Without the driver built the port would
# wait for that switch for ever, so the two go together.
DP_TX_PORT = """					port@1 {	/* SuperSpeed lanes: the combo PHY, DP side */
						reg = <1>;
						usbc_ss: endpoint {
							remote-endpoint = <&dp_ss_in>;
						};
					};

"""
DP_TX_NODE = """
&soc {
	/*
	 * DP TX (kernel/exynos9810-dp.c, bring-up). Stock: displayport@0x11090000
	 * (reg 0x11090000 size 0xffff, SPI 248, pd-dpu), the DP side of the
	 * combo PHY at 0x110a0000 (usbdrd_phy0 reg[1]), PHY isolation PMU 0x704
	 * bit 0 (isolation = <0x704>; 1 = not isolated). Lane maps per CC
	 * orientation, normal first: stock displayport_reg.c:1225-1230 with
	 * dp_sw_sel = gpg0-4, which reads 1 with CC1 (normal) on this phone; a
	 * 2-lane assignment (pin D) uses PMA lanes 2+3 normal, 0+1 reverse
	 * (:189-217).
	 */
	displayport@11090000 {
		compatible = "samsung,exynos9810-dp";
		reg = <0x11090000 0x10000>, <0x110a0000 0x1000>,
		      <0x11003014 0x8>,		/* CMU_FSYS0 QCH_CON_DP_LINK_QCH, _GTC */
		      <0x16050000 0x10000>,	/* DECON2 (stock decon_t) */
		      <0x16072000 0x1000>,	/* DPU_DMA channel 1 (stock idma_g1 reg[1]) */
		      <0x16022000 0x1000>,	/* DPP channel 1 (stock idma_g1 reg[0]) */
		      <0x16070000 0x110>,	/* DPU_DMA common (stock idma_g* reg[2]) */
		      <0x16000000 0x4000>,	/* CMU_DPU: gates and Q-channels, read only */
		      <0x16030270 0x4>,		/* DECON0 DP_CONNECTION_CONTROL (M4 W1) */
		      <0x16037270 0x4>,		/* its shadow copy (SHD_DECON), read only */
		      <0x16011010 0x4>,		/* disp sysreg DISP_DPU_TE_QACTIVE_PLL_EN (W2) */
		      <0x160310c0 0x1c>,	/* DECON0-space blender window 4, for DECON2 */
		      <0x16073000 0x1000>,	/* DPU_DMA channel 2 (IDMA_VG0, DPU1 bus port) */
		      <0x16023000 0x1000>;	/* DPP channel 2 (IDMA_VG0) */
		reg-names = "link", "phy", "link-qch", "decon2", "dma1", "dpp1", "dma-common",
			    "dpu-cmu", "decon0-dp-conn", "decon0-dp-conn-shadow", "disp-sysreg-te",
			    "decon0-win4", "dma2", "dpp2";
		interrupts = <GIC_SPI 248 IRQ_TYPE_LEVEL_HIGH>;
		samsung,usb-phy = <&usbdrd_phy>;
		samsung,pmu-syscon = <&pmu_system_controller>;
		samsung,phy-isolation = <0x704 0x1>;
		samsung,link-power-status = <0x4024 0xf>;	/* pd-dpu STATUS: 0xf = on */
		samsung,lane-map = <3 2 0 1>, <0 1 3 2>;
		samsung,two-lane-lanes = <2 3>, <0 1>;
		orientation-switch;
		mode-switch;

		port {
			dp_ss_in: endpoint {
				remote-endpoint = <&usbc_ss>;
			};
		};
	};
};
"""
ROOT_NODES = """
/* ---- USB-C SBU switch and the PCAL6524 expander (make-dts.py --dp-altmode) ---- */
/ {
	aliases {
		i2c96 = &expander_i2c;
	};

	expander_i2c: i2c-expander {
		compatible = "i2c-gpio";
		sda-gpios = <&gpp6 3 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN)>;
		scl-gpios = <&gpp6 2 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN)>;
		i2c-gpio,delay-us = <5>;
		#address-cells = <1>;
		#size-cells = <0>;

		expander: gpio@22 {
			compatible = "nxp,pcal6524";
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
		select-gpios = <&expander 1 GPIO_ACTIVE_HIGH>;	/* SBU_SW_SEL: 1 = reverse (CC2) */
		orientation-switch;
		mode-switch;

		port {
			sbu_mux_in: endpoint {
				remote-endpoint = <&usbc_sbu>;
			};
		};
	};
};
"""

# ---- USB-C connector and DisplayPort Alt Mode (--dp-altmode) ----------------
# Board wiring, all of it here and none in the drivers (docs/dp-altmode.md):
#   PCAL6524 at 0x22: stock hsi2c@10930000 (hsi2c_25), which this kernel does
#     not bring up; zrn-bt has reached it by bit-banging gpp6-3 SDA / gpp6-2
#     SCL since the Bluetooth work, so the bus is the same i2c-gpio, now from
#     the DT (alias i2c96: the number tools used so far). Pin 7 BT_EN (zrn-bt),
#     pin 1 SBU_SW_SEL (stock dp,sbu_sw_sel), pin 4 AUX_SW_OE (stock
#     dp,aux_sw_oe; stock drives 1 = switch off at probe and on detach, 0 =
#     on, after selecting: active low). Pin 8 WPC_EN must stay an input: no
#     consumer here, and gpio-pca953x leaves unclaimed pins as they are.
#   SBU switch: mainline gpio-sbu-mux (enable = AUX_SW_OE, select = the CC
#     orientation). Stock drives select = !gpg0-4; on the phone (dp2) gpg0-4
#     reads 1 with CC1 (normal) and 0 with CC2 (reverse), so select is 1 for
#     reverse: active high.
#   connector: the usb-c-connector binding under the MAX77705 USB-C node; the
#     typec port is registered on it. altmodes/displayport is the port's DP
#     capability VDO: DFP_D (2) | DP signalling (bit 2) | receptacle (bit 6) |
#     DFP_D pin assignments C, D, E (bits 10-12) = 0x1c46.
if args.dp_altmode:
    if not args.power:
        sys.exit("make-dts: --dp-altmode needs --power (the MAX77705 USB-C node)")
    usbc_append(USBC_INSIDE.replace("@PORT1@", DP_TX_PORT if args.dp_tx else ""))
    s += ROOT_NODES
    if args.dp_tx:
        s += DP_TX_NODE
elif args.dp_tx:
    sys.exit("make-dts: --dp-tx needs --dp-altmode")
# ---- end USB-C connector and DisplayPort Alt Mode ---------------------------

# ---- USB OTG (otg agent): the board's host-mode wiring ----------------------
# Stock wires it the same way: dwc3 dr_mode "otg" (usbdrd_dwc3), VBUS out
# from the MAX77705 charger's CHGIN boost ("otg" power supply, stock
# max77705_set_otg), the role from the USB-C MCU (ccic notifier -> dwc3).
# Here: the charger's usb-otg-vbus child is the boost as a regulator
# (kernel/patch-max77705-otg.py), the USB-C controller takes it as its
# vbus-supply and drives dwc3's role switch (kernel/max77705-usbc.c).
# Device mode stays the default, so the gadget is there whenever nothing
# asks for host. Needs --power: without the USB-C driver nothing would ever
# switch the role.
if args.power:
    sub("""			max77705_wireless: wireless {
			};""", """			max77705_wireless: wireless {
			};

			max77705_otg: usb-otg-vbus {
				regulator-name = "max77705-otg";
				regulator-min-microvolt = <5000000>;
				regulator-max-microvolt = <5000000>;
			};""")
    sub("""			compatible = "maxim,max77705-usbc";
			reg = <0x25>;
			interrupt-parent = <&max77705>;
			interrupts = <3>;""", """			compatible = "maxim,max77705-usbc";
			reg = <0x25>;
			interrupt-parent = <&max77705>;
			interrupts = <3>;
			vbus-supply = <&max77705_otg>;
			usb-role-switch = <&usb>;""")
    sub("""		dr_mode = "peripheral";""", """		dr_mode = "otg";
		usb-role-switch;
		role-switch-default-mode = "peripheral";""")
# ---- end USB OTG ------------------------------------------------------------

if not args.touch:
    disable("			touchscreen-size-y = <4096>;")

if not args.gpu:
    # A power domain with no consumer is powered off by genpd, and that
    # sequence is only validated with the GPU running.
    sub("""		#power-domain-cells = <0>;
		label = "G3D";""", """		#power-domain-cells = <0>;
		label = "G3D";
		status = "disabled";""")
    sub("""		clock-output-names = "g3d";""", """		clock-output-names = "g3d";
		status = "disabled";""")

if args.hall:
    # the flip cover: as zrn-hall.ko's node (sensors/hall/zrn-hall.conf). The
    # pull stays as the bootloader left it; stock sets pull-down (pud 1)
    sub("""			gpios = <&gpa0 6 GPIO_ACTIVE_LOW>;
		};
	};
""", """			gpios = <&gpa0 6 GPIO_ACTIVE_LOW>;
		};

		lid-switch {
			label = "Flip cover";
			linux,input-type = <EV_SW>;
			linux,code = <SW_LID>;
			gpios = <&gpa3 6 GPIO_ACTIVE_LOW>;
			debounce-interval = <15>;
			wakeup-source;
		};
	};
""")

Path(args.out).write_text(s)
print("make-dts: {} (ufs={} power={} touch={} gpu={} decon={} wlan={} dp_altmode={} fallback={})".format(
    args.out, args.ufs, args.power, args.touch, args.gpu, args.decon, args.wlan, args.dp_altmode,
    args.fallback))
