// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cstring>

#include "hero.h"
#include "podsclient.h"
#include "pods-ui.h"

static const char HERE[] = "this computer";
static const int MAX_NAME = 127;	/* bytes, as the pods keep it */

static QString js(const QJsonObject &o, const char *k, const char *d = "") {
	QJsonValue v = o.value(k);
	return v.isString() ? v.toString() : QString::fromUtf8(d);
}
static long jl(const QJsonObject &o, const char *k, long d) {
	QJsonValue v = o.value(k);
	return v.isDouble() ? long(v.toDouble()) : d;
}
static QJsonObject jo(const QJsonObject &o, const char *k) { return o.value(k).toObject(); }

MainWindow::MainWindow(PodsClient *client, bool with_tray) : c(client) {
	setWindowTitle("AirPods");
	tabs = new QTabWidget(this);
	tabs->setDocumentMode(true);
	static const char *const NAMES[TABS] = { "Status", "Listening", "Controls", "About" };
	for (int i = 0; i < TABS; i++) {
		pages[i] = new QScrollArea;
		pages[i]->setWidgetResizable(true);
		pages[i]->setFrameShape(QFrame::NoFrame);
		pages[i]->setWidget(new QWidget);
		tabs->addTab(pages[i], NAMES[i]);
	}
	setCentralWidget(tabs);
	statusBar();
	connect(c, &PodsClient::replied, this, &MainWindow::said);

	connect(&poll, &QTimer::timeout, this, &MainWindow::tick);
	poll.start(1000);
	/* "someone is looking": short bursts of listening, for as long as it is said */
	connect(&hinter, &QTimer::timeout, this, &MainWindow::hint);
	hinter.start(10000);
	redrawn.start();

	if (with_tray && QSystemTrayIcon::isSystemTrayAvailable()) {
		tray = new QSystemTrayIcon(windowIcon(), this);
		QMenu *menu = new QMenu(this);
		menu->addAction("Show", this, [this] { showNormal(); activateWindow(); });
		menu->addAction("Quit", this, [this] { quitting = true; qApp->quit(); });
		tray->setContextMenu(menu);
		connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
			if (r != QSystemTrayIcon::Trigger) return;
			if (isVisible() && !isMinimized()) hide();
			else { showNormal(); activateWindow(); }
		});
		tray->show();
	}
	resize(560, 720);
	rebuild();
}

/* ---- the clock ---- */

bool MainWindow::looking() const { return isVisible() && !isMinimized(); }

void MainWindow::hint() {
	if (looking()) c->scan_hint(true);
}

void MainWindow::tick() {
	if (QApplication::activePopupWidget()) return;	/* a list open: not pulled from under the pointer */
	bool changed = false;
	if (!c->changed_here_lately() && !name_editing && !slider_held) changed = c->load(false);
	if (changed || redrawn.elapsed() > 30000) later();	/* the ages move on by themselves */
}

void MainWindow::showEvent(QShowEvent *e) {
	QMainWindow::showEvent(e);
	hint();
}

void MainWindow::hideEvent(QHideEvent *e) {
	QMainWindow::hideEvent(e);
	if (!looking()) c->scan_hint(false);
}

void MainWindow::changeEvent(QEvent *e) {
	QMainWindow::changeEvent(e);
	if (e->type() == QEvent::WindowStateChange) {
		if (isMinimized()) c->scan_hint(false);
		else hint();
	}
}

void MainWindow::closeEvent(QCloseEvent *e) {
	if (tray && !quitting) {
		hide();
		e->ignore();
		return;
	}
	c->scan_hint(false);
	e->accept();
}

void MainWindow::said(const QString &why) {
	QString text;
	if (why == "not-running") text = "The AirPods service isn't running.";
	else if (why == "not-linked") text = "Not sent: they aren't connected to this computer.";
	else if (why == "bluetooth-off") text = "Bluetooth is off.";
	else if (why == "not-in-bluez") text = "They aren't paired with this computer.";
	else if (why == "unknown-device") text = "The AirPods service doesn't know that pair any more.";
	else if (why == "connected") text = "They can't be forgotten while they are connected.";
	else if (why == "busy") text = "Busy: try again in a moment.";
	else text = "The AirPods service said: " + why;
	statusBar()->showMessage(text, 8000);
}

