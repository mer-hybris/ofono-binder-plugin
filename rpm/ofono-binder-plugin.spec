Name: ofono-binder-plugin

Version: 1.0.1
Release: 1
Summary: Binder based ofono plugin
License: GPLv2
URL: https://github.com/mer-hybris/ofono-binder-plugin
Source: %{name}-%{version}.tar.bz2

%define libglibutil_version 1.0.61
%define libgbinder_version 1.1.15
%define libgbinder_radio_version 1.4.8
%define libmce_version 1.0.6
%define ofono_version 1.28+git2

BuildRequires: pkgconfig
BuildRequires: ofono-devel >= %{ofono_version}
BuildRequires: pkgconfig(libgbinder) >= %{libgbinder_version}
BuildRequires: pkgconfig(libgbinder-radio) >= %{libgbinder_radio_version}
BuildRequires: pkgconfig(libglibutil) >= %{libglibutil_version}
BuildRequires: pkgconfig(libmce-glib) >= %{libmce_version}

# license macro requires rpm >= 4.11
BuildRequires: pkgconfig(rpm)
%define license_support %(pkg-config --exists 'rpm >= 4.11'; echo $?)

Requires: ofono >= %{ofono_version}
Requires: libgbinder >= %{libgbinder_version}
Requires: libgbinder-radio >= %{libgbinder_radio_version}
Requires: libglibutil >= %{libglibutil_version}
Requires: libmce-glib >= %{libmce_version}

Conflicts: ofono-ril-plugin
Obsoletes: ofono-ril-plugin
Conflicts: ofono-ril-binder-plugin
Obsoletes: ofono-ril-binder-plugin

%define plugin_dir %(pkg-config ofono --variable=plugindir)
%define config_dir /etc/ofono/

%description
Binder plugin for Sailfish OS fork of ofono

%package -n ofono-configs-binder
Summary: Package to provide default binder configs for ofono
Provides: ofono-configs

%description -n ofono-configs-binder
This package provides default configs for ofono

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} PLUGINDIR=%{plugin_dir} KEEP_SYMBOLS=1 release

%check
make test

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} install
mkdir -p %{buildroot}%{config_dir}
install -m 644 binder.conf %{buildroot}%{config_dir}

%files
%dir %{plugin_dir}
%defattr(-,root,root,-)
%{plugin_dir}/binderplugin.so
%if %{license_support} == 0
%license LICENSE
%endif

%files -n ofono-configs-binder
%dir %{config_dir}
%defattr(-,root,root,-)
%config %{config_dir}/binder.conf
