#!/usr/bin/env python3
"""Write the S9's thermistor decoding tables, from Samsung's device tree, as data.

    python3 -I camera/iris/stock-dt.py stock/BOOT.img OUT     # research/iris
    python3 -I power/thermistors/extract.py OUT/dtb5_1fd7000.txt > power/thermistors/starlte-rev26.json

The input is the text dump of stock's rev26 tree (the F's board: hw_rev 0x1a,
docs/g960f-differences.md). Every value comes from that tree; the notes on
what reads each channel and what stock does with it come from Samsung's
kernel source (drivers/battery_v2/sec_adc.c, sec_battery.c,
drivers/iio/adc/exynos_adc.c) and DeviceKeystring's BatteryStatusEng page,
and are in README.md.

Decoding, as sec_bat_get_value_by_adc() and the sec-thermistor driver do it:
average the raw 12-bit ADC (adc_check_count reads, the highest and lowest
dropped), then interpolate linearly between the two table points around it;
below the first ADC point or above the last, the end value. Temperatures are
in tenths of a degree C, and fall as the ADC value rises (NTC to ground).
"""
import json
import re
import sys


def cells(txt, node, prop):
    """<0x..> cells of prop inside the first node whose header line is node."""
    m = re.search(r'^(\s*)' + re.escape(node) + r' \{\n(.*?)^\1\};', txt, re.S | re.M)
    if not m:
        sys.exit(f'extract: no node {node}')
    p = re.search(r'^\s*' + re.escape(prop) + r' = <([^>]*)>;', m.group(2), re.M)
    if not p:
        sys.exit(f'extract: no {prop} in {node}')
    return [int(x, 16) for x in p.group(1).split()]


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


def table(txt, node, adc_prop, temp_prop):
    adc = cells(txt, node, adc_prop)
    temp = [signed(v) for v in cells(txt, node, temp_prop)]
    if len(adc) != len(temp):
        sys.exit(f'extract: {node} {adc_prop} has {len(adc)} points, {temp_prop} {len(temp)}')
    return {'adc': adc, 'deci_c': temp}


def io_channels(txt, node):
    v = cells(txt, node, 'io-channels')
    return [v[i + 1] for i in range(0, len(v), 2)]


def main():
    txt = open(sys.argv[1]).read()
    adc_block = 'adc@14230000 (samsung,exynos-adc-v3, 12 bit), clock gate_adcif'
    batt = 'battery'
    ch = io_channels(txt, batt)	# adc-temp, adc-chg-temp, adc-in-bat, adc-wpc-temp, adc-usb-temp
    out = {
        'source': 'stock BOOT.img device tree, Samsung Star rev26 (SM-G960F)',
        'adc_block': adc_block,
        'adc_check_count': cells(txt, batt, 'battery,adc_check_count')[0],
        'thermistors': [
            {'name': 'battery', 'stock_sysfs': 'battery/batt_temp, batt_temp_adc',
             'adc_channel': ch[0], **table(txt, batt, 'battery,temp_table_adc', 'battery,temp_table_data')},
            {'name': 'usb', 'stock_sysfs': 'battery/usb_temp, usb_temp_adc',
             'adc_channel': ch[4], **table(txt, batt, 'battery,usb_temp_table_adc', 'battery,usb_temp_table_data')},
            {'name': 'charger', 'stock_sysfs': 'battery/chg_temp, chg_temp_adc',
             'adc_channel': ch[1], **table(txt, batt, 'battery,chg_temp_table_adc', 'battery,chg_temp_table_data')},
            {'name': 'wpc (coil)', 'stock_sysfs': 'battery/batt_wpc_temp, batt_wpc_temp_adc',
             'adc_channel': ch[3], **table(txt, batt, 'battery,wpc_temp_table_adc', 'battery,wpc_temp_table_data')},
        ],
    }
    for node, sysfs in (('sec_thermistor@0', 'sec/sec-ap-thermistor/temperature, temp_adc'),
                        ('sec_thermistor@2', 'sec/sec-wf-thermistor/temperature, temp_adc'),
                        ('sec_thermistor@10', 'sec/sec-pa-thermistor/temperature, temp_adc')):
        m = re.search(re.escape(node) + r' \{.*?thermistor_name = "([^"]+)"', txt, re.S)
        out['thermistors'].append({
            'name': m.group(1), 'stock_sysfs': sysfs, 'adc_channel': io_channels(txt, node)[0],
            **table(txt, node, 'adc_array', 'temp_array')})
    json.dump(out, sys.stdout, indent=1)
    sys.stdout.write('\n')


main()
