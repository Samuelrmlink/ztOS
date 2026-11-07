// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * ztpods-qt: AirPods' batteries, listening modes and settings, on a Qt desktop. The same
 * ztos-podsd and the same pods-ui.h as ztMETRO's ztpods; only the front end differs.
 *
 *   ztpods-qt [--tray] [MAC]          MAC: open on that pair. --tray: an icon in the
 *                                     tray, battery in its tooltip; closing hides
 *   ztpods-qt --render TAB OUT.png [MAC]
 *                                     TAB (status, listening, controls, about) to a
 *                                     picture, no display needed. ZTPODS_RENDER_NOW=SECONDS
 *                                     draws the ages as of then, for stable pictures.
 */
#include <QApplication>
#include <QIcon>
#include <QStyle>
#include <QSystemTrayIcon>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mainwindow.h"
#include "podsclient.h"

static const char *const TAB_NAMES[TABS] = { "status", "listening", "controls", "about" };

static int usage() {
	fprintf(stderr, "usage: ztpods-qt [--tray] [MAC]\n       ztpods-qt --render status|listening|controls|about OUT.png [MAC]\n");
	return 2;
}

int main(int argc, char **argv) {
	bool rendering = argc >= 2 && !strcmp(argv[1], "--render");
	if (rendering && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	app.setApplicationName("ztpods-qt");
	app.setApplicationDisplayName("AirPods");
	app.setDesktopFileName("ztpods-qt");
	app.setWindowIcon(QIcon::fromTheme("ztpods-qt", QIcon(":/ztpods-qt.svg")));
	/* offscreen has no desktop to choose a style: Breeze, as on KDE, when it is there */
	if (rendering && qEnvironmentVariableIsEmpty("QT_STYLE_OVERRIDE")) QApplication::setStyle("Breeze");

	QStringList args = app.arguments();
	args.removeFirst();
	bool tray = false;
	int tab = -1;
	QString png, mac;
	if (rendering) {
		if (args.size() < 3 || args.size() > 4) return usage();
		for (int i = 0; i < TABS; i++) {
			if (args[1] == TAB_NAMES[i]) tab = i;
		}
		if (tab < 0) return usage();
		png = args[2];
		if (args.size() == 4) mac = args[3];
	} else {
		for (const QString &a : args) {
			if (a == "--tray") tray = true;
			else if (a.startsWith('-')) return usage();
			else mac = a;
		}
	}

	PodsClient client;
	if (rendering) client.set_render_now(qEnvironmentVariableIntValue("ZTPODS_RENDER_NOW"));
	client.pick(mac);
	client.load(true);
	MainWindow w(&client, tray);
	if (rendering) return w.render(tab, png) ? 0 : 1;
	if (!tray || !QSystemTrayIcon::isSystemTrayAvailable()) w.show();
	return app.exec();
}
