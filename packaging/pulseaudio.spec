%define conf_option --disable-static --enable-alsa --disable-ipv6 --disable-oss-output --disable-oss-wrapper --enable-dlog --enable-bluez --disable-hal --disable-hal-compat --disable-legacy-runtime-dir --with-udev-rules-dir=/usr/lib/udev/rules.d

Name:       pulseaudio
Summary:    Improved Linux sound server
%if 0%{?tizen_profile_mobile}
Version:    0.9.23
Release:    35
%else
Version:    4.0.111
Release:    1
%endif
Group:      Multimedia/PulseAudio
License:    LGPLv2+
URL:        http://pulseaudio.org
Source0:    http://0pointer.de/lennart/projects/pulseaudio/pulseaudio-%{version}.tar.gz
%if "%{_repository}" == "mobile"
Source1:    pulseaudio.service.mobile
%else
Source1:    pulseaudio.service.wearable
%endif
Source2:    pulseaudio.rule
%if "%{_repository}" == "mobile"
Requires:   udev
Requires:   systemd
Requires(post):   bluez
%else
Requires:   systemd >= 183
Requires:   dbus
Requires:   bluez
%endif
Requires(preun):  /usr/bin/systemctl
Requires(post):   /usr/bin/systemctl
Requires(postun): /usr/bin/systemctl
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
Requires(post): sys-assert
BuildRequires:  pkgconfig(speexdsp)
BuildRequires:  pkgconfig(sndfile)
BuildRequires:  pkgconfig(alsa)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(bluez)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(dlog)
BuildRequires:  pkgconfig(vconf)
BuildRequires:  pkgconfig(security-server)
BuildRequires:  m4
BuildRequires:  libtool-ltdl-devel
BuildRequires:  libtool
BuildRequires:  intltool
BuildRequires:  fdupes
%if "%{_repository}" == "wearable"
BuildRequires:  pkgconfig(libascenario)
BuildRequires:  pkgconfig(json)
BuildRequires:  pkgconfig(sbc)
BuildRequires:  pkgconfig(iniparser)
%endif


%if 0%{?tizen_profile_mobile}
%define pulseversion  0.9.23
%else
%define pulseversion  4.0
%endif

%description
PulseAudio is a sound server for Linux and other Unix like operating
systems. It is intended to be an improved drop-in replacement for the
Enlightened Sound Daemon (ESOUND).

%package libs
Summary:    PulseAudio client libraries
Group:      Multimedia/PulseAudio
Requires:   %{name} = %{version}-%{release}
Requires:   /bin/sed
%if 0%{?tizen_profile_mobile}
Requires(post): /sbin/syslogd
%endif

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

%if 0%{?tizen_profile_mobile}
%package module-devel
Summary:    Headers for PulseAudio module development
Group:      Development/Libraries
Requires:   %{name}-libs-devel = %{version}-%{release}

%description module-devel
Headers for developing pulseaudio modules
%endif

%prep
%setup -q


%build
%if 0%{?tizen_profile_mobile}
cd ./mobile
unset LD_AS_NEEDED
export CFLAGS+=" -DPA_EXT_USE_VOLUME_FADING"
export LDFLAGS+="-Wl,--no-as-needed"
%ifarch %{ix86}
%reconfigure %{conf_option}
%else
%reconfigure %{conf_option} --enable-bt-a2dp-aptx --enable-security
%endif
make %{?jobs:-j%jobs}

%else
cd ./wearable
export NORECONFIGURE=yes
./bootstrap.sh
%if 0%{?sec_build_binary_debug_enable}
export CFLAGS="$CFLAGS -DTIZEN_DEBUG_ENABLE"
export CXXFLAGS="$CXXFLAGS –DTIZEN_DEBUG_ENABLE"
export FFLAGS="$FFLAGS -DTIZEN_DEBUG_ENABLE"
%endif
%if 0%{?sec_product_feature_mmfw_audio_qc_ucm}
export CFLAGS+=" -DPA_USE_QCOM_VOIP -DPA_USE_QCOM_UCM -DPA_DISABLE_MONO_AUDIO"
%endif
%if 0%{?sec_build_binary_sdk}
export CFLAGS+=" -D__SDK__"
%endif
unset LD_AS_NEEDED
%ifarch %{arm}
export CFLAGS+=" -mfloat-abi=softfp -mfpu=neon"
%endif
export CFLAGS+=" -D__TIZEN__ -D__TIZEN_BT__ -D__TIZEN_LOG__"
export LDFLAGS+="-Wl,--no-as-needed"
%ifarch %{ix86}
%configure %{conf_option}
%else
%configure %{conf_option} --enable-security
%endif
make %{?_smp_mflags}
%endif


%install
%if 0%{?tizen_profile_mobile}
cd ./mobile
%make_install
mkdir -p %{buildroot}/usr/share/license
cp LGPL %{buildroot}/usr/share/license/%{name}
cp LGPL %{buildroot}/usr/share/license/%{name}-libs
cp LGPL %{buildroot}/usr/share/license/%{name}-utils
cp LGPL %{buildroot}/usr/share/license/%{name}-module-bluetooth