/* ---- building ---- */

void MainWindow::later() {
	if (pending) return;
	pending = true;
	QMetaObject::invokeMethod(this, &MainWindow::rebuild, Qt::QueuedConnection);
}

void MainWindow::rebuild() {
	pending = false;
	redrawn.restart();
	for (int i = 0; i < TABS; i++) {
		int scroll = pages[i]->verticalScrollBar()->value();
		QWidget *old = pages[i]->takeWidget();
		pages[i]->setWidget(build(i));
		if (old) old->deleteLater();
		pages[i]->verticalScrollBar()->setValue(scroll);
	}
	update_tray();
}

void MainWindow::show_tab(int tab) { tabs->setCurrentIndex(tab); }

QWidget *MainWindow::build(int tab) {
	switch (tab) {
	case TAB_STATUS: return build_status();
	case TAB_LISTEN: return build_listening();
	case TAB_CONTROLS: return build_controls();
	default: return build_about();
	}
}

QLabel *MainWindow::note(const QString &text) {
	QLabel *l = new QLabel(text);
	l->setWordWrap(true);
	l->setForegroundRole(QPalette::PlaceholderText);
	QPalette p = l->palette();	/* in a row that can't be used, as dim as the row */
	p.setColor(QPalette::Disabled, QPalette::PlaceholderText, p.color(QPalette::Disabled, QPalette::WindowText));
	l->setPalette(p);
	return l;
}

static QVBoxLayout *page_layout(QWidget *w) {
	QVBoxLayout *l = new QVBoxLayout(w);
	l->setContentsMargins(18, 14, 18, 14);
	l->setSpacing(10);
	return l;
}

void MainWindow::open_bluetooth() {
	if (QProcess::startDetached("systemsettings", { "kcm_bluetooth" })) return;
	if (QProcess::startDetached("kcmshell6", { "kcm_bluetooth" })) return;
	statusBar()->showMessage("Open your desktop's Bluetooth settings to pair them.", 8000);
}

static struct pods_battery reading(const QJsonObject &b) {
	QJsonValue lv = b.value("level");
	struct pods_battery r = { lv.isDouble(), lv.isDouble() ? qBound(0, lv.toInt(), 100) : 0, b.value("charging").toBool() };
	return r;
}

/* The picture: the buds and the case, and a ring for each battery. Then, for a reading not
 * from a connection, how old it is. */
void MainWindow::batteries(QVBoxLayout *l, bool live) {
	QJsonObject d = c->dev(), bat = jo(d, "battery");
	struct pods_ring rings[3];
	int n;
	if (bat.contains("headset")) {
		rings[0].parts = PODS_PART_HEADSET;
		rings[0].b = reading(jo(bat, "headset"));
		n = 1;
	} else {
		struct pods_battery lb = reading(jo(bat, "left")), rb = reading(jo(bat, "right")), cb = reading(jo(bat, "case"));
		n = pods_rings(&lb, &rb, &cb, rings);
	}
	l->addWidget(new PodsHero(rings, n, js(d, "model_label").contains("Pro"), js(d, "lid") == "open"), 0, Qt::AlignHCenter);
	struct heard { int parts; QString age, from; };
	QList<heard> said;
	for (int i = 0; i < n; i++) {
		int p = rings[i].parts;
		QJsonObject b = jo(bat, p == PODS_PART_HEADSET ? "headset" : p & PODS_PART_LEFT ? "left" : p & PODS_PART_RIGHT ? "right" : "case");
		/* over a link too, the case says nothing when it is away: its reading has an age */
		QString from = js(b, "source");
		bool fresh = p == PODS_PART_CASE ? live && (from == "aap" || from == "case") && c->now() - jl(b, "seen", 0) < 300 : live;
		if (rings[i].b.known && !fresh) said.append({ p, c->age(jl(b, "seen", 0)), from });
	}
	bool same = said.size() > 1;	/* all the same: said once */
	for (const heard &h : said) same &= h.age == said[0].age && h.from == said[0].from;
	for (int i = 0; i < (same ? 1 : int(said.size())); i++) {
		char line[160];
		pods_heard_text(line, sizeof(line), same ? nullptr : pods_ring_name(said[i].parts), said[i].age.toUtf8().constData(),
			said[i].from.toUtf8().constData());
		l->addWidget(note(QString::fromUtf8(line)));
	}
}

