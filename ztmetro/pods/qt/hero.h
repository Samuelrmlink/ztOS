// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The status tab's picture: the buds and the case, a battery ring under each, and its
 * level. Shapes, colours and layout are ../pods-art.h, the same as ztMETRO's page.
 */
#pragma once
#include <QWidget>

#include "pods-art.h"

class PodsHero : public QWidget {
	Q_OBJECT
public:
	/* pro: ear tips on the buds. lit: the case's light on (its lid open). */
	PodsHero(const struct pods_ring *rings, int n, bool pro, bool lit);
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override;
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	struct pods_ring r[3];
	int n;
	bool pro, lit;
};