mkdir -p %{buildroot}%{_libdir}/systemd/system
install -m 644 %{SOURCE1} %{buildroot}%{_libdir}/systemd/system/
mv %{buildroot}%{_libdir}/systemd/system/pulseaudio.service.mobile %{buildroot}%{_libdir}/systemd/system/pulseaudio.service
mkdir -p  %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants
ln -s  ../pulseaudio.service  %{buildroot}%{_libdir}/systemd/system/multi-user.target.wants/pulseaudio.service
mkdir -p %{buildroot}/opt/etc/smack/accesses.d
install -m 644 %{SOURCE2} %{buildroot}/opt/etc/smack/accesses.d/pulseaudio.rule

# FIXME: remove initscripts once systemd is enabled
install -D -m0755 pulseaudio.sh.in %{buildroot}%{_sysconfdir}/rc.d/init.d/pulseaudio.sh
install -d %{buildroot}%{_sysconfdir}/rc.d/rc3.d
install -d %{buildroot}%{_sysconfdir}/rc.d/rc4.d
ln -s ../init.d/pulseaudio.sh %{buildroot}%{_sysconfdir}/rc.d/rc3.d/S20pulseaudio
ln -s ../init.d/pulseaudio.sh %{buildroot}%{_sysconfdir}/rc.d/rc4.d/S20pulseaudio

