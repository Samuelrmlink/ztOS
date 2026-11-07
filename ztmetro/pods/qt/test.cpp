// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ztpods-qt's controls against a pretend ztos-podsd: fixture state files in a private
 * XDG_RUNTIME_DIR, and a datagram socket where the daemon's would be that logs each line
 * it gets and answers "ok". Every control is used, and the line it sends is checked.
 * Offscreen; no daemon, no Bluetooth.   ctest --test-dir build   (README.md)
 */
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "mainwindow.h"
#include "podsclient.h"

static int failures;
static int fake = -1;
static QString run_dir;
static FILE *log_file;
static const char MAC[] = "40:DA:5C:00:00:01";

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void settle() {
	for (int i = 0; i < 5; i++) {
		QCoreApplication::processEvents();
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}
}

/* what the pretend daemon got since last asked, scan hints apart */
static QStringList heard(bool with_hints = false) {
	QStringList lines;
	char buf[512];
	struct sockaddr_un from;
	socklen_t len = sizeof(from);
	ssize_t n;
	usleep(20000);
	while ((n = recvfrom(fake, buf, sizeof(buf) - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &len)) > 0) {
		buf[n] = 0;
		fprintf(log_file, "%s\n", buf);
		if (len > sizeof(sa_family_t)) sendto(fake, "ok", 2, MSG_DONTWAIT, (struct sockaddr *)&from, len);
		if (with_hints || strncmp(buf, "scan-hint", 9)) lines << QString::fromUtf8(buf);
		len = sizeof(from);
	}
	settle();
	return lines;
}

static void expect(const QStringList &want) {
	QStringList got = heard();
	if (got != want) {
		fprintf(stderr, "FAIL: wanted [%s], got [%s]\n", qPrintable(want.join(" | ")), qPrintable(got.join(" | ")));
		failures++;
	} else {
		for (const QString &l : got) printf("  sent: %s\n", qPrintable(l));
	}
}

static void fixture(const char *path) {
	QFile::remove(run_dir + "/ztos-pods-state.json");
	CHECK(QFile::copy(path, run_dir + "/ztos-pods-state.json"));
}

template <class T> static T *find(QWidget *w, const QString &text) {
	for (T *c : w->findChildren<T *>()) {
		if (c->text() == text && c->isVisibleTo(w)) return c;
	}
	fprintf(stderr, "FAIL: no \"%s\"\n", qPrintable(text));
	failures++;
	return nullptr;
}

/* the control in a form row whose label is this */
template <class T> static T *row(QWidget *w, const QString &label) {
	for (QLabel *l : w->findChildren<QLabel *>()) {
		if (l->text() == label && l->buddy()) {
			if (T *t = qobject_cast<T *>(l->buddy())) return t;
			if (T *t = l->buddy()->findChild<T *>()) return t;
		}
	}
	fprintf(stderr, "FAIL: no row \"%s\"\n", qPrintable(label));
	failures++;
	return nullptr;
}

static bool says(QWidget *w, const QString &part) {
	for (QLabel *l : w->findChildren<QLabel *>()) {
		if (l->text().contains(part) && l->isVisibleTo(w)) return true;
	}
	return false;
}

static void connected(const char *fix) {
	printf("connected:\n");
	fixture(fix);
	PodsClient c;
	c.load(true);
	MainWindow w(&c, false);
	w.show();
	settle();
	CHECK(heard(true).contains("scan-hint on"));
	CHECK(says(&w, "Samuel’s AirPods Pro"));
	for (QPushButton *b : w.findChildren<QPushButton *>()) CHECK(b->text() != "Connect Here");	/* linked already */

	/* listening */
	w.show_tab(TAB_LISTEN);
	settle();
	QRadioButton *anc = find<QRadioButton>(&w, "Noise Cancellation");
	CHECK(anc && anc->isChecked() && anc->isEnabled());
	if (QRadioButton *b = find<QRadioButton>(&w, "Transparency")) b->click();
	expect({ QString("set %1 listening_mode transparency").arg(MAC) });
	CHECK(find<QRadioButton>(&w, "Transparency")->isChecked());
	if (QCheckBox *b = find<QCheckBox>(&w, "Conversation Awareness")) {
		CHECK(b->isChecked());
		b->click();
	}
	expect({ QString("set %1 conversation 0").arg(MAC) });
	if (QSlider *s = row<QSlider>(&w, "Adaptive strength")) {
		CHECK(s->value() == 100);
		s->triggerAction(QAbstractSlider::SliderPageStepSub);
	}
	expect({ QString("set %1 adaptive_strength 90").arg(MAC) });
	/* the cycle: 10 is anc + adaptive. One alone is shown, not sent. */
	auto cycle = [&](const char *label) {
		QCheckBox *hit = nullptr;
		for (QCheckBox *b : w.findChildren<QCheckBox *>()) {
			if (b->text() == label) hit = b;
		}
		CHECK(hit);
		if (hit) hit->click();
	};
	cycle("Off");
	expect({ QString("set %1 mode_cycle 11").arg(MAC) });
	cycle("Off");
	expect({ QString("set %1 mode_cycle 10").arg(MAC) });
	cycle("Adaptive");
	expect({});
	CHECK(says(&w, "Choose at least two"));
	cycle("Transparency");
	expect({ QString("set %1 mode_cycle 6").arg(MAC) });

	/* controls */
	w.show_tab(TAB_CONTROLS);
	settle();
	if (QComboBox *b = row<QComboBox>(&w, "Press and hold, left")) {
		CHECK(b->currentText() == "Noise control");
		emit b->activated(1);
	}
	expect({ QString("set %1 hold_left 5").arg(MAC) });
	if (QComboBox *b = row<QComboBox>(&w, "Press speed (not reported)")) emit b->activated(2);
	expect({ QString("set %1 press_speed 2").arg(MAC) });
	CHECK(row<QComboBox>(&w, "Press speed")->currentText() == "Slowest");
	if (QComboBox *b = row<QComboBox>(&w, "Microphone")) emit b->activated(1);
	expect({ QString("set %1 mic 1").arg(MAC) });
	if (QCheckBox *b = find<QCheckBox>(&w, "Case sounds")) b->click();
	expect({ QString("set %1 case_tone 1").arg(MAC) });
	if (QCheckBox *b = find<QCheckBox>(&w, "Automatic ear detection")) b->click();
	expect({ QString("set %1 ear_detection 1").arg(MAC) });
	if (QSlider *s = row<QSlider>(&w, "Chime volume")) {
		s->setSliderDown(true);
		s->setSliderPosition(40);	/* a drag: sent as it goes, at most every 120 ms */
		s->setSliderDown(false);
	}
	{
		QStringList got = heard();
		CHECK(!got.isEmpty() && got.last() == QString("set %1 chime_volume 40").arg(MAC));
		for (const QString &l : got) printf("  sent: %s\n", qPrintable(l));
	}
	for (const char *k : { "Press and hold duration", "Swipe length", "Press and hold, right" }) CHECK(row<QComboBox>(&w, k));
	for (const char *k : { "Swipe for volume", "Pause when you fall asleep" }) CHECK(find<QCheckBox>(&w, k));
	CHECK(row<QSlider>(&w, "Case sounds volume"));

	/* about: the name */
	w.show_tab(TAB_ABOUT);
	settle();
	if (QLineEdit *e = w.findChild<QLineEdit *>()) {
		e->selectAll();
		e->del();
		const QString name = "Sam’s Pods";
		for (QChar ch : name) {
			e->insert(ch);
			emit e->textEdited(e->text());
		}
		settle();
		emit e->returnPressed();
	}
	expect({ QString("rename %1 Sam’s Pods").arg(MAC) });
	CHECK(!says(&w, "Forget"));
	w.hide();
	settle();
	CHECK(heard(true).contains("scan-hint off"));
}

