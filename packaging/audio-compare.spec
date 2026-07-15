Name:           audio-compare
Version:        0.1.0
Release:        1%{?dist}
Summary:        Compare audio files by ear on GNOME

License:        GPL-3.0-or-later
URL:            https://github.com/mmxgn/audio-compare
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libadwaita-1)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(libebur128)

Requires:       gtk4
Requires:       libadwaita
Requires:       gstreamer1
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Requires:       libebur128

%description
Compare two or more audio files by ear. Load files, see their waveforms,
and A/B them at the same playback position.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install

%files
%{_bindir}/audio-compare
%{_datadir}/applications/org.mmxgn.audiocompare.desktop
%{_datadir}/icons/hicolor/scalable/apps/org.mmxgn.audiocompare.svg
