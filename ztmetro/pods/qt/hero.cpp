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

static QPainterPath outline(const struct pods_shape &sh, const struct pods_xf &t) {
	QPainterPath p;
	if (sh.kind == PODS_S_PATH) {
		for (const struct pods_seg *g = sh.path; g->op != PODS_END; g++) {
			const float *q = g->p;
			switch (g->op) {
			case PODS_M: p.moveTo(pods_xf_x(&t, q[0]), pods_xf_y(&t, q[1])); break;
			case PODS_L: p.lineTo(pods_xf_x(&t, q[0]), pods_xf_y(&t, q[1])); break;
			case PODS_C:
				p.cubicTo(pods_xf_x(&t, q[0]), pods_xf_y(&t, q[1]), pods_xf_x(&t, q[2]), pods_xf_y(&t, q[3]),
					pods_xf_x(&t, q[4]), pods_xf_y(&t, q[5]));
				break;
			case PODS_Z: p.closeSubpath(); break;
			}
		}
		return p;
	}
	QRectF box(QPointF(pods_xf_x(&t, sh.x), pods_xf_y(&t, sh.y)), QPointF(pods_xf_x(&t, sh.x + sh.w), pods_xf_y(&t, sh.y + sh.h)));
	if (sh.kind == PODS_S_ELLIPSE) p.addEllipse(box.normalized());
	else p.addRoundedRect(box.normalized(), sh.r * t.s, sh.r * t.s);
	return p;
}

static QBrush paint(int which, const struct pods_xf &t) {
	const struct pods_fill &f = PODS_FILLS[which];
	if (f.n == 1) return rgb(f.rgb[0]);
	QLinearGradient g(pods_xf_x(&t, f.x0), pods_xf_y(&t, f.y0), pods_xf_x(&t, f.x1), pods_xf_y(&t, f.y1));
	for (int i = 0; i < f.n; i++) g.setColorAt(f.at[i], rgb(f.rgb[i]));
	return QBrush(g);
}

static void draw_art(QPainter &p, const struct pods_shape *shapes, int count, const struct pods_xf &t, bool pro, bool lit) {
	/* outlines first, all of them, so where two shapes meet is no line */
	for (int i = 0; i < count; i++) {
		if (shapes[i].edged && !(shapes[i].pro_only && !pro)) {
			p.strokePath(outline(shapes[i], t), QPen(rgb(PODS_C_EDGE), 1.6 * qMax(t.s, 0.7f)));
		}
	}
	for (int i = 0; i < count; i++) {
		const struct pods_shape &sh = shapes[i];
		if (sh.pro_only && !pro) continue;
		int fill = sh.fill == PODS_F_LED_OFF && lit ? int(PODS_F_LED_ON) : int(sh.fill);
		if (sh.kind == PODS_S_LINE) {
			p.setPen(QPen(paint(fill, t), sh.r * t.s, Qt::SolidLine, Qt::FlatCap));
			p.drawLine(QPointF(pods_xf_x(&t, sh.x), pods_xf_y(&t, sh.y)), QPointF(pods_xf_x(&t, sh.x + sh.w), pods_xf_y(&t, sh.y + sh.h)));
			continue;
		}
		p.fillPath(outline(sh, t), paint(fill, t));
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
		struct pods_xf t = { a.x, a.y, a.s, a.art == PODS_ART_CASE ? PODS_CASE_W : PODS_BUD_W, a.art == PODS_ART_BUD_R };
		if (a.art == PODS_ART_CASE) draw_art(p, PODS_CASE, PODS_N_CASE, t, pro, lit);
		else draw_art(p, PODS_BUD, PODS_N_BUD, t, pro, lit);
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