/* Connecting them here: BlueZ does it, and only for this click. May take them from
 * another device, as an Apple device tapping "connect" does. */
void MainWindow::connect_row(QVBoxLayout *l) {
	QJsonObject d = c->dev();
	if (!c->has_dev() || d.value("ready").toBool() || js(d, "link") == "connected") return;
	if (!c->root().value("bluetooth").toBool()) {
		l->addWidget(note("Bluetooth is off, so they can't be connected or heard."));
		return;
	}
	QHBoxLayout *h = new QHBoxLayout;
	if (!d.value("in_bluez").toBool()) {
		l->addWidget(note("They aren't paired with this computer. Pair them in the Bluetooth settings."));
		QPushButton *b = new QPushButton(QIcon::fromTheme("preferences-system-bluetooth"), "Open Bluetooth Settings…");
		connect(b, &QPushButton::clicked, this, &MainWindow::open_bluetooth);
		h->addWidget(b);
		h->addStretch();
		l->addLayout(h);
		return;
	}
	QString state = js(d, "connect", "idle");
	if (state == "busy") {
		l->addWidget(note("Connecting…"));
		return;
	}
	QPushButton *b = new QPushButton(QIcon::fromTheme("network-connect"), "Connect Here");
	connect(b, &QPushButton::clicked, this, [this] {
		c->connect_here();
		later();
	});
	h->addWidget(b);
	h->addStretch();
	l->addLayout(h);
	if (state == "failed") {
		l->addWidget(note("That didn't work. They may be in their closed case, out of range, or holding on to another device."));
	}
}

void MainWindow::nearby(QVBoxLayout *l) {
	QJsonArray list = c->root().value("nearby").toArray();
	QGroupBox *box = nullptr;
	QVBoxLayout *in = nullptr;
	for (const QJsonValue &v : list) {
		QJsonObject b = v.toObject();
		if (c->now() - jl(b, "seen", 0) > 30) continue;
		if (!box) {
			box = new QGroupBox("Nearby");
			in = new QVBoxLayout(box);
			in->addWidget(note("Heard over the air. Anyone's AirPods look like this until they have connected "
				"to this computer once, and the levels are in steps of 10%."));
		}
		QJsonObject bat = jo(b, "battery");
		QStringList parts;
		static const char *const P[3][2] = { { "left", "L" }, { "right", "R" }, { "case", "case" } };
		for (auto &p : P) {
			QJsonValue lv = jo(bat, p[0]).value("level");
			if (lv.isDouble()) parts << QString("%1 %2%").arg(p[1]).arg(lv.toInt());
		}
		in->addWidget(new QLabel("<b>" + js(b, "model_label", "AirPods").toHtmlEscaped() + "</b>"));
		in->addWidget(note(parts.isEmpty() ? "No levels" : parts.join(", ")));
	}
	if (box) l->addWidget(box);
}

