// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The window: four tabs, as ztMETRO's pages (pods-pages.c, the spec): status, listening,
 * controls, about. A tab is built again from the state whenever it changes; not while a
 * slider is held or the name is being edited, nor for 1.5 s after a change made here.
 */
#pragma once
#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

class PodsClient;
class QScrollArea;
class QTabWidget;
class QVBoxLayout;
class QFormLayout;
class QSystemTrayIcon;
class QJsonObject;
class QLabel;

enum { TAB_STATUS, TAB_LISTEN, TAB_CONTROLS, TAB_ABOUT, TABS };

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	MainWindow(PodsClient *client, bool tray);
	void rebuild();
	void show_tab(int tab);
	bool render(int tab, const QString &png);	/* the tab, all of it, to a file */

protected:
	void closeEvent(QCloseEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
	void changeEvent(QEvent *e) override;

private:
	QWidget *build(int tab);
	QWidget *build_status();
	QWidget *build_listening();
	QWidget *build_controls();
	QWidget *build_about();
	void battery_row(QVBoxLayout *l, const QString &label, const QJsonObject &b, bool live);
	void connect_row(QVBoxLayout *l);
	void settings_banner(QVBoxLayout *l);
	void rows(QFormLayout *f, int page);
	void nearby(QVBoxLayout *l);
	bool mode_available(int i) const;
	QLabel *note(const QString &text);
	void open_bluetooth();
	void later();			/* rebuilt once the signal now running is over */
	void tick();
	void hint();
	bool looking() const;
	void said(const QString &why);
	void update_tray();

	PodsClient *c;
	QTabWidget *tabs;
	QScrollArea *pages[TABS];
	QTimer poll, hinter;
	QElapsedTimer redrawn, slid;
	bool pending = false, slider_held = false, name_editing = false, quitting = false;
	QString name_edit;
	QSystemTrayIcon *tray = nullptr;
};
