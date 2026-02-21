/*
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

/*
 * vncviewer.c - the Xt-based VNC viewer.
 */

#include "vncviewer.h"
#include <stdarg.h>
#include <time.h>

char *programName;
XtAppContext appContext;
Display* dpy;

Widget toplevel;

FILE *dbglog = NULL;

jmp_buf xtErrorJmpBuf;
volatile int xtErrorJmpBufSet = 0;

#define DBG_LOG_PATH "/tmp/vncviewer_debug.log"

void
dbg_init(void)
{
  dbglog = fopen(DBG_LOG_PATH, "w");
  if (!dbglog) {
    fprintf(stderr, "WARNING: could not open %s for writing\n", DBG_LOG_PATH);
    dbglog = stderr;
  }
  setbuf(dbglog, NULL); /* unbuffered so nothing is lost on crash */
  dbg_printf("=== vncviewer debug log started ===");
}

void
dbg_printf(const char *fmt, ...)
{
  va_list ap;
  struct timespec ts;
  struct tm tm;

  if (!dbglog) return;

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  fprintf(dbglog, "[%02d:%02d:%02d.%03ld] ",
          tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
  va_start(ap, fmt);
  vfprintf(dbglog, fmt, ap);
  va_end(ap);
  fprintf(dbglog, "\n");
}

int
main(int argc, char **argv)
{
  int i;
  Bool firstConnectionDone = False;
  programName = argv[0];
  dbg_init();

  /* The -listen option is used to make us a daemon process which listens for
     incoming connections from servers, rather than actively connecting to a
     given server. The -tunnel and -via options are useful to create
     connections tunneled via SSH port forwarding. We must test for the
     -listen option before invoking any Xt functions - this is because we use
     forking, and Xt doesn't seem to cope with forking very well. For -listen
     option, when a successful incoming connection has been accepted,
     listenForIncomingConnections() returns, setting the listenSpecified
     flag. */

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-listen") == 0) {
      listenForIncomingConnections(&argc, argv, i);
      break;
    }
    if (strcmp(argv[i], "-sshvnc") == 0) {
      if (!setupSshVnc(&argc, argv, i))
	exit(1);
      break;
    }
    if (strcmp(argv[i], "-tunnel") == 0 || strcmp(argv[i], "-via") == 0) {
      if (!createTunnel(&argc, argv, i))
	exit(1);
      break;
    }
  }

  /* Call the main Xt initialisation function.  It parses command-line options,
     generating appropriate resource specs, and makes a connection to the X
     display. */

  toplevel = XtVaAppInitialize(&appContext, "Vncviewer",
			       cmdLineOptions, numCmdLineOptions,
			       &argc, argv, fallback_resources,
			       XtNborderWidth, 0, NULL);

  dpy = XtDisplay(toplevel);

  /* Interpret resource specs and process any remaining command-line arguments
     (i.e. the VNC server name).  If the server name isn't specified on the
     command line, getArgsAndResources() will pop up a dialog box and wait
     for one to be entered. */

  GetArgsAndResources(argc, argv);

  for (;;) {
    /* Connect (or reconnect) to the VNC server */

    if (!listenSpecified) {
      if (!ConnectToRFBServer(vncServerHost, vncServerPort)) {
        dbg_printf("Connection failed, retrying in 3 seconds...");
        fprintf(stderr, "Connection failed, retrying in 3 seconds...\n");
        sleep(3);
        /* If using -sshvnc, check remote x11vnc and restart if needed */
        if (sshvncSpecified && !reconnectSshVnc()) {
          fprintf(stderr, "Failed to restart remote x11vnc, retrying...\n");
        }
        continue;
      }
    }

    /* Initialise the VNC connection, including reading the password */

    if (!InitialiseRFBConnection()) {
      dbg_printf("RFB init failed, retrying in 3 seconds...");
      fprintf(stderr, "RFB init failed, retrying in 3 seconds...\n");
      close(rfbsock);
      sleep(3);
      continue;
    }

    /* Only create widgets on first connection */
    if (!firstConnectionDone) {
      CreatePopup();
      SetVisualAndCmap();
      ToplevelInitBeforeRealization();
      DesktopInitBeforeRealization();
      XtRealizeWidget(toplevel);
      InitialiseSelection();
      ToplevelInitAfterRealization();
      DesktopInitAfterRealization();
      firstConnectionDone = True;
    }

    /* Tell the VNC server which pixel format and encodings we want to use */

    SetFormatAndEncodings();

    /* Main loop - process VNC messages. X events are processed whenever
       the VNC connection is idle. */

    dbg_printf("Entering main loop");

    while (1) {
      if (!HandleRFBServerMessage()) {
        dbg_printf("HandleRFBServerMessage returned False - connection lost");
        break;
      }
    }

    close(rfbsock);

    if (listenSpecified)
      break;

    dbg_printf("Reconnecting in 3 seconds...");
    fprintf(stderr, "Connection lost, reconnecting in 3 seconds...\n");
    sleep(3);

    /* If using -sshvnc, check remote x11vnc and restart if needed */
    if (sshvncSpecified && !reconnectSshVnc()) {
      fprintf(stderr, "Failed to restart remote x11vnc, retrying...\n");
      continue;
    }
  }

  dbg_printf("Exiting, cleaning up");
  Cleanup();
  cleanupSshVnc();

  return 0;
}
