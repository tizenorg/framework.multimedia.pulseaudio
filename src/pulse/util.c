/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/winsock.h>
#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/usergroup.h>

#include "util.h"

char *pa_get_user_name(char *s, size_t l) {
    const char *p;
    char *name = NULL;
#ifdef OS_IS_WIN32
    char buf[1024];
#endif

#ifdef HAVE_PWD_H
    struct passwd *r;
#endif

    pa_assert(s);
    pa_assert(l > 0);

    if ((p = (getuid() == 0 ? "root" : NULL)) ||
        (p = getenv("USER")) ||
        (p = getenv("LOGNAME")) ||
        (p = getenv("USERNAME")))
    {
        name = pa_strlcpy(s, p, l);
    } else {
#ifdef HAVE_PWD_H

        if ((r = pa_getpwuid_malloc(getuid())) == NULL) {
            pa_snprintf(s, l, "%lu", (unsigned long) getuid());
            return s;
        }

        name = pa_strlcpy(s, r->pw_name, l);
        pa_getpwuid_free(r);

#elif defined(OS_IS_WIN32) /* HAVE_PWD_H */
        DWORD size = sizeof(buf);

        if (!GetUserName(buf, &size)) {
            errno = ENOENT;
            return NULL;
        }

        name = pa_strlcpy(s, buf, l);

#else /* HAVE_PWD_H */

        return NULL;
#endif /* HAVE_PWD_H */
    }

    return name;
}

char *pa_get_host_name(char *s, size_t l) {

    pa_assert(s);
    pa_assert(l > 0);

    if (gethostname(s, l) < 0)
        return NULL;

    s[l-1] = 0;
    return s;
}

char *pa_get_home_dir(char *s, size_t l) {
    char *e, *dir;

#ifdef HAVE_PWD_H
    struct passwd *r;
#endif

    pa_assert(s);
    pa_assert(l > 0);

    if ((e = getenv("HOME")))
        return pa_strlcpy(s, e, l);

    if ((e = getenv("USERPROFILE")))
        return pa_strlcpy(s, e, l);

#ifdef HAVE_PWD_H
    errno = 0;
    if ((r = pa_getpwuid_malloc(getuid())) == NULL) {
        if (!errno)
            errno = ENOENT;

        return NULL;
    }

    dir = pa_strlcpy(s, r->pw_dir, l);

    pa_getpwuid_free(r);

    return dir;
#else /* HAVE_PWD_H */

    errno = ENOENT;
    return NULL;
#endif
}

char *pa_get_binary_name(char *s, size_t l) {

    pa_assert(s);
    pa_assert(l > 0);

#if defined(OS_IS_WIN32)
    {
        char path[PATH_MAX];

        if (GetModuleFileName(NULL, path, PATH_MAX))
            return pa_strlcpy(s, pa_path_get_filename(path), l);
    }
#endif

#ifdef __linux__
    {
        char *rp;
        /* This works on Linux only */

        if ((rp = pa_readlink("/proc/self/exe"))) {
            pa_strlcpy(s, pa_path_get_filename(rp), l);
            pa_xfree(rp);
            return s;
        }
    }

#endif

#if defined(HAVE_SYS_PRCTL_H) && defined(PR_GET_NAME)
    {

        #ifndef TASK_COMM_LEN
        /* Actually defined in linux/sched.h */
        #define TASK_COMM_LEN 16
        #endif

        char tcomm[TASK_COMM_LEN+1];
        memset(tcomm, 0, sizeof(tcomm));

        /* This works on Linux only */
        if (prctl(PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
            return pa_strlcpy(s, tcomm, l);

    }
#endif

    errno = ENOENT;
    return NULL;
}

char *pa_path_get_filename(const char *p) {
    char *fn;

    if (!p)
        return NULL;

    if ((fn = strrchr(p, PA_PATH_SEP_CHAR)))
        return fn+1;

    return (char*) p;
}

char *pa_get_fqdn(char *s, size_t l) {
    char hn[256];
#ifdef HAVE_GETADDRINFO
    struct addrinfo *a, hints;
#endif

    pa_assert(s);
    pa_assert(l > 0);

    if (!pa_get_host_name(hn, sizeof(hn)))
        return NULL;

#ifdef HAVE_GETADDRINFO
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_CANONNAME;

    if (getaddrinfo(hn, NULL, &hints, &a) < 0 || !a || !a->ai_canonname || !*a->ai_canonname)
        return pa_strlcpy(s, hn, l);

    pa_strlcpy(s, a->ai_canonname, l);
    freeaddrinfo(a);
    return s;
#else
    return pa_strlcpy(s, hn, l);
#endif
}

int pa_msleep(unsigned long t) {
#ifdef OS_IS_WIN32
    Sleep(t);
    return 0;
#elif defined(HAVE_NANOSLEEP)
    struct timespec ts;

    ts.tv_sec = (time_t) (t / PA_MSEC_PER_SEC);
    ts.tv_nsec = (long) ((t % PA_MSEC_PER_SEC) * PA_NSEC_PER_MSEC);

    return nanosleep(&ts, NULL);
#else
#error "Platform lacks a sleep function."
#endif
}
