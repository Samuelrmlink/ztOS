/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thermistor decoding as stock does it, shared by zrn-therm.c and the host
 * test (test-decode.c), so it has no kernel or libc dependency.
 *
 * - therm_trim_mean(): sec_bat_get_adc_data() in Samsung's sec_adc.c: the
 *   highest and the lowest of the samples dropped, the rest averaged.
 * - therm_decode(): sec_bat_get_value_by_adc(): a binary search of the table
 *   (ADC rising), then linear interpolation in integer arithmetic. Stock
 *   clamps past either end of the table; here a value past either end is
 *   unknown instead (an open or shorted thermistor, or no conversion), so
 *   nothing outside the table is ever reported.
 * - therm_gauge_reg(): the MAX77705 fuel gauge's Temp register as stock
 *   writes it (max77705_fg_write_temp): whole degrees in the high byte, the
 *   tenths as 1/256 C (x 1000 / 39) in the low byte. Below 0 C stock's
 *   arithmetic breaks; there the register gets plain two's complement in
 *   1/256 C, which is what the gauge expects.
 */
#ifndef THERM_DECODE_H
#define THERM_DECODE_H

struct therm_table {
	const int *adc;		/* rising */
	const int *deci;	/* tenths of a degree C at each adc point */
	int n;
};

#define THERM_SAMPLES 5		/* stock's battery,adc_check_count */

/* the trimmed mean of count (>= 3) samples, stock's arithmetic */
static inline int therm_trim_mean(const int *s, int count)
{
	int max = s[0], min = s[0], total = s[0], i;

	for (i = 1; i < count; i++) {
		if (s[i] > max)
			max = s[i];
		else if (s[i] < min)
			min = s[i];
		total += s[i];
	}
	return (total - max - min) / (count - 2);
}

/* 0 and *deci set, or -1: adc outside the table */
static inline int therm_decode(const struct therm_table *t, int adc, int *deci)
{
	int low = 0, high = t->n - 1, mid;

	if (t->n < 2 || adc < t->adc[0] || adc > t->adc[t->n - 1])
		return -1;
	if (adc == t->adc[0]) {
		*deci = t->deci[0];
		return 0;
	}
	if (adc == t->adc[t->n - 1]) {
		*deci = t->deci[t->n - 1];
		return 0;
	}
	while (low <= high) {
		mid = (low + high) / 2;
		if (t->adc[mid] > adc) {
			high = mid - 1;
		} else if (t->adc[mid] < adc) {
			low = mid + 1;
		} else {
			*deci = t->deci[mid];
			return 0;
		}
	}
	*deci = t->deci[high] + ((t->deci[low] - t->deci[high]) * (adc - t->adc[high])) /
		(t->adc[low] - t->adc[high]);
	return 0;
}

/* the gauge's Temp register (16 bits) for a temperature in tenths of a degree */
static inline unsigned int therm_gauge_reg(int deci)
{
	if (deci >= 0)
		return ((unsigned int)(deci / 10) << 8) | (unsigned int)((deci % 10) * 1000 / 39);
	return (unsigned int)(deci * 256 / 10) & 0xffff;
}

/* what mainline's max17042 reads back from that register, tenths of a degree */
static inline int therm_gauge_read(unsigned int reg)
{
	int v = (int)(reg & 0xffff);

	if (v & 0x8000)
		v -= 0x10000;
	return v * 10 / 256;
}
#endif
