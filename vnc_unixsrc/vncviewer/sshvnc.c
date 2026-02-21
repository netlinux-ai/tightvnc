/*
 *  Copyright (C) 2026 Graham North.  All Rights Reserved.
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
 * sshvnc.c - SSH to remote host, start x11vnc, connect directly.
 *
 * Usage: vncviewer -sshvnc user@host [-sshvnc-display :N] [-sshvnc-persist]
 *
 * Flow:
 *   1. SSH to remote, auto-discover X display, start x11vnc with -bg
 *   2. Parse PORT= and PID from x11vnc output
 *   3. Connect viewer directly to host::port (no VNC password needed)
 *   4. On exit, SSH back to kill remote x11vnc (unless -sshvnc-persist)
 */

#include <vncviewer.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

Bool sshvncSpecified = False;

static char sshvncUserHost[256];
static char sshvncHost[256];
static char sshvncDisplay[64];
static Bool sshvncPersist = False;
static pid_t sshvncRemotePid = 0;

/* Server argument for argv rewriting: "host::port" */
static char sshvncServerArg[300];


Bool
setupSshVnc(int *pargc, char **argv, int argIndex)
{
  char *userHost;
  char *atSign;
  int remotePort = 0;
  char cmd[1024];
  char line[1024];
  FILE *fp;
  int i;

  if (argIndex + 1 >= *pargc) {
    fprintf(stderr, "%s: -sshvnc requires user@host argument\n", programName);
    return False;
  }

  userHost = argv[argIndex + 1];
  if (strlen(userHost) >= sizeof(sshvncUserHost)) {
    fprintf(stderr, "%s: -sshvnc user@host too long\n", programName);
    return False;
  }
  strcpy(sshvncUserHost, userHost);

  /* Extract just the hostname (after @ if present) */
  atSign = strchr(sshvncUserHost, '@');
  if (atSign)
    strncpy(sshvncHost, atSign + 1, sizeof(sshvncHost) - 1);
  else
    strncpy(sshvncHost, sshvncUserHost, sizeof(sshvncHost) - 1);
  sshvncHost[sizeof(sshvncHost) - 1] = '\0';

  /* Remove -sshvnc and user@host from argv */
  removeArgs(pargc, argv, argIndex, 2);

  /* Scan remaining args for -sshvnc-display and -sshvnc-persist */
  sshvncDisplay[0] = '\0';
  for (i = argIndex; i < *pargc; ) {
    if (strcmp(argv[i], "-sshvnc-display") == 0) {
      if (i + 1 >= *pargc) {
        fprintf(stderr, "%s: -sshvnc-display requires argument\n",
                programName);
        return False;
      }
      strncpy(sshvncDisplay, argv[i + 1], sizeof(sshvncDisplay) - 1);
      sshvncDisplay[sizeof(sshvncDisplay) - 1] = '\0';
      removeArgs(pargc, argv, i, 2);
    } else if (strcmp(argv[i], "-sshvnc-persist") == 0) {
      sshvncPersist = True;
      removeArgs(pargc, argv, i, 1);
    } else {
      i++;
    }
  }

  /* SSH to remote and start x11vnc with -bg -nopw.
     -bg makes x11vnc print PORT=XXXX then fork to background.
     -nopw disables VNC password (trust is via SSH authentication).
     We append pgrep to get the forked server PID in one SSH session.
     When no display is specified, auto-discover from /tmp/.X11-unix/. */
  if (sshvncDisplay[0]) {
    snprintf(cmd, sizeof(cmd),
             "ssh %s '"
             "x11vnc -bg -nopw -display %s 2>&1;"
             " pgrep -n x11vnc'",
             sshvncUserHost, sshvncDisplay);
  } else {
    snprintf(cmd, sizeof(cmd),
             "ssh %s '"
             "D=$(ls /tmp/.X11-unix/ 2>/dev/null | sed s/X// | head -1);"
             " x11vnc -bg -nopw -display :${D:-0} 2>&1;"
             " pgrep -n x11vnc'",
             sshvncUserHost);
  }

  fprintf(stderr, "%s: Starting x11vnc on %s...\n",
          programName, sshvncHost);

  fp = popen(cmd, "r");
  if (!fp) {
    perror("popen");
    return False;
  }

  /* Parse x11vnc output for PORT=.
     The last numeric-only line is the pgrep PID output. */
  while (fgets(line, sizeof(line), fp)) {
    char *p;

    p = strstr(line, "PORT=");
    if (p)
      remotePort = atoi(p + 5);

    /* Last numeric-only line is the pgrep output */
    if (line[0] >= '0' && line[0] <= '9' &&
        strspn(line, "0123456789\n") == strlen(line))
      sshvncRemotePid = atoi(line);
  }

  pclose(fp);

  if (remotePort == 0) {
    fprintf(stderr, "%s: Failed to get VNC port from x11vnc\n", programName);
    return False;
  }

  fprintf(stderr, "%s: x11vnc on %s port %d (pid %d) — connecting directly\n",
          programName, sshvncHost, remotePort, (int)sshvncRemotePid);

  /* Connect directly to remote host::port (no tunnel).
     Append "host::port" as the server argument. */
  snprintf(sshvncServerArg, sizeof(sshvncServerArg),
           "%s::%d", sshvncHost, remotePort);
  argv[*pargc] = sshvncServerArg;
  (*pargc)++;
  argv[*pargc] = NULL;

  sshvncSpecified = True;

  atexit(cleanupSshVnc);

  return True;
}


