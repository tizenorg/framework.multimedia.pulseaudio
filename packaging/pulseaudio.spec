%define pulseversion  0.9.23
%define conf_option --disable-static --enable-alsa --disable-ipv6 --disable-oss-output --disable-oss-wrapper --enable-dlog --enable-bluez --disable-hal --disable-hal-compat --disable-legacy-runtime-dir --with-udev-rules-dir=/usr/lib/udev/rules.d

Name:       pulseaudio
Summary:    Improved Linux sound server
Version:    0.9.23
Release:    32
Group:      Multimedia/PulseAudio
License:    LGPLv2+
URL:        http://pulseaudio.org
Source0:    http://0pointer.de/lennart/projects/pulseaudio/pulseaudio-%{version}.tar.gz
Source1:    pulseaudio.service
Requires:   udev
Requires:   power-manager
Requires:   systemd
Requires(post):   bluez
Requires(preun):  /usr/bin/systemctl
Requires(post):   /usr/bin/systemctl
Requires(postun): /usr/bin/systemctl
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
BuildRequires:  pkgconfig(speexdsp)
BuildRequires:  pkgconfig(sndfile)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(bluez)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(dlog)
BuildRequires:  pkgconfig(vconf)
BuildRequires:  m4
BuildRequires:  libtool-ltdl-devel
BuildRequires:  libtool
BuildRequires:  intltool
BuildRequires:  fdupes
Requires(post): system-server


%description
PulseAudio is a sound server for Linux and other Unix like operating
systems. It is intended to be an improved drop-in replacement for the
Enlightened Sound Daemon (ESOUND).

%package libs
Summary:    PulseAudio client libraries
Group:      Multimedia/PulseAudio
Requires:   %{name} = %{version}-%{release}
Requires:   /bin/sed

%description libs
Client libraries used by applications that access a PulseAudio sound server
via PulseAudio's native interface.


%package libs-devel
Summary:    PulseAudio client development headers and libraries
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description libs-devel
Headers and libraries for developing applications that access a PulseAudio
 sound server via PulseAudio's native interface


%package utils
Summary:    Command line tools for the PulseAudio sound server
Group:      Multimedia/PulseAudio
Requires:   %{name} = %{version}-%{release}
Requires:   /bin/sed

%description utils
These tools provide command line access to various features of the
PulseAudio sound server. Included tools are:
   pabrowse - Browse available PulseAudio servers on the local network.
   paplay - Playback a WAV file via a PulseAudio sink.
   pacat - Cat raw audio data to a PulseAudio sink.
   parec - Cat raw audio data from a PulseAudio source.
   pacmd - Connect to PulseAudio's built-in command line control interface.
   pactl - Send a control command to a PulseAudio server.
   padsp - /dev/dsp wrapper to transparently support OSS applications.

%package module-bluetooth
Summary:    Bluetooth module for PulseAudio sound server
Group:      Multimedia/PulseAudio
Requires:   %{name} = %{version}-%{release}
Requires:   /bin/sed

%description module-bluetooth
This module enables PulseAudio to work with bluetooth devices, like headset
 or audio gatewa

%package module-devel
Summary:    Headers for PulseAudio module development
Group:      Development/Libraries
Requires:   %{name}-libs-devel = %{version}-%{release}

%description module-devel
Headers for developing pulseaudio modules

%prep
%setup -q


%build
unset LD_AS_NEEDED
export CFLAGS+=" -DPA_EXT_USE_VOLUME_FADING"
export LDFLAGS+="-Wl,--no-as-needed"
%ifarch %{ix86}
%reconfigure %{conf_option}
%else
%reconfigure %{conf_option} --enable-bt-a2dp-aptx
%endif
make %{?jobs:-j%jobs}

%install
%make_install

mkdir -p %{buildroot}%{_libdir}/systemd/system
install -m 644 %{SOURCE1} %{buildroot}%{_libdir}/systemd/system/pulseaudio.service
mkdir -p  %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants
ln -s  ../pulseaudio.service  %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants/pulseaudio.service

# FIXME: remove initscripts once systemd is enabled
install -D -m0755 pulseaudio.sh.in %{buildroot}%{_sysconfdir}/rc.d/init.d/pulseaudio.sh
install -d %{buildroot}%{_sysconfdir}/rc.d/rc3.d
install -d %{buildroot}%{_sysconfdir}/rc.d/rc4.d
ln -s ../init.d/pulseaudio.sh %{buildroot}%{_sysconfdir}/rc.d/rc3.d/S20pulseaudio
ln -s ../init.d/pulseaudio.sh %{buildroot}%{_sysconfdir}/rc.d/rc4.d/S20pulseaudio

