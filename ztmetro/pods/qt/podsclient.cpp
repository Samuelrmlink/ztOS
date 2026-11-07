// SPDX-License-Identifier: GPL-3.0-or-later
#include "podsclient.h"

#include <QFile>
#include <QJsonDocument>
#include <QSocketNotifier>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "pods-ui.h"

PodsClient::PodsClient(QObject *parent) : QObject(parent) {
	/* bound (to an abstract name of the kernel's choosing), so the daemon answers */
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un a = {};
	a.sun_family = AF_UNIX;
	if (fd >= 0 && bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(sa_family_t)) == 0) {
		notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
		connect(notifier, &QSocketNotifier::activated, this, &PodsClient::read_reply);
	}
}

PodsClient::~PodsClient() {
	if (fd >= 0) close(fd);
}

QString PodsClient::runtime_path(const char *name) const {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	return QString::fromLocal8Bit(rt ? rt : "/tmp") + "/" + name;
}

bool PodsClient::load(bool force) {
	QByteArray path = runtime_path("ztos-pods-state.json").toLocal8Bit();
	struct stat st;
	if (stat(path.constData(), &st) != 0) {
		if (!have_root && !force) return false;
		root_ = QJsonObject();
		have_root = false;
		dev_index = -1;
		mtime_ns = -1;
		return true;
	}
	qint64 m = qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
	if (!force && m == mtime_ns) return false;
	mtime_ns = m;
	QFile f(QString::fromLocal8Bit(path));
	if (!f.open(QIODevice::ReadOnly)) return false;
	QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isObject()) return false;
	root_ = doc.object();
	have_root = true;
	pick(wanted);
	return true;
}

void PodsClient::pick(const QString &mac) {
	wanted = mac;
	dev_index = -1;
	QJsonArray list = devices();
	for (int i = 0; i < list.size(); i++) {
		if (dev_index < 0) dev_index = i;
		if (!wanted.isEmpty() && list[i].toObject().value("mac").toString().compare(wanted, Qt::CaseInsensitive) == 0) dev_index = i;
	}
	if (dev_index >= 0 && wanted.isEmpty()) wanted = list[dev_index].toObject().value("mac").toString();
}

QJsonObject PodsClient::dev() const {
	return dev_index >= 0 ? devices().at(dev_index).toObject() : QJsonObject();
}

void PodsClient::put_dev(const QJsonObject &d) {
	QJsonArray list = devices();
	if (dev_index < 0 || dev_index >= list.size()) return;
	list[dev_index] = d;
	root_["devices"] = list;
}

long PodsClient::now() const { return fixture_now ? fixture_now : long(time(nullptr)); }

bool PodsClient::changed_here_lately() const { return local.isValid() && local.elapsed() < 1500; }

int PodsClient::setting(const char *key) const {
	QJsonValue v = dev().value("settings").toObject().value(key);
	return v.isDouble() ? v.toInt() : -1;
}

bool PodsClient::in_list(const char *list, const char *want) const {
	for (const QJsonValue &v : dev().value(list).toArray()) {
		if (v.toString() == QLatin1String(want)) return true;
	}
	return false;
}

QString PodsClient::age(long seen) const {
	char text[32];
	pods_age_text(text, sizeof(text), now() - seen);
	return QString::fromUtf8(text);
}

void PodsClient::send(const QString &line) {
	QByteArray b = line.toUtf8();
	QByteArray path = runtime_path("ztos-pods.sock").toLocal8Bit();
	struct sockaddr_un a = {};
	a.sun_family = AF_UNIX;
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", path.constData());
	if (fd < 0) return;
	if (sendto(fd, b.constData(), size_t(b.size()), MSG_DONTWAIT, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) < 0) {
		if (errno != ENOENT && errno != ECONNREFUSED) perror("ztpods-qt: send");
		else emit replied("not-running");
	}
}

void PodsClient::read_reply() {
	char buf[128];
	ssize_t n;
	while ((n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT)) > 0) {
		buf[n] = 0;
		if (!strncmp(buf, "err ", 4)) emit replied(QString::fromUtf8(buf + 4).trimmed());
	}
}

void PodsClient::set_local(const char *key, int value) {
	QJsonObject d = dev();
	QJsonObject s = d.value("settings").toObject();
	s[key] = value;
	d["settings"] = s;
	put_dev(d);
	local.start();
}

void PodsClient::set_setting(const char *key, int value) {
	send(QString("set %1 %2 %3").arg(mac(), key).arg(value));
	set_local(key, value);
}

void PodsClient::set_mode(const char *name) {
	send(QString("set %1 listening_mode %2").arg(mac(), name));
	QJsonObject d = dev();
	d["mode"] = name;
	put_dev(d);
	local.start();
}

void PodsClient::connect_here() {
	send("connect " + mac());
	QJsonObject d = dev();
	d["connect"] = "busy";
	put_dev(d);
	local.start();
}

void PodsClient::rename(const QString &name) {
	QString one = name;
	one.replace('\n', ' ');
	send("rename " + mac() + " " + one);
	QJsonObject d = dev();
	d["name"] = one;
	put_dev(d);
	local.start();
}

void PodsClient::forget() {
	send("forget " + mac());
	wanted.clear();
	local.invalidate();
}

void PodsClient::scan_hint(bool on) { send(on ? "scan-hint on" : "scan-hint off"); }
