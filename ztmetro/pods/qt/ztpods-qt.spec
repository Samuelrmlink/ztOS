# SPDX-License-Identifier: GPL-3.0-or-later
# ztpods-qt and ztos-podsd as an RPM for Rocky Linux / RHEL / Fedora (README.md).
#   ztmetro/pods/qt/rpm.sh   builds it in a Rocky 10 container, into out/rpm/
# The source tarball is `git archive` of ztmetro/pods, as ztpods-qt-VERSION/.
%global commit %{?commit}%{!?commit:unknown}

Name:           ztpods-qt
Version:        0.1.0
Release:        2%{?dist}
Summary:        AirPods batteries, listening modes and settings for a Qt desktop
License:        GPL-3.0-or-later
URL:            https://github.com/Samuelrmlink/ztOS
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(Qt6Widgets)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  systemd-rpm-macros
BuildRequires:  desktop-file-utils
# ctest runs the window offscreen
BuildRequires:  qt6-qtbase-gui
# Qt 6, libsystemd and glibc are added by RPM from what the binaries link against
Requires:       bluez
Requires:       dbus
Requires:       qt6-qtbase-gui
# the Wayland platform plugin, for KDE Plasma on Wayland
Requires:       qt6-qtwayland
Requires:       hicolor-icon-theme
%{?systemd_requires}

%description
ztOS's AirPods app as a normal Qt 6 Widgets desktop app (ztpods-qt), and
ztos-podsd, the user service that talks to the AirPods: battery for the pods
and the case (from their broadcasts too, once they have connected here),
listening modes and their settings over Apple's AAP channel. Built from
ztmetro/pods in ztOS, commit %{commit}.

The service runs per user and is enabled for new logins by a preset; on an
existing login run: systemctl --user enable --now ztos-podsd

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -S ztmetro/pods/qt \
	-DZTPODS_DAEMON=BUILD \
	-DZTPODS_SYSTEMD_USER_DIR=%{_userunitdir} \
	-DZTPODS_TESTS=ON
%cmake_build

%install
%cmake_install
install -Dm644 /dev/stdin %{buildroot}%{_userpresetdir}/80-ztpods-qt.preset <<'EOF'
enable ztos-podsd.service
EOF

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/ztpods-qt.desktop
export QT_QPA_PLATFORM=offscreen
%ctest

%post
%systemd_user_post ztos-podsd.service

%preun
%systemd_user_preun ztos-podsd.service

%files
%license LICENSE
%doc ztmetro/pods/qt/README.md
%{_bindir}/ztpods-qt
%{_bindir}/ztos-podsd
%{_userunitdir}/ztos-podsd.service
%{_userpresetdir}/80-ztpods-qt.preset
%{_datadir}/applications/ztpods-qt.desktop
%{_datadir}/icons/hicolor/scalable/apps/ztpods-qt.svg

%changelog
* Sat Nov 07 2026 Samuel Hedrick <choptec@gmail.com> - 0.1.0-2
- ztos-podsd: ask for the device list again when a busy bluetoothd times it out,
  and for a pair's properties when it is first heard of in a partial change, so
  a pair connected at start is seen as connected; a discovery start that timed
  out or said InProgress is still stopped at the end of its burst
* Sat Nov 07 2026 Samuel Hedrick <choptec@gmail.com> - 0.1.0-1
- First package: ztpods-qt and ztos-podsd from ztOS
