// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * What ztos-podsd says, and what is said to it: $XDG_RUNTIME_DIR/ztos-pods-state.json,
 * read when its mtime moves (it is replaced whole, so a file watcher loses it), and
 * ztos-pods.sock, a line a datagram. No Bluetooth in here.
 */
#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QSocketNotifier;

class PodsClient : public QObject {
	Q_OBJECT
public:
	explicit PodsClient(QObject *parent = nullptr);
	~PodsClient() override;

	bool load(bool force);			/* true if anything changed */
	bool running() const { return have_root; }
	const QJsonObject &root() const { return root_; }
	QJsonObject dev() const;		/* the pair on show; empty: none */
	bool has_dev() const { return dev_index >= 0; }
	int dev_index_of() const { return dev_index; }
	QJsonArray devices() const { return root_.value("devices").toArray(); }
	void pick(const QString &mac);		/* empty: the first */
	QString mac() const { return dev().value("mac").toString(); }

	long now() const;			/* the file's own time when rendering for a picture */
	void set_render_now(long t) { fixture_now = t; }
	bool changed_here_lately() const;	/* a change made here: the file is not read over it */

	/* the state's words */
	bool ready() const { return dev().value("ready").toBool(); }
	bool settings_open() const { return ready() && dev().value("settings_known").toBool(); }
	int setting(const char *key) const;	/* -1: not reported */
	bool in_list(const char *list, const char *want) const;
	QString age(long seen) const;

	/* the commands; each shown at once, the daemon's file catches up */
	void send(const QString &line);
	void set_setting(const char *key, int value);
	void set_local(const char *key, int value);
	void set_mode(const char *name);
	void connect_here();
	void rename(const QString &name);
	void forget();
	void scan_hint(bool on);

signals:
	void replied(const QString &why);	/* the daemon's "err WHY" */

private:
	void put_dev(const QJsonObject &d);
	void read_reply();
	QString runtime_path(const char *name) const;

	QJsonObject root_;
	bool have_root = false;
	int dev_index = -1;
	QString wanted;
	qint64 mtime_ns = -1;
	long fixture_now = 0;
	int fd = -1;
	QSocketNotifier *notifier = nullptr;
	QElapsedTimer local;
};
