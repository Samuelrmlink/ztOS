// SPDX-License-Identifier: GPL-3.0-or-later
#include "hero.h"

#include <QPainter>
#include <QPainterPath>

static const float UNIT = 1.0f;	/* pixels to a unit of pods-art.h, at most */

static QColor rgb(unsigned v) { return QColor((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }

PodsHero::PodsHero(const struct pods_ring *rings, int count, bool is_pro, bool is_lit) : n(count), pro(is_pro), lit(is_lit) {
	for (int i = 0; i < n; i++) r[i] = rings[i];
	setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
	QStringList said;
	for (int i = 0; i < n; i++) {
		char text[16];
		pods_ring_text(text, sizeof(text), &r[i]);
		said << QString("%1 %2%3").arg(pods_ring_name(r[i].parts), r[i].b.known ? text : "not known",
			r[i].b.charging ? ", charging" : "");
	}
	setAccessibleName("Battery: " + said.join("; "));
	setToolTip(said.join("\n"));
}

int PodsHero::heightForWidth(int w) const {
	struct pods_hero h;
	pods_hero_layout(&h, r, n, w, UNIT);
	return int(h.h + 0.999f) + 4;
}

QSize PodsHero::sizeHint() const {
	struct pods_hero h;
	pods_hero_layout(&h, r, n, 1e6f, UNIT);
	return QSize(int(h.w) + 1, int(h.h) + 5);
}

static QPainterPath outline(const struct pods_shape &s) {
	QPainterPath p;
	QRectF box(s.x, s.y, s.w, s.h);
	if (s.kind == PODS_S_ELLIPSE) p.addEllipse(box.normalized());
	else p.addRoundedRect(box.normalized(), s.r, s.r);
	return p;
}

static void draw_art(QPainter &p, const struct pods_shape *shapes, int count, float box_w, float box_h, float x, float y,
		float s, bool mirror, bool pro, bool lit) {
	QLinearGradient body(0, y, 0, y + box_h * s);
	body.setColorAt(0, rgb(PODS_C_BODY_TOP));
	body.setColorAt(1, rgb(PODS_C_BODY_BOT));
	/* outlines first, all of them, so where two shapes meet is no line */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < count; i++) {
			struct pods_shape sh = pods_place(&shapes[i], box_w, x, y, s, mirror);
			if (sh.pro_only && !pro) continue;
			bool edged = sh.paint == PODS_P_BODY || sh.paint == PODS_P_TIP;
			if (pass == 0) {
				if (edged) p.strokePath(outline(sh), QPen(rgb(PODS_C_EDGE), 2.0 * qMax(s, 0.6f)));
				continue;
			}
			if (sh.kind == PODS_S_LINE) {
				p.setPen(QPen(rgb(PODS_C_SEAM), sh.r, Qt::SolidLine, Qt::FlatCap));
				p.drawLine(QPointF(sh.x, sh.y), QPointF(sh.x + sh.w, sh.y + sh.h));
				continue;
			}
			QBrush brush;
			switch (sh.paint) {
			case PODS_P_BODY: brush = QBrush(body); break;
			case PODS_P_TIP: brush = rgb(PODS_C_TIP); break;
			case PODS_P_DARK: brush = rgb(PODS_C_DARK); break;
			case PODS_P_METAL: brush = rgb(PODS_C_METAL); break;
			case PODS_P_LED: brush = rgb(lit ? PODS_C_LED_ON : PODS_C_LED_OFF); break;
			default: brush = rgb(PODS_C_SEAM); break;
			}
			p.fillPath(outline(sh), brush);
		}
	}
}

