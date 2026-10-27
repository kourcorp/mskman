Name:           mskman
Version:        0.2.0
Release:        1%{?dist}
Summary:        Maze chase arcade game
License:        MIT
URL:            https://github.com/kourcorp/mskman
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  desktop-file-utils

%description
MSKman is a retro maze chase game for the Linux desktop. Eat every dot,
grab the energizers to turn the tables on four ghosts, and collect bonus
fruit on a new maze every level. Up to four players take turns, and the
options screen tunes cornering and speeds. All graphics and sounds are
original and generated at runtime.

%prep
%autosetup

%build
%set_build_flags
%make_build

%install
%make_install PREFIX=%{_prefix}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop
make check

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
%{_mandir}/man6/%{name}.6*

%changelog
* Tue Oct 27 2026 kourcorp <263039746+kourcorp@users.noreply.github.com> - 0.2.0-1
- Turn-based play for up to four players
- A newly generated maze for every level after the first two
- Options screen with corner assist and player/ghost speed settings

* Sun Oct 11 2026 kourcorp <263039746+kourcorp@users.noreply.github.com> - 0.1.0-1
- First playable version