# Export pulsecore headers for PA modules development
install -d %{buildroot}/usr/include/pulsecore
install -m 644 src/pulsecore/*.h %{buildroot}/usr/include/pulsecore/

pushd %{buildroot}/etc/pulse/filter
ln -sf filter_8000_44100.dat filter_11025_44100.dat
ln -sf filter_8000_44100.dat filter_12000_44100.dat
ln -sf filter_8000_44100.dat filter_16000_44100.dat
ln -sf filter_8000_44100.dat filter_22050_44100.dat
ln -sf filter_8000_44100.dat filter_24000_44100.dat
ln -sf filter_8000_44100.dat filter_32000_44100.dat
popd

rm -f %{buildroot}/etc/xdg/autostart/pulseaudio-kde.desktop
rm -f %{buildroot}/usr/bin/start-pulseaudio-kde
rm -f %{buildroot}/usr/bin/start-pulseaudio-x11

%find_lang pulseaudio
%fdupes  %{buildroot}/%{_datadir}
%fdupes  %{buildroot}/%{_includedir}


%preun
if [ $1 == 0 ]; then
    systemctl stop pulseaudio.service
fi

%post
/sbin/ldconfig
systemctl daemon-reload
if [ $1 == 1 ]; then
    systemctl restart pulseaudio.service
fi

%postun
/sbin/ldconfig
systemctl daemon-reload

%post libs -p /sbin/ldconfig

%postun libs -p /sbin/ldconfig

%post module-bluetooth -p /sbin/ldconfig

%postun module-bluetooth -p /sbin/ldconfig


%docs_package

%lang_package

%files
%manifest pulseaudio.manifest
%doc LICENSE GPL LGPL
%{_sysconfdir}/pulse/filter/*.dat
%exclude %{_sysconfdir}/pulse/daemon.conf
%exclude %{_sysconfdir}/pulse/default.pa
%exclude %{_sysconfdir}/pulse/system.pa
%dir %{_sysconfdir}/pulse/
%exclude %config(noreplace) %{_sysconfdir}/pulse/daemon.conf
%exclude %config(noreplace) %{_sysconfdir}/pulse/default.pa
%exclude %config(noreplace) %{_sysconfdir}/pulse/system.pa
%{_sysconfdir}/rc.d/init.d/pulseaudio.sh
%{_sysconfdir}/rc.d/rc3.d/S20pulseaudio
%{_sysconfdir}/rc.d/rc4.d/S20pulseaudio
%{_bindir}/esdcompat
%{_bindir}/pulseaudio
%dir %{_libexecdir}/pulse
%{_libexecdir}/pulse/*
%{_libdir}/libpulsecore-%{pulseversion}.so
%exclude %{_libdir}/libpulse-mainloop-glib.so.*
%{_libdir}/udev/rules.d/90-pulseaudio.rules
%exclude %{_datadir}/pulseaudio/alsa-mixer/paths/*
%exclude %{_datadir}/pulseaudio/alsa-mixer/profile-sets/*
%{_bindir}/pamon
%{_sysconfdir}/dbus-1/system.d/pulseaudio-system.conf
#list all modules
%{_libdir}/pulse-%{pulseversion}/modules/libalsa-util.so
%{_libdir}/pulse-%{pulseversion}/modules/libcli.so
%{_libdir}/pulse-%{pulseversion}/modules/libprotocol-cli.so
%{_libdir}/pulse-%{pulseversion}/modules/libprotocol-http.so
%{_libdir}/pulse-%{pulseversion}/modules/libprotocol-native.so
%{_libdir}/pulse-%{pulseversion}/modules/libprotocol-simple.so
%{_libdir}/pulse-%{pulseversion}/modules/librtp.so
%{_libdir}/pulse-%{pulseversion}/modules/module-alsa-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-alsa-source.so
%{_libdir}/pulse-%{pulseversion}/modules/module-always-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-console-kit.so
%{_libdir}/pulse-%{pulseversion}/modules/module-device-restore.so
%{_libdir}/pulse-%{pulseversion}/modules/module-device-manager.so
%{_libdir}/pulse-%{pulseversion}/modules/module-stream-restore.so
%{_libdir}/pulse-%{pulseversion}/modules/module-cli-protocol-tcp.so
%{_libdir}/pulse-%{pulseversion}/modules/module-cli-protocol-unix.so
%{_libdir}/pulse-%{pulseversion}/modules/module-cli.so
%{_libdir}/pulse-%{pulseversion}/modules/module-combine.so
%{_libdir}/pulse-%{pulseversion}/modules/module-default-device-restore.so
%{_libdir}/pulse-%{pulseversion}/modules/module-detect.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-esound-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-http-protocol-tcp.so
%{_libdir}/pulse-%{pulseversion}/modules/module-http-protocol-unix.so
%{_libdir}/pulse-%{pulseversion}/modules/module-intended-roles.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-ladspa-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-match.so
%{_libdir}/pulse-%{pulseversion}/modules/module-mmkbd-evdev.so
%{_libdir}/pulse-%{pulseversion}/modules/module-native-protocol-fd.so
%{_libdir}/pulse-%{pulseversion}/modules/module-native-protocol-tcp.so
%{_libdir}/pulse-%{pulseversion}/modules/module-native-protocol-unix.so
%{_libdir}/pulse-%{pulseversion}/modules/module-null-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-pipe-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-pipe-source.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-position-event-sounds.so
%{_libdir}/pulse-%{pulseversion}/modules/module-remap-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-rescue-streams.so
%{_libdir}/pulse-%{pulseversion}/modules/module-rtp-recv.so
%{_libdir}/pulse-%{pulseversion}/modules/module-rtp-send.so
%{_libdir}/pulse-%{pulseversion}/modules/module-simple-protocol-tcp.so
%{_libdir}/pulse-%{pulseversion}/modules/module-simple-protocol-unix.so
%{_libdir}/pulse-%{pulseversion}/modules/module-sine.so
%{_libdir}/pulse-%{pulseversion}/modules/module-tunnel-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-tunnel-source.so
%{_libdir}/pulse-%{pulseversion}/modules/module-suspend-on-idle.so
%{_libdir}/pulse-%{pulseversion}/modules/module-volume-restore.so
%{_libdir}/pulse-%{pulseversion}/modules/module-alsa-card.so
%{_libdir}/pulse-%{pulseversion}/modules/module-augment-properties.so
%{_libdir}/pulse-%{pulseversion}/modules/module-card-restore.so
%{_libdir}/pulse-%{pulseversion}/modules/module-cork-music-on-phone.so
%{_libdir}/pulse-%{pulseversion}/modules/module-sine-source.so
%{_libdir}/pulse-%{pulseversion}/modules/module-loopback.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-rygel-media-server.so
%{_libdir}/pulse-%{pulseversion}/modules/module-policy.so
%{_libdir}/pulse-%{pulseversion}/modules/module-echo-cancel.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/libprotocol-esound.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-esound-compat-spawnfd.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-esound-compat-spawnpid.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-esound-protocol-tcp.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-esound-protocol-unix.so
%{_libdir}/pulse-%{pulseversion}/modules/module-udev-detect.so
%exclude /usr/share/vala/vapi/libpulse-mainloop-glib.deps
%exclude /usr/share/vala/vapi/libpulse-mainloop-glib.vapi
%exclude /usr/share/vala/vapi/libpulse.deps
%exclude /usr/share/vala/vapi/libpulse.vapi
%{_libdir}/systemd/system/pulseaudio.service
%{_libdir}/systemd/system/multi-user.target.wants/pulseaudio.service

%files libs
%defattr(-,root,root,-)
%exclude %config(noreplace) %{_sysconfdir}/pulse/client.conf
%{_libdir}/libpulse.so.*
%{_libdir}/libpulse-simple.so.*
%{_libdir}/libpulsecommon-*.so

%files libs-devel
%defattr(-,root,root,-)
%{_includedir}/pulse/*
#%{_includedir}/pulse-modules-headers/pulsecore/
%{_libdir}/libpulse.so
%{_libdir}/libpulse-simple.so
%{_libdir}/pkgconfig/libpulse-simple.pc
%{_libdir}/pkgconfig/libpulse.pc
%exclude %{_libdir}/pkgconfig/libpulse-mainloop-glib.pc
%exclude %{_libdir}/libpulse-mainloop-glib.so

%files module-devel
%defattr(-,root,root)
%{_includedir}/pulsecore/*

%files utils
%defattr(-,root,root,-)
%doc %{_mandir}/man1/pabrowse.1.gz
%doc %{_mandir}/man1/pacat.1.gz
%doc %{_mandir}/man1/pacmd.1.gz
%doc %{_mandir}/man1/pactl.1.gz
#%doc %{_mandir}/man1/padsp.1.gz
%doc %{_mandir}/man1/paplay.1.gz
%doc %{_mandir}/man1/pasuspender.1.gz
%{_bindir}/pacat
%{_bindir}/pacmd
%{_bindir}/pactl
#%{_bindir}/padsp
%{_bindir}/paplay
%{_bindir}/parec
%{_bindir}/pamon
%{_bindir}/parecord
%{_bindir}/pasuspender

%files module-bluetooth
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-proximity.so
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-device.so
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-discover.so
%{_libdir}/pulse-%{pulseversion}/modules/libbluetooth-ipc.so
%{_libdir}/pulse-%{pulseversion}/modules/libbluetooth-sbc.so
%{_libdir}/pulse-%{pulseversion}/modules/libbluetooth-util.so
#%{_libdir}/pulseaudio/pulse/proximity-helper
