Name:             umockdev
Version:          0
Release:          1%{?dist}
Summary:          Mock hardware devices

License:          LGPL-2.1-or-later
URL:              https://github.com/martinpitt/%{name}
Source0:          https://github.com/martinpitt/%{name}/releases/download/%{version}/%{name}-%{version}.tar.xz

BuildRequires:    git
BuildRequires:    meson
BuildRequires:    gtk-doc
BuildRequires:    gobject-introspection-devel
BuildRequires:    glib2-devel
BuildRequires:    libgudev1-devel systemd-devel
BuildRequires:    libpcap-devel
BuildRequires:    vala
BuildRequires:    chrpath
BuildRequires:    systemd-udev

%description
With this program and libraries you can easily create mock udev objects.
This is useful for writing tests for software which talks to
hardware devices.

%package devel
Summary: Development packages for %{name}
Requires: %{name}%{?_isa} = %{version}-%{release}

%description devel
The %{name}-devel package contains the libraries to develop
using %{name}.

%prep
%autosetup -S git -n %{name}-%{version}

%build
%meson -Dgtk_doc=true
%meson_build

%check
# don't be too picky about timing; upstream CI and local developer tests
# are strict, but many koji arches are emulated and utterly slow
export SLOW_TESTBED_FACTOR=10
%meson_test

%install
%meson_install

# Remove rpath
chrpath --delete %{buildroot}%{_bindir}/umockdev-record \
	%{buildroot}%{_bindir}/umockdev-run
chrpath --delete %{buildroot}%{_libdir}/libumockdev.so.*
chrpath --delete %{buildroot}%{_libdir}/libumockdev-preload.so.*

rm -rf $RPM_BUILD_ROOT/%{_datadir}/doc/umockdev

%ldconfig_scriptlets

%files
%license COPYING
%doc README.md
%{_bindir}/umockdev-*
%{_libdir}/libumockdev.so.*
%{_libdir}/libumockdev-preload.so*
%{_libdir}/girepository-1.0/UMockdev-1.0.typelib

%files devel
%doc docs/script-format.txt docs/examples/battery.c docs/examples/battery.py
%{_libdir}/libumockdev.so
%{_libdir}/pkgconfig/umockdev-1.0.pc
%{_datadir}/gir-1.0/UMockdev-1.0.gir
%{_includedir}/umockdev-1.0
%{_datadir}/gtk-doc/html/umockdev/
%{_datadir}/vala/vapi/umockdev-1.0.vapi

%changelog