QWidget *MainWindow::build_status() {
	QWidget *w = new QWidget;
	QVBoxLayout *l = page_layout(w);
	QJsonObject d = c->dev();
	if (!c->running()) {
		l->addWidget(note("The AirPods service isn't running, so nothing is known about them. "
			"Start it with: systemctl --user start ztos-podsd"));
		l->addStretch();
		return w;
	}
	if (!c->root().value("bluetooth").toBool()) l->addWidget(note("Bluetooth is off. These are the last readings."));
	if (!c->has_dev()) {
		l->addWidget(note("No AirPods yet. Pair them in the Bluetooth settings. Once they have connected to this "
			"computer, their batteries show here even when they aren't connected."));
		QHBoxLayout *h = new QHBoxLayout;
		QPushButton *b = new QPushButton(QIcon::fromTheme("preferences-system-bluetooth"), "Open Bluetooth Settings…");
		connect(b, &QPushButton::clicked, this, &MainWindow::open_bluetooth);
		h->addWidget(b);
		h->addStretch();
		l->addLayout(h);
		nearby(l);
		l->addStretch();
		return w;
	}
	QLabel *name = new QLabel(js(d, "name", "AirPods"));
	QFont f = name->font();
	f.setPointSizeF(f.pointSizeF() * 1.7);
	name->setFont(f);
	name->setWordWrap(true);
	l->addWidget(name);
	QString link = js(d, "link", "none");
	bool live = link == "aap";
	QString line;
	if (live) {
		line = "Connected";
	} else if (link == "connected") {
		line = "Connected, reading from the AirPods…";
	} else if (!d.value("has_keys").toBool()) {
		line = "Not connected. Battery can be read from broadcasts after they have connected here once.";
	} else {
		char text[96];
		pods_where_in_use(text, sizeof(text), js(d, "host", "unknown").toUtf8().constData(), jl(d, "connection", -1), HERE);
		line = QString::fromUtf8(text);
	}
	l->addWidget(note(line));
	connect_row(l);

	batteries(l, live);

	QJsonObject ear = jo(d, "ear");
	const char *lp = pods_place_text(js(ear, "left").toUtf8().constData());
	const char *rp = pods_place_text(js(ear, "right").toUtf8().constData());
	QString where;
	if (lp && rp) where = QString("Left %1, right %2").arg(lp, rp);
	else if (lp) where = QString("Left %1").arg(lp);
	else if (rp) where = QString("Right %1").arg(rp);
	if (!where.isEmpty()) {
		if (js(d, "ear_source") != "aap") where += ", " + c->age(jl(d, "ear_seen", 0));
		l->addWidget(note(where));
	}
	QString lid = js(d, "lid", "unknown");
	if (lid != "unknown") l->addWidget(note("Lid " + lid + ", " + c->age(jl(d, "lid_seen", 0))));
	if (!live) l->addWidget(note("Heard from their broadcasts while this window is open. Levels can lag events by a little."));

	/* other pairs, if there are any */
	QJsonArray list = c->devices();
	if (list.size() > 1) {
		QGroupBox *box = new QGroupBox("Other AirPods");
		QVBoxLayout *in = new QVBoxLayout(box);
		for (int i = 0; i < list.size(); i++) {
			if (i == c->dev_index_of()) continue;
			QJsonObject o = list[i].toObject();
			QHBoxLayout *h = new QHBoxLayout;
			QLabel *t = new QLabel("<b>" + js(o, "name", "AirPods").toHtmlEscaped() + "</b><br>" + js(o, "model_label").toHtmlEscaped());
			h->addWidget(t, 1);
			QPushButton *b = new QPushButton("Show");
			QString mac = js(o, "mac");
			connect(b, &QPushButton::clicked, this, [this, mac] {
				c->pick(mac);
				name_editing = false;
				later();
			});
			h->addWidget(b);
			in->addLayout(h);
		}
		l->addWidget(box);
	}
	nearby(l);
	l->addStretch();
	return w;
}

/* The top of a tab of settings: why they can't be changed, if they can't, and how old they are.
 * Settings go over AAP only: no broadcast carries them, and nothing can set one without a link. */
void MainWindow::settings_banner(QVBoxLayout *l) {
	if (c->settings_open()) return;
	QJsonObject d = c->dev();
	if (c->ready()) {
		l->addWidget(note("Reading settings from the AirPods…"));
		return;
	}
	if (js(d, "link") == "connected") {
		l->addWidget(note("Connected, reading from the AirPods…"));
		return;
	}
	long seen = jl(d, "link_seen", 0);
	if (seen) {
		l->addWidget(note("Not connected to this computer. As they were when last connected, " + c->age(seen) +
			". They can only be changed with a connection."));
	} else {
		l->addWidget(note("Not connected to this computer. They can only be read and changed with a connection."));
	}
	connect_row(l);
}