# Export pulsecore headers for PA modules development
install -d %{buildroot}/usr/include/pulsecore
install -m 644 src/pulsecore/*.h %{buildroot}/usr/include/pulsecore/

%else
cd ./wearable

%make_install
mkdir -p %{buildroot}/%{_datadir}/license
mkdir -p %{buildroot}/opt/usr/devel/usr/bin
cp LGPL %{buildroot}/%{_datadir}/license/%{name}
cp LGPL %{buildroot}/%{_datadir}/license/pulseaudio-libs
cp LGPL %{buildroot}/%{_datadir}/license/pulseaudio-utils
cp LGPL %{buildroot}/%{_datadir}/license/pulseaudio-module-bluetooth

mkdir -p %{buildroot}%{_libdir}/systemd/system/sound.target.wants
install -m 644 %{SOURCE1} %{buildroot}%{_libdir}/systemd/system/
mv %{buildroot}%{_libdir}/systemd/system/pulseaudio.service.wearable %{buildroot}%{_libdir}/systemd/system/pulseaudio.service
ln -s  ../pulseaudio.service  %{buildroot}%{_libdir}/systemd/system/sound.target.wants/pulseaudio.service
mv %{buildroot}/usr/bin/pa* %{buildroot}/opt/usr/devel/usr/bin

%endif

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
%if 0%{?tizen_profile_wearable}
/usr/bin/vconftool set -t int memory/Sound/SoundCaptureStatus 0 -g 29 -f -i -s system::vconf_multimedia
/usr/bin/vconftool set -t int memory/private/sound/pcm_dump 0 -g 29 -f -i -s system::vconf_multimedia
%endif
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


%files
%if 0%{?tizen_profile_mobile}
%manifest mobile/pulseaudio.manifest
%{_bindir}/pamon
%exclude %config(noreplace) %{_sysconfdir}/pulse/daemon.conf
%exclude %config(noreplace) %{_sysconfdir}/pulse/default.pa
%exclude %config(noreplace) %{_sysconfdir}/pulse/system.pa
%{_sysconfdir}/rc.d/init.d/pulseaudio.sh
%{_sysconfdir}/rc.d/rc3.d/S20pulseaudio
%{_sysconfdir}/rc.d/rc4.d/S20pulseaudio
%{_bindir}/esdcompat
%dir %{_libexecdir}/pulse
%{_libexecdir}/pulse/*
%else
%manifest wearable/pulseaudio.manifest
%exclude %{_bindir}/esdcompat
%exclude %{_libexecdir}/pulse/proximity-helper
%endif

%{_sysconfdir}/pulse/filter/*.dat
%exclude %{_sysconfdir}/pulse/daemon.conf
%exclude %{_sysconfdir}/pulse/default.pa
%exclude %{_sysconfdir}/pulse/system.pa
%dir %{_sysconfdir}/pulse/
%{_bindir}/pulseaudio
%{_libdir}/libpulsecore-%{pulseversion}.so
%exclude %{_libdir}/libpulse-mainloop-glib.so.*
%{_libdir}/udev/rules.d/90-pulseaudio.rules
%exclude %{_datadir}/pulseaudio/alsa-mixer/paths/*
%exclude %{_datadir}/pulseaudio/alsa-mixer/profile-sets/*
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
%exclude %{_libdir}/pulse-%{pulseversion}/modules/libraop.so
%exclude %{_libdir}/pulse-%{pulseversion}/modules/module-raop-sink.so
%exclude /usr/share/vala/vapi/libpulse-mainloop-glib.deps
%exclude /usr/share/vala/vapi/libpulse-mainloop-glib.vapi
%exclude /usr/share/vala/vapi/libpulse.deps
%exclude /usr/share/vala/vapi/libpulse.vapi
%{_libdir}/systemd/system/pulseaudio.service

%if 0%{?tizen_profile_mobile}
%{_libdir}/pulse-%{pulseversion}/modules/module-cork-music-on-phone.so
%{_libdir}/systemd/system/multi-user.target.wants/pulseaudio.service
/opt/etc/smack/accesses.d/pulseaudio.rule
/usr/share/license/%{name}
%else
%{_libdir}/systemd/system/sound.target.wants/pulseaudio.service
%exclude %{_libdir}/cmake/PulseAudio/PulseAudioConfig.cmake
%exclude %{_libdir}/cmake/PulseAudio/PulseAudioConfigVersion.cmake
%{_libdir}/pulse-%{pulseversion}/modules/module-combine-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-dbus-protocol.so
%{_libdir}/pulse-%{pulseversion}/modules/module-filter-apply.so
%{_libdir}/pulse-%{pulseversion}/modules/module-filter-heuristics.so
%{_libdir}/pulse-%{pulseversion}/modules/module-null-source.so
%{_libdir}/pulse-%{pulseversion}/modules/module-remap-source.so
%{_libdir}/pulse-%{pulseversion}/modules/module-role-cork.so
%{_libdir}/pulse-%{pulseversion}/modules/module-role-ducking.so
%{_libdir}/pulse-%{pulseversion}/modules/module-switch-on-connect.so
%{_libdir}/pulse-%{pulseversion}/modules/module-switch-on-port-available.so
%{_libdir}/pulse-%{pulseversion}/modules/module-systemd-login.so
%{_libdir}/pulse-%{pulseversion}/modules/module-virtual-sink.so
%{_libdir}/pulse-%{pulseversion}/modules/module-virtual-source.so
%{_libdir}/pulse-%{pulseversion}/modules/module-virtual-surround-sink.so
%{_datadir}/license/%{name}
%endif
%exclude %{_mandir}/*
%exclude /usr/share/locale/*

%files libs
%if 0%{?tizen_profile_mobile}
%defattr(-,root,root,-)
%exclude %config(noreplace) %{_sysconfdir}/pulse/client.conf
%{_libdir}/libpulsecommon-*.so
/usr/share/license/%{name}-libs
%else
%manifest wearable/pulseaudio_shared.manifest
%exclude %{_sysconfdir}/pulse/client.conf
%{_libdir}/pulseaudio/libpulsecommon-*.so
%{_datadir}/license/pulseaudio-libs
%endif

%{_libdir}/libpulse.so.*
%{_libdir}/libpulse-simple.so.*

%files libs-devel
%if 0%{?tizen_profile_mobile}
%defattr(-,root,root,-)
%endif
%{_includedir}/pulse/*
#%{_includedir}/pulse-modules-headers/pulsecore/
%{_libdir}/libpulse.so
%{_libdir}/libpulse-simple.so
%{_libdir}/pkgconfig/libpulse-simple.pc
%{_libdir}/pkgconfig/libpulse.pc
%exclude %{_libdir}/pkgconfig/libpulse-mainloop-glib.pc
%exclude %{_libdir}/libpulse-mainloop-glib.so

%if 0%{?tizen_profile_mobile}
%files module-devel
%defattr(-,root,root)
%{_includedir}/pulsecore/*
%endif

%files utils
%if 0%{?tizen_profile_mobile}
%defattr(-,root,root,-)
%{_bindir}/pacat
%{_bindir}/pacmd
%{_bindir}/pactl
#%{_bindir}/padsp
%{_bindir}/paplay
%{_bindir}/parec
%{_bindir}/pamon
%{_bindir}/parecord
%{_bindir}/pasuspender
/usr/share/license/%{name}-utils
%else
%manifest wearable/pulseaudio_shared.manifest
/opt/usr/devel/usr/bin/pacat
/opt/usr/devel/usr/bin/pacat-simple
/opt/usr/devel/usr/bin/pacmd
/opt/usr/devel/usr/bin/pactl
/opt/usr/devel/usr/bin/paplay
/opt/usr/devel/usr/bin/parec
/opt/usr/devel/usr/bin/pamon
/opt/usr/devel/usr/bin/parecord
%exclude /opt/usr/devel/usr/bin/pasuspender
%{_datadir}/license/pulseaudio-utils
%endif

%files module-bluetooth
%if 0%{?tizen_profile_mobile}
%defattr(-,root,root,-)
%{_libdir}/pulse-%{pulseversion}/modules/libbluetooth-ipc.so
%{_libdir}/pulse-%{pulseversion}/modules/libbluetooth-sbc.so
#%{_libdir}/pulseaudio/pulse/proximity-helper
/usr/share/license/%{name}-module-bluetooth
%else
%manifest wearable/pulseaudio_shared.manifest
%exclude /etc/bash_completion.d/pulseaudio-bash-completion.sh
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-policy.so
%{_datadir}/license/pulseaudio-module-bluetooth
%endif
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-proximity.so
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-device.so
%{_libdir}/pulse-%{pulseversion}/modules/module-bluetooth-discover.so
%{_libdir}/pulse-%{pulseversion}/modules/libbluetooth-util.so


