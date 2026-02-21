/*
 *  Copyright (C) 2026 Graham Whaley.  All Rights Reserved.
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
 * sshvnc.c - SSH to remote host, start x11vnc, create tunnel and connect.
 *
 * Usage: vncviewer -sshvnc user@host [-sshvnc-display :N] [-sshvnc-persist]
 */

#include <vncviewer.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

Bool sshvncSpecified = False;

static char sshvncUserHost[256];
static char sshvncDisplay[64];
static Bool sshvncPersist = False;
static pid_t sshvncTunnelPid = 0;
static pid_t sshvncRemotePid = 0;

/* Fake "localhost::port" argument for argv rewriting */
static char sshvncLastArgv[32];


Bool
setupSshVnc(int *pargc, char **argv, int argIndex)
{
  char *userHost;
  int localPort, remotePort = 0;
  char cmd[1024];
  char line[1024];
  char portSpec[64];
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

  /* SSH to remote and start x11vnc with -bg.
     -bg makes x11vnc print PORT=XXXX then background itself (fork).
     We append pgrep to get the real forked server PID in one SSH session. */
  if (sshvncDisplay[0]) {
    snprintf(cmd, sizeof(cmd),
             "ssh %s 'x11vnc -nopw -bg -forever -auth guess -display %s 2>&1;"
             " pgrep -n x11vnc'",
             sshvncUserHost, sshvncDisplay);
  } else {
    snprintf(cmd, sizeof(cmd),
             "ssh %s 'x11vnc -nopw -bg -forever -auth guess 2>&1;"
             " pgrep -n x11vnc'",
             sshvncUserHost);
  }

  fprintf(stderr, "%s: Starting x11vnc on %s...\n",
          programName, sshvncUserHost);

  fp = popen(cmd, "r");
  if (!fp) {
    perror("popen");
    return False;
  }

  /* Parse x11vnc output for PORT=.
     The last line will be the real PID from pgrep. */
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

  fprintf(stderr, "%s: x11vnc running on port %d (pid %d)\n",
          programName, remotePort, (int)sshvncRemotePid);

  /* Find a free local port for the tunnel */
  localPort = FindFreeTcpPort();
  if (localPort == 0) {
    fprintf(stderr, "%s: Could not find a free local port\n", programName);
    return False;
  }

  /* Create SSH tunnel via fork/exec so we can track the PID */
  snprintf(portSpec, sizeof(portSpec),
           "%d:localhost:%d", localPort, remotePort);

  sshvncTunnelPid = fork();
  if (sshvncTunnelPid == -1) {
    perror("fork");
    return False;
  }

  if (sshvncTunnelPid == 0) {
    /* Child - redirect stdio to /dev/null and exec ssh tunnel */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 1);
      dup2(devnull, 2);
      if (devnull > 2)
        close(devnull);
    }
    execlp("ssh", "ssh", "-N", "-L", portSpec, sshvncUserHost, (char *)NULL);
    _exit(1);
  }

  /* Parent - give tunnel time to establish */
  fprintf(stderr, "%s: SSH tunnel localhost:%d -> %s:localhost:%d (pid %d)\n",
          programName, localPort, sshvncUserHost, remotePort,
          (int)sshvncTunnelPid);
  sleep(1);

  /* Append localhost::localPort as the server argument.
     After removing -sshvnc args there is no server spec in argv,
     so we add one (safe because we removed at least 2 args earlier). */
  snprintf(sshvncLastArgv, sizeof(sshvncLastArgv),
           "localhost::%d", localPort);
  argv[*pargc] = sshvncLastArgv;
  (*pargc)++;
  argv[*pargc] = NULL;

  sshvncSpecified = True;
  tunnelSpecified = True;

  atexit(cleanupSshVnc);

  return True;
}


void
cleanupSshVnc(void)
{
  if (!sshvncSpecified)
    return;

  /* Kill local SSH tunnel */
  if (sshvncTunnelPid > 0) {
    fprintf(stderr, "%s: Killing SSH tunnel (pid %d)\n",
            programName, (int)sshvncTunnelPid);
    kill(sshvncTunnelPid, SIGTERM);
    waitpid(sshvncTunnelPid, NULL, WNOHANG);
    sshvncTunnelPid = 0;
  }

  /* Kill remote x11vnc unless persist mode */
  if (!sshvncPersist && sshvncRemotePid > 0) {
    char cmd[512];
    fprintf(stderr, "%s: Killing remote x11vnc (pid %d on %s)\n",
            programName, (int)sshvncRemotePid, sshvncUserHost);
    snprintf(cmd, sizeof(cmd), "ssh %s 'kill %d' 2>/dev/null",
             sshvncUserHost, (int)sshvncRemotePid);
    if (system(cmd) != 0)
      fprintf(stderr, "%s: Warning: failed to kill remote x11vnc\n",
              programName);
    sshvncRemotePid = 0;
  }
}