void MainWindow::rows(QFormLayout *f, int page) {
	bool open = c->settings_open();
	for (int i = 0; i < PODS_N_ROWS; i++) {
		const struct pods_row *r = &PODS_ROWS[i];
		if (r->page != page || !c->in_list("visible", r->key)) continue;
		int value = c->setting(r->key);
		/* a value the pods never reported is not shown as if they had */
		QString caption = QString(r->title) + (value < 0 ? " (not reported)" : "");
		const char *key = r->key;
		switch (r->kind) {
		case PODS_R_SWITCH: {
			QCheckBox *b = new QCheckBox(r->title);
			b->setChecked(value > 0);
			connect(b, &QCheckBox::clicked, this, [this, key] {
				c->set_setting(key, c->setting(key) == 1 ? 0 : 1);
				later();
			});
			if (value >= 0) {
				f->addRow(b);
			} else {
				QWidget *two = new QWidget;
				QVBoxLayout *v = new QVBoxLayout(two);
				v->setContentsMargins(0, 0, 0, 0);
				v->setSpacing(0);
				v->addWidget(b);
				QLabel *n = note(open ? "Not reported by the AirPods. Click to turn on." : "Not known");
				n->setIndent(b->style()->pixelMetric(QStyle::PM_IndicatorWidth) + 6);
				v->addWidget(n);
				f->addRow(two);
			}
			break;
		}
		case PODS_R_SLIDER: {
			QWidget *two = new QWidget;
			QHBoxLayout *h = new QHBoxLayout(two);
			h->setContentsMargins(0, 0, 0, 0);
			QSlider *s = new QSlider(Qt::Horizontal);
			s->setRange(0, 100);
			s->setValue(value < 0 ? 0 : value);
			s->setPageStep(10);
			QLabel *shown = new QLabel(QString::number(s->value()));
			shown->setMinimumWidth(shown->fontMetrics().horizontalAdvance("100") + 4);
			shown->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			h->addWidget(s, 1);
			h->addWidget(shown);
			connect(s, &QSlider::sliderPressed, this, [this] { slider_held = true; slid.invalidate(); });
			connect(s, &QSlider::sliderMoved, this, [this, key](int v) {
				c->set_local(key, v);
				if (!slid.isValid() || slid.elapsed() > 120) {
					slid.start();
					c->set_setting(key, v);
				}
			});
			connect(s, &QSlider::sliderReleased, this, [this, key, s] {
				slider_held = false;
				c->set_setting(key, s->value());
			});
			connect(s, &QSlider::valueChanged, this, [this, key, s, shown](int v) {
				shown->setText(QString::number(v));
				if (!s->isSliderDown()) c->set_setting(key, v);	/* keys, wheel, a click on the groove */
			});
			f->addRow(caption, two);
			break;
		}
		case PODS_R_PICKER: {
			QComboBox *b = new QComboBox;
			int sel = 0;
			for (int k = 0; k < r->count; k++) {
				b->addItem(r->options[k]);
				if (r->values[k] == value) sel = k;
			}
			b->setCurrentIndex(sel);
			const int *values = r->values;
			int count = r->count;
			connect(b, &QComboBox::activated, this, [this, key, values, count](int k) {
				if (k >= 0 && k < count) c->set_setting(key, values[k]);
				later();
			});
			f->addRow(caption, b);
			break;
		}
		}
	}
}

bool MainWindow::mode_available(int i) const {
	if (!strcmp(PODS_MODES[i].name, "adaptive") && !c->in_list("caps", "adaptive")) {
		return js(c->dev(), "mode") == "adaptive";
	}
	return true;
}

static QFormLayout *form() {
	QFormLayout *f = new QFormLayout;
	f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	return f;
}