void PodsHero::paintEvent(QPaintEvent *) {
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	struct pods_hero h;
	pods_hero_layout(&h, r, n, width(), UNIT);
	p.translate(0, 2);
	for (int i = 0; i < h.n_art; i++) {
		auto &a = h.art[i];
		if (a.art == PODS_ART_CASE) draw_art(p, PODS_CASE, PODS_N_CASE, PODS_CASE_W, PODS_CASE_H, a.x, a.y, a.s, false, pro, lit);
		else draw_art(p, PODS_BUD, PODS_N_BUD, PODS_BUD_W, PODS_BUD_H, a.x, a.y, a.s, a.art == PODS_ART_BUD_R, pro, lit);
	}
	QColor text = palette().color(QPalette::WindowText);
	QColor track = palette().color(QPalette::WindowText);
	track.setAlphaF(0.14);
	QFont f = font();
	f.setPixelSize(qMax(10, int(h.label_h * 0.8f)));
	p.setFont(f);
	QFontMetricsF fm(f);
	for (int i = 0; i < h.n_rings; i++) {
		auto &g = h.rings[i];
		const struct pods_battery &b = g.ring.b;
		QColor colour = rgb(pods_battery_low(&b) ? PODS_C_LOW : PODS_C_FULL);
		float width = g.r * PODS_RING_WIDTH * 2;
		QRectF box(g.cx - g.r, g.cy - g.r, 2 * g.r, 2 * g.r);
		p.setPen(QPen(track, width));
		p.setBrush(Qt::NoBrush);
		p.drawEllipse(box);
		if (b.known && b.level > 0) {
			p.setPen(QPen(colour, width, Qt::SolidLine, b.level >= 100 ? Qt::FlatCap : Qt::RoundCap));
			p.drawArc(box, 90 * 16, -int(qMin(b.level, 100) * 360 * 16 / 100));
		}
		if (b.charging) {
			QPolygonF bolt;
			for (auto &pt : PODS_BOLT) bolt << QPointF(g.cx + pt[0] * g.r, g.cy + pt[1] * g.r);
			p.setPen(Qt::NoPen);
			p.setBrush(colour);
			p.drawPolygon(bolt);
		}
		/* the line under it: a badge, then the level */
		char level[16];
		pods_ring_text(level, sizeof(level), &g.ring);
		QString t = QString::fromUtf8(level);
		float badge = h.label_h * 0.78f, gap = h.label_h * 0.25f;
		bool has_badge = g.ring.parts == PODS_PART_LEFT || g.ring.parts == PODS_PART_RIGHT || g.ring.parts == PODS_PART_CASE;
		float tw = fm.horizontalAdvance(t), all = tw + (has_badge ? badge + gap : 0);
		float x = g.cx - all / 2, mid = g.label_y + h.label_h / 2;
		if (g.ring.parts == PODS_PART_CASE) {
			QRectF c(x, mid - badge * 0.36f, badge, badge * 0.72f);
			p.setPen(Qt::NoPen);
			p.setBrush(rgb(PODS_C_BADGE));
			p.drawRoundedRect(c, badge * 0.22f, badge * 0.22f);
			p.setPen(QPen(palette().color(QPalette::Window), qMax(1.0f, badge * 0.08f)));
			p.drawLine(QPointF(c.left(), c.top() + c.height() * 0.38f), QPointF(c.right(), c.top() + c.height() * 0.38f));
		} else if (has_badge) {
			QRectF c(x, mid - badge / 2, badge, badge);
			p.setPen(Qt::NoPen);
			p.setBrush(rgb(PODS_C_BADGE));
			p.drawEllipse(c);
			QFont bf = f;
			bf.setBold(true);
			bf.setPixelSize(qMax(7, int(badge * 0.62f)));
			p.setFont(bf);
			p.setPen(palette().color(QPalette::Window));
			p.drawText(c, Qt::AlignCenter, g.ring.parts == PODS_PART_LEFT ? "L" : "R");
			p.setFont(f);
		}
		if (has_badge) x += badge + gap;
		p.setPen(b.known ? text : palette().color(QPalette::PlaceholderText));
		if (pods_battery_low(&b)) p.setPen(colour);
		p.drawText(QRectF(x, g.label_y, tw + 2, h.label_h), Qt::AlignLeft | Qt::AlignVCenter, t);
	}
}
