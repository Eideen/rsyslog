/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/* Portions Copyright 2010 by Rainer Gerhards and Adiscon
 */
/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 *	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T
 *	All Rights Reserved
 */

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

#include "config.h"



/*
 * Attempts to open the local log device
 * and return a file descriptor.
 */
int
sun_oopenklog(char *name, int mode)
{
	int fd;
	struct strioctl str;
	pthread_t mythreadno;

	if (Debug) {
		mythreadno = pthread_self();
	}

	if ((fd = open(name, mode)) < 0) {
		logerror("cannot open %s", name);
		DPRINT3(1, "openklog(%u): cannot create %s (%d)\n",
		    mythreadno, name, errno);
		return (-1);
	}
	str.ic_cmd = I_CONSLOG;
	str.ic_timout = 0;
	str.ic_len = 0;
	str.ic_dp = NULL;
	if (ioctl(fd, I_STR, &str) < 0) {
		logerror("cannot register to log console messages");
		DPRINT2(1, "openklog(%u): cannot register to log "
		    "console messages (%d)\n", mythreadno, errno);
		return (-1);
	}
	return (fd);
}


/*
 * Open the log device, and pull up all pending messages.
 */
void
sun_prepare_sys_poll()
{
	int nfds, funix;

	if ((funix = openklog(LogName, O_RDONLY)) < 0) {
		logerror("can't open kernel log device - fatal");
		exit(1);
	}

	Pfd.fd = funix;
	Pfd.events = POLLIN;

	for (;;) {
		nfds = poll(&Pfd, 1, 0);
		if (nfds <= 0) {
			if (sys_init_msg_count > 0)
				flushmsg(SYNC_FILE);
			break;
		}

		if (Pfd.revents & POLLIN) {
			getkmsg(0);
		} else if (Pfd.revents & (POLLNVAL|POLLHUP|POLLERR)) {
			logerror("kernel log driver poll error");
			break;
		}
	}

}

/*
 * this thread listens to the local stream log driver for log messages
 * generated by this host, formats them, and queues them to the logger
 * thread.
 */
/*ARGSUSED*/
void *
sun_sys_poll(void *ap)
{
	int nfds;
	static int klogerrs = 0;
	pthread_t mythreadno;

	if (Debug) {
		mythreadno = pthread_self();
	}

	DPRINT1(1, "sys_poll(%u): sys_thread started\n", mythreadno);

	/*
	 * Try to process as many messages as we can without blocking on poll.
	 * We count such "initial" messages with sys_init_msg_count and
	 * enqueue them without the SYNC_FILE flag.  When no more data is
	 * waiting on the local log device, we set timeout to INFTIM,
	 * clear sys_init_msg_count, and generate a flush message to sync
	 * the previously counted initial messages out to disk.
	 */

	sys_init_msg_count = 0;

	for (;;) {
		errno = 0;
		t_errno = 0;

		nfds = poll(&Pfd, 1, INFTIM);

		if (nfds == 0)
			continue;

		if (nfds < 0) {
			if (errno != EINTR)
				logerror("poll");
			continue;
		}
		if (Pfd.revents & POLLIN) {
			getkmsg(INFTIM);
		} else {
			if (shutting_down) {
				pthread_exit(0);
			}
			if (Pfd.revents & (POLLNVAL|POLLHUP|POLLERR)) {
				logerror("kernel log driver poll error");
				(void) close(Pfd.fd);
				Pfd.fd = -1;
			}
		}

		while (Pfd.fd == -1 && klogerrs++ < 10) {
			Pfd.fd = openklog(LogName, O_RDONLY);
		}
		if (klogerrs >= 10) {
			logerror("can't reopen kernel log device - fatal");
			exit(1);
		}
	}
	/*NOTREACHED*/
	return (NULL);
}

/*
 * Pull up one message from log driver.
 */
void
sun_getkmsg(int timeout)
{
	int flags = 0, i;
	char *lastline;
	struct strbuf ctl, dat;
	struct log_ctl hdr;
	char buf[MAXLINE+1];
	size_t buflen;
	size_t len;
	char tmpbuf[MAXLINE+1];
	pthread_t mythreadno;

	if (Debug) {
		mythreadno = pthread_self();
	}

	dat.maxlen = MAXLINE;
	dat.buf = buf;
	ctl.maxlen = sizeof (struct log_ctl);
	ctl.buf = (caddr_t)&hdr;

	while ((i = getmsg(Pfd.fd, &ctl, &dat, &flags)) == MOREDATA) {
		lastline = &dat.buf[dat.len];
		*lastline = '\0';

		DPRINT2(5, "sys_poll:(%u): getmsg: dat.len = %d\n",
		    mythreadno, dat.len);
		buflen = strlen(buf);
		len = findnl_bkwd(buf, buflen);

		(void) memcpy(tmpbuf, buf, len);
		tmpbuf[len] = '\0';

		/*
		 * Format sys will enqueue the log message.
		 * Set the sync flag if timeout != 0, which
		 * means that we're done handling all the
		 * initial messages ready during startup.
		 */
		if (timeout == 0) {
			formatsys(&hdr, tmpbuf, 0);
			sys_init_msg_count++;
		} else {
			formatsys(&hdr, tmpbuf, 1);
		}
		sys_msg_count++;

		if (len != buflen) {
			/* If anything remains in buf */
			size_t remlen;

			if (buf[len] == '\n') {
				/* skip newline */
				len++;
			}

			/*
			 *  Move the remaining bytes to
			 * the beginnning of buf.
			 */

			remlen = buflen - len;
			(void) memcpy(buf, &buf[len], remlen);
			dat.maxlen = MAXLINE - remlen;
			dat.buf = &buf[remlen];
		} else {
			dat.maxlen = MAXLINE;
			dat.buf = buf;
		}
	}

	if (i == 0 && dat.len > 0) {
		dat.buf[dat.len] = '\0';
		/*
		 * Format sys will enqueue the log message.
		 * Set the sync flag if timeout != 0, which
		 * means that we're done handling all the
		 * initial messages ready during startup.
		 */
		DPRINT2(5, "getkmsg(%u): getmsg: dat.maxlen = %d\n",
		    mythreadno, dat.maxlen);
		DPRINT2(5, "getkmsg(%u): getmsg: dat.len = %d\n",
		    mythreadno, dat.len);
		DPRINT2(5, "getkmsg(%u): getmsg: strlen(dat.buf) = %d\n",
		    mythreadno, strlen(dat.buf));
		DPRINT2(5, "getkmsg(%u): getmsg: dat.buf = \"%s\"\n",
		    mythreadno, dat.buf);
		DPRINT2(5, "getkmsg(%u): buf len = %d\n",
		    mythreadno, strlen(buf));
		if (timeout == 0) {
			formatsys(&hdr, buf, 0);
			sys_init_msg_count++;
		} else {
			formatsys(&hdr, buf, 1);
		}
		sys_msg_count++;
	} else if (i < 0 && errno != EINTR) {
		if (!shutting_down) {
			logerror("kernel log driver read error");
		}
		(void) close(Pfd.fd);
		Pfd.fd = -1;
	}
}
