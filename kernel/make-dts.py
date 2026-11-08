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

Path(args.out).write_text(s)
print("make-dts: {} (ufs={} power={} touch={} gpu={} decon={} wlan={} fallback={})".format(
    args.out, args.ufs, args.power, args.touch, args.gpu, args.decon, args.wlan, args.fallback))