/*
 * Show a small centered window with a shutdown message.
 * Returns the window (caller should XDestroyWindow when done).
 * Returns None if the display is unavailable.
 */
static Window
showShutdownNotice(const char *msg)
{
  Window win;
  XSetWindowAttributes attr;
  XGCValues gcv;
  GC gc;
  int screen;
  int textW, textH, winW, winH, x, y;
  XFontStruct *font;

  if (!dpy)
    return None;

  screen = DefaultScreen(dpy);
  font = XLoadQueryFont(dpy, "-*-helvetica-bold-r-*-*-18-*-*-*-*-*-*-*");
  if (!font)
    font = XLoadQueryFont(dpy, "fixed");
  if (!font)
    return None;

  textW = XTextWidth(font, msg, strlen(msg));
  textH = font->ascent + font->descent;
  winW = textW + 40;
  winH = textH + 30;
  x = (DisplayWidth(dpy, screen) - winW) / 2;
  y = (DisplayHeight(dpy, screen) - winH) / 2;

  attr.override_redirect = True;
  attr.background_pixel = BlackPixel(dpy, screen);
  attr.border_pixel = WhitePixel(dpy, screen);

  win = XCreateWindow(dpy, RootWindow(dpy, screen),
                      x, y, winW, winH, 2,
                      CopyFromParent, InputOutput, CopyFromParent,
                      CWOverrideRedirect | CWBackPixel | CWBorderPixel,
                      &attr);

  XMapRaised(dpy, win);

  gcv.foreground = WhitePixel(dpy, screen);
  gcv.font = font->fid;
  gc = XCreateGC(dpy, win, GCForeground | GCFont, &gcv);
  XDrawString(dpy, win, gc, 20, 15 + font->ascent, msg, strlen(msg));
  XFreeGC(dpy, gc);
  XFreeFontInfo(NULL, font, 0);
  XFlush(dpy);

  return win;
}


void
cleanupSshVnc(void)
{
  Window notice = None;
  char msg[300];

  if (!sshvncSpecified)
    return;
  sshvncSpecified = False;

  /* Show on-screen notice while cleanup runs */
  if (!sshvncPersist && sshvncRemotePid > 0) {
    snprintf(msg, sizeof(msg), "Disconnecting from %s...", sshvncHost);
    notice = showShutdownNotice(msg);
  }

  /* Kill remote x11vnc unless persist mode */
  if (!sshvncPersist && sshvncRemotePid > 0) {
    char cmd[512];
    fprintf(stderr, "%s: Stopping remote x11vnc (pid %d on %s)...\n",
            programName, (int)sshvncRemotePid, sshvncHost);
    snprintf(cmd, sizeof(cmd), "ssh %s 'kill %d' 2>/dev/null",
             sshvncUserHost, (int)sshvncRemotePid);
    if (system(cmd) != 0)
      fprintf(stderr, "%s: Warning: failed to kill remote x11vnc\n",
              programName);
    sshvncRemotePid = 0;
  }

  if (notice != None && dpy) {
    XDestroyWindow(dpy, notice);
    XFlush(dpy);
  }
}
