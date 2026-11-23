/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zrn-i2c20: owner of the Galaxy S9's stock "hsi2c20" bus (gpp5-0 SCL,
 * gpp5-1 SDA) and of its supply, S2MPS18 LDO38 (VDD_NFC_1P8). Clients on it:
 * the S3NRN82 NFC controller (0x27) and the heart-rate sensor (0x57).
 *
 * A client calls zrn_i2c20_get() before it talks to its chip and
 * zrn_i2c20_put() when done. The first get switches LDO38 on (mode 2, "on
 * while awake") if it was off; the last put restores the exact value read
 * before. The I2C core serialises the clients' transfers on the adapter.
 */
#ifndef ZRN_I2C20_H
#define ZRN_I2C20_H

#include <linux/i2c.h>

#define ZRN_I2C20_BUS	103

/* returns the bus adapter (reference held), or an ERR_PTR */
struct i2c_adapter *zrn_i2c20_get(void);
void zrn_i2c20_put(struct i2c_adapter *adap);

#endif