static void offline(const char *fix) {
	printf("offline:\n");
	fixture(fix);
	PodsClient c;
	c.load(true);
	MainWindow w(&c, false);
	w.show();
	settle();
	heard();
	CHECK(says(&w, "Not connected to anything"));
	CHECK(says(&w, "from the case"));
	if (QPushButton *b = find<QPushButton>(&w, "Connect Here")) b->click();
	expect({ QString("connect %1").arg(MAC) });
	CHECK(says(&w, "Connecting…"));
	w.show_tab(TAB_LISTEN);
	settle();
	CHECK(says(&w, "As they were when last connected"));
	QRadioButton *r = find<QRadioButton>(&w, "Adaptive");
	CHECK(r && r->isChecked() && !r->isEnabled());	/* the last mode, read-only */
	w.show_tab(TAB_ABOUT);
	settle();
	CHECK(!w.findChild<QLineEdit *>());
	/* forget: asked first. No, then yes. */
	auto answer = [](QMessageBox::StandardButton which) {
		QTimer::singleShot(50, [which] {
			if (auto *m = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) m->button(which)->click();
		});
	};
	answer(QMessageBox::Cancel);
	if (QPushButton *b = find<QPushButton>(&w, "Forget…")) b->click();
	expect({});
	answer(QMessageBox::Yes);
	if (QPushButton *b = find<QPushButton>(&w, "Forget…")) b->click();
	expect({ QString("forget %1").arg(MAC) });
}

static void mixed(const char *fix) {
	printf("mixed, opened on the second pair:\n");
	fixture(fix);
	PodsClient c;
	c.set_render_now(1794093380);	/* the nearby pair was heard 10 s before */
	c.pick("11:22:33:44:55:66");
	c.load(true);
	MainWindow w(&c, false);
	w.show();
	settle();
	heard();
	CHECK(says(&w, "Old AirPods"));
	CHECK(says(&w, "They aren't paired with this computer"));
	CHECK(says(&w, "L 60%, R 70%"));
	if (QPushButton *b = find<QPushButton>(&w, "Show")) b->click();
	settle();
	CHECK(says(&w, "Samuel’s AirPods Pro"));
	CHECK(says(&w, "In use by another device, playing music"));
	CHECK(says(&w, "That didn't work"));
	w.show_tab(TAB_CONTROLS);
	settle();
	CHECK(says(&w, "Not known"));	/* case_tone, not reported, no link */
	expect({});
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: ztpods-qt-test FIXTURES_DIR\n");
		return 2;
	}
	QTemporaryDir tmp;
	run_dir = tmp.path();
	qputenv("XDG_RUNTIME_DIR", run_dir.toLocal8Bit());
	qputenv("ZTPODS_TEST", "1");
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);

	fake = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s/ztos-pods.sock", qPrintable(run_dir));
	if (fake < 0 || bind(fake, (struct sockaddr *)&a, sizeof(a)) < 0) return 1;
	QString log = qEnvironmentVariable("ZTPODS_TEST_LOG", run_dir + "/heard.log");
	log_file = fopen(qPrintable(log), "w");
	if (!log_file) return 1;

	QString dir = QString::fromLocal8Bit(argv[1]);
	connected(qPrintable(dir + "/connected.json"));
	offline(qPrintable(dir + "/offline.json"));
	mixed(qPrintable(dir + "/mixed.json"));
	fclose(log_file);
	printf("%s (every line the pretend daemon heard: %s)\n", failures ? "FAILED" : "ztpods-qt-test: ok", qPrintable(log));
	return failures ? 1 : 0;
}