QWidget *MainWindow::build_listening() {
	QWidget *w = new QWidget;
	QVBoxLayout *l = page_layout(w);
	if (!c->has_dev()) {
		l->addWidget(note("No AirPods yet."));
		l->addStretch();
		return w;
	}
	settings_banner(l);
	QWidget *body = new QWidget;	/* while they can't be changed: shown, not usable */
	body->setEnabled(c->settings_open());
	QVBoxLayout *bl = new QVBoxLayout(body);
	bl->setContentsMargins(0, 0, 0, 0);
	QJsonObject d = c->dev();
	if (c->in_list("caps", "modes")) {
		QGroupBox *box = new QGroupBox("Noise control");
		QVBoxLayout *in = new QVBoxLayout(box);
		QString mode = js(d, "mode");
		if (mode.isEmpty()) mode = js(d, "last_mode");
		for (int i = 0; i < PODS_N_MODES; i++) {
			if (!mode_available(i)) continue;
			QRadioButton *b = new QRadioButton(PODS_MODES[i].label);
			b->setChecked(mode == PODS_MODES[i].name);
			connect(b, &QRadioButton::clicked, this, [this, i] {
				c->set_mode(PODS_MODES[i].name);
				later();
			});
			in->addWidget(b);
		}
		bl->addWidget(box);
	} else {
		bl->addWidget(note("These AirPods have no listening modes."));
	}
	QFormLayout *f = form();
	rows(f, PODS_UI_LISTEN);
	if (f->rowCount()) {
		QGroupBox *box = new QGroupBox("Listening");
		box->setLayout(f);
		bl->addWidget(box);
	} else {
		delete f;
	}
	int mask = c->setting("mode_cycle");
	if (c->in_list("visible", "mode_cycle") && (c->settings_open() || mask >= 0)) {
		QGroupBox *box = new QGroupBox("Press and hold switches between");
		QVBoxLayout *in = new QVBoxLayout(box);
		if (mask < 0) in->addWidget(note("The AirPods don't report this. Choosing here replaces their list. At least two."));
		else if (!(mask & (mask - 1))) in->addWidget(note("Choose at least two: they are sent when there are two."));
		for (int i = 0; i < PODS_N_MODES; i++) {
			if (!mode_available(i)) continue;
			QCheckBox *b = new QCheckBox(PODS_MODES[i].label);
			b->setChecked(mask > 0 && (mask & PODS_MODES[i].bit));
			connect(b, &QCheckBox::clicked, this, [this, i] {
				int m = c->setting("mode_cycle");
				if (m < 0) m = 0;
				m ^= PODS_MODES[i].bit;
				/* with fewer than two the daemon would refuse it: shown, sent when there are two */
				c->set_local("mode_cycle", m);
				if (m & (m - 1)) c->set_setting("mode_cycle", m);
				later();
			});
			in->addWidget(b);
		}
		bl->addWidget(box);
	}
	l->addWidget(body);
	l->addStretch();
	return w;
}

QWidget *MainWindow::build_controls() {
	QWidget *w = new QWidget;
	QVBoxLayout *l = page_layout(w);
	if (!c->has_dev()) {
		l->addWidget(note("No AirPods yet."));
		l->addStretch();
		return w;
	}
	settings_banner(l);
	QGroupBox *box = new QGroupBox("Controls");
	box->setEnabled(c->settings_open());
	QFormLayout *f = form();
	box->setLayout(f);
	rows(f, PODS_UI_CONTROLS);
	if (f->rowCount()) l->addWidget(box);
	else delete box;
	l->addStretch();
	return w;
}

QWidget *MainWindow::build_about() {
	QWidget *w = new QWidget;
	QVBoxLayout *l = page_layout(w);
	if (!c->has_dev()) {
		l->addWidget(note("No AirPods yet."));
		l->addStretch();
		return w;
	}
	QJsonObject d = c->dev();
	QFormLayout *f = form();
	if (c->ready()) {
		QWidget *row = new QWidget;
		QHBoxLayout *h = new QHBoxLayout(row);
		h->setContentsMargins(0, 0, 0, 0);
		QLineEdit *e = new QLineEdit(name_editing ? name_edit : js(d, "name"));
		QPushButton *save = new QPushButton("Save");
		QPushButton *revert = new QPushButton("Revert");
		save->setEnabled(name_editing && !name_edit.isEmpty() && name_edit != js(d, "name"));
		revert->setEnabled(name_editing);
		h->addWidget(e, 1);
		h->addWidget(save);
		h->addWidget(revert);
		connect(e, &QLineEdit::textEdited, this, [this, e, save, revert](const QString &t) {
			/* whole characters only, and no more than the pods keep */
			QString cut = t;
			while (cut.toUtf8().size() > MAX_NAME) cut.chop(1);
			if (cut.size() && cut.at(cut.size() - 1).isHighSurrogate()) cut.chop(1);
			if (cut != t) e->setText(cut);
			name_editing = true;
			name_edit = cut;
			save->setEnabled(!cut.isEmpty() && cut != js(c->dev(), "name"));
			revert->setEnabled(true);
		});
		auto do_save = [this] {
			if (name_editing && !name_edit.isEmpty() && name_edit != js(c->dev(), "name")) c->rename(name_edit);
			name_editing = false;
			later();
		};
		connect(e, &QLineEdit::returnPressed, this, do_save);
		connect(save, &QPushButton::clicked, this, do_save);
		connect(revert, &QPushButton::clicked, this, [this] {
			name_editing = false;
			later();
		});
		f->addRow("Name:", row);
		QLabel *n = note("Saved on the AirPods. This computer's Bluetooth list changes at their next connection, "
			"and Apple devices keep their own name for them.");
		f->addRow(QString(), n);
	} else {
		QLabel *name = new QLabel(js(d, "name", "AirPods"));
		name->setTextInteractionFlags(Qt::TextSelectableByMouse);
		f->addRow("Name:", name);
		f->addRow(QString(), note("The name is kept on the AirPods: connect them to change it."));
	}
	auto field = [f](const char *label, const QString &value) {
		QLabel *v = new QLabel(value);
		v->setWordWrap(true);
		v->setTextInteractionFlags(Qt::TextSelectableByMouse);
		f->addRow(label, v);
	};
	field("Model:", js(d, "model_label", "AirPods"));
	if (!js(d, "model_number").isEmpty()) field("Model number:", js(d, "model_number"));
	if (!js(d, "firmware").isEmpty()) field("Version:", js(d, "firmware"));
	long colour = jl(d, "color", -1);
	if (colour >= 0 && colour < PODS_N_COLOURS) field("Colour:", PODS_COLOURS[colour]);
	field("Address:", js(d, "mac"));
	field("Broadcasts:", d.value("has_keys").toBool() ? "Can be read: this computer has their keys"
		: "Can't be read until they have connected here once");
	if (jl(d, "link_seen", 0) && !c->ready()) field("Last connected here:", c->age(jl(d, "link_seen", 0)));
	l->addLayout(f);
	if (!c->ready()) connect_row(l);
	if (!d.value("tested").toBool()) {
		l->addWidget(note("ztOS has been written against AirPods Pro 2. On this model some settings may not work: "
			"what they do comes from other projects' notes."));
	}
	if (!c->ready() && d.value("has_keys").toBool()) {
		QGroupBox *box = new QGroupBox("Forget");
		QVBoxLayout *in = new QVBoxLayout(box);
		in->addWidget(note("Removes what this computer keeps to read their broadcasts, and their last readings. "
			"It doesn't unpair them; that's in the Bluetooth settings."));
		QHBoxLayout *h = new QHBoxLayout;
		QPushButton *b = new QPushButton(QIcon::fromTheme("edit-delete"), "Forget…");
		connect(b, &QPushButton::clicked, this, [this] {
			QString name = js(c->dev(), "name", "AirPods");
			if (QMessageBox::question(this, "Forget AirPods", "Forget “" + name + "”?\n\nTheir keys and last readings "
					"go. They stay paired, and come back once they connect here again.",
					QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
				return;
			}
			c->forget();
			later();
		});
		h->addWidget(b);
		h->addStretch();
		in->addLayout(h);
		l->addWidget(box);
	}
	l->addStretch();
	return w;
}

void MainWindow::update_tray() {
	QJsonObject d = c->dev();
	QString tip = "AirPods";
	if (c->has_dev()) {
		QJsonObject bat = jo(d, "battery");
		QStringList parts;
		static const char *const P[4][2] = { { "headset", "Headphones" }, { "left", "Left" }, { "right", "Right" }, { "case", "Case" } };
		for (auto &p : P) {
			QJsonValue lv = jo(bat, p[0]).value("level");
			if (lv.isDouble()) parts << QString("%1 %2%%3").arg(p[1]).arg(lv.toInt()).arg(jo(bat, p[0]).value("charging").toBool() ? " (charging)" : "");
		}
		tip = js(d, "name", "AirPods") + "\n" + (parts.isEmpty() ? QString("No battery readings") : parts.join("\n"));
	} else if (!c->running()) {
		tip = "AirPods: the service isn't running";
	}
	if (tray) tray->setToolTip(tip);
}

/* ---- a picture, for looking at without a display ---- */

bool MainWindow::render(int tab, const QString &png) {
	show_tab(tab);
	resize(600, 400);
	show();
	QApplication::processEvents();
	QWidget *content = pages[tab]->widget();
	int vw = pages[tab]->viewport()->width();
	int need = content->hasHeightForWidth() ? content->heightForWidth(vw) : content->sizeHint().height();
	need = qMax(need, content->minimumSizeHint().height());
	resize(600, height() - pages[tab]->viewport()->height() + need + 2);
	statusBar()->clearMessage();	/* (no daemon to hear the scan hint) */
	QApplication::processEvents();
	return grab().save(png);
}
