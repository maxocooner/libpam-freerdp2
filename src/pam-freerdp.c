/*
 * Copyright © 2012 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Ted Gould <ted@canonical.com>
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <pwd.h>

#include <security/pam_modules.h>
#include <security/pam_modutil.h>
#include <security/pam_appl.h>

#define PAM_TYPE_DOMAIN  1234

/* Either grab a value or prompt for it */
static char *
get_item (pam_handle_t * pamh, int type)
{
	/* Check to see if we just have the value.  If we do, great
	   let's dup it some we're consitently allocating memory */
	if (type != PAM_TYPE_DOMAIN) {
		char * value = NULL;
		if (pam_get_item(pamh, type, (const void **)&value) == PAM_SUCCESS && value != NULL) {
			return strdup(value);
		}
	}
	/* Now we need to prompt */

	/* Build up the message we're prompting for */
	struct pam_message message;
	const struct pam_message * pmessage = &message;

	message.msg = NULL;
	message.msg_style = PAM_PROMPT_ECHO_ON;

	switch (type) {
	case PAM_USER:
		message.msg = "login:";
		break;
	case PAM_RUSER:
		message.msg = "remote login:";
		break;
	case PAM_RHOST:
		message.msg = "remote host:";
		break;
	case PAM_AUTHTOK:
		message.msg = "password:";
		message.msg_style = PAM_PROMPT_ECHO_OFF;
		break;
	case PAM_TYPE_DOMAIN:
		message.msg = "domain:";
		break;
	default:
		return NULL;
	}

	struct pam_conv * conv = NULL;
	if (pam_get_item(pamh, PAM_CONV, (const void **)&conv) != PAM_SUCCESS || conv == NULL || conv->conv == NULL) {
		return NULL;
	}

	struct pam_response * responses = NULL;
	if (conv->conv(1, &pmessage, &responses, conv->appdata_ptr) != PAM_SUCCESS) {
		return NULL;
	}

	char * retval = responses->resp;
	free(responses);

	if (type == PAM_RHOST) {
		char * subloc = strstr(retval, "://");
		if (subloc != NULL) {
			char * original = retval;
			char * newish = subloc + strlen("://");
			char * endslash = strstr(newish, "/");

			if (endslash != NULL) {
				endslash[0] = '\0';
			}

			retval = strdup(newish);
			free(original);
		}
	}

	return retval;
}

#define GET_ITEM(val, type) \
	if ((val = get_item(pamh, type)) == NULL) { \
		retval = PAM_AUTH_ERR; \
		goto done; \
	}

/* TODO: Make this a build thing */
#define XFREERDP "/usr/bin/xfreerdp"

/* Authenticate.  We need to make sure we have a user account, that
   there are remote accounts and then verify them with FreeRDP */
PAM_EXTERN int
pam_sm_authenticate (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	char * username = NULL;
	char * password = NULL;
	char * ruser = NULL;
	char * rhost = NULL;
	char * rdomain = NULL;
	int retval = PAM_IGNORE;

	/* Get all the values, or prompt for them, or return with
	   an auth error */
	GET_ITEM(username, PAM_USER);
	GET_ITEM(ruser,    PAM_RUSER);
	GET_ITEM(rhost,    PAM_RHOST);
	GET_ITEM(rdomain,  PAM_TYPE_DOMAIN);
	GET_ITEM(password, PAM_AUTHTOK);

	int stdinpipe[2];
	if (pipe(stdinpipe) != 0) {
		retval = PAM_SYSTEM_ERR;
		goto done;
	}

	/* At this point we should have the values, let's check the auth */
	pid_t pid;
	switch (pid = fork()) {
	case 0: { /* child */
		dup2(stdinpipe[0], 0);

		char * args[5];

		args[0] = AUTH_CHECK;
		args[1] = rhost;
		args[2] = ruser;
		args[3] = rdomain;
		args[4] = NULL;

		struct passwd * pwdent = getpwnam(username);
		if (pwdent == NULL) {
			_exit(EXIT_FAILURE);
		}

		if (setgid(pwdent->pw_gid) < 0 || setuid(pwdent->pw_uid) < 0 ||
				setegid(pwdent->pw_gid) < 0 || seteuid(pwdent->pw_uid) < 0) {
			_exit(EXIT_FAILURE);
		}

		setenv("HOME", pwdent->pw_dir, 1);

		execvp(args[0], args);
		_exit(EXIT_FAILURE);
		break;
	}
	case -1: { /* fork'n error! */
		retval = PAM_SYSTEM_ERR;
		break;
	}
	default: {
		int forkret = 0;
		int bytesout = 0;

		bytesout += write(stdinpipe[1], password, strlen(password));
		bytesout += write(stdinpipe[1], "\n", 1);

		close(stdinpipe[1]);

		if (waitpid(pid, &forkret, 0) < 0 || bytesout == 0) {
			retval = PAM_SYSTEM_ERR;
		} else if (forkret == 0) {
			retval = PAM_SUCCESS;
		} else {
			retval = PAM_AUTH_ERR;
		}
	}
	}

	/* Free Memory and return our status */
done:
	if (username != NULL) { free(username); }
	if (password != NULL) { free(password); }
	if (ruser != NULL)    { free(ruser); }
	if (rhost != NULL)    { free(rhost); }
	if (rdomain != NULL)  { free(rdomain); }

	return retval;
}

pid_t session_pid = 0;
/* Open Session.  Here we need to fork a little process so that we can
   give the credentials to the session itself so that it can startup the
   xfreerdp viewer for the login */
PAM_EXTERN int
pam_sm_open_session (pam_handle_t *pamh, int flags, int argc, const char ** argv)
{
	if (session_pid != 0) {
		kill(session_pid, SIGKILL);
		session_pid = 0;
	}

	char * username = NULL;
	char * password = NULL;
	char * ruser = NULL;
	char * rhost = NULL;
	char * rdomain = NULL;
	int retval = PAM_SUCCESS;

	/* Get all the values, or prompt for them, or return with
	   an auth error */
	GET_ITEM(username, PAM_USER);
	GET_ITEM(ruser,    PAM_RUSER);
	GET_ITEM(rhost,    PAM_RHOST);
	GET_ITEM(rdomain,  PAM_TYPE_DOMAIN);
	GET_ITEM(password, PAM_AUTHTOK);

	struct passwd * pwdent = getpwnam(username);
	if (pwdent == NULL) {
		retval = PAM_SYSTEM_ERR;
		goto done;
	}
	
	/* Make our socket and bind it */
	int socketfd;
	struct sockaddr_un socket_addr;

	socketfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (socketfd < 0) {
		retval = PAM_SYSTEM_ERR;
		goto done;
	}

	memset(&socket_addr, 0, sizeof(struct sockaddr_un));
	socket_addr.sun_family = AF_UNIX;
	strncpy(socket_addr.sun_path, pwdent->pw_dir, sizeof(socket_addr.sun_path) - 1);
	strncpy(socket_addr.sun_path + strlen(pwdent->pw_dir), "/.freerdp-socket", sizeof(socket_addr.sun_path) - 1);

	/* We bind the socket before forking so that we ensure that
	   there isn't a race condition to get to it.  Things will block
	   otherwise. */
	if (bind(socketfd, (struct sockaddr *)&socket_addr, sizeof(struct sockaddr_un)) < 0) {
		close(socketfd);
		retval = PAM_SYSTEM_ERR;
		goto done;
	}

	/* Set the socket file permissions to be 600 and the user and group
	   to be the guest user.  NOTE: This won't protect on BSD */
	if (chmod(socket_addr.sun_path, S_IRUSR | S_IWUSR) != 0 ||
			chown(socket_addr.sun_path, pwdent->pw_uid, pwdent->pw_gid) != 0) {
		close(socketfd);
		retval = PAM_SYSTEM_ERR;
		goto done;
	}

	/* Build this up as a buffer so we can just write it and see that
	   very, very clearly */
	int buffer_len = 0;
	buffer_len += strlen(ruser) + 1;    /* Add one for the space */
	buffer_len += strlen(rhost) + 1;    /* Add one for the space */
	buffer_len += strlen(rdomain) + 1;  /* Add one for the space */
	buffer_len += strlen(password) + 1; /* Add one for the NULL */

	char * buffer = malloc(buffer_len);
	snprintf(buffer, buffer_len, "%s %s %s %s", ruser, password, rdomain, rhost);

	pid_t pid = fork();
	if (pid == 0) {
		if (setgid(pwdent->pw_gid) < 0 || setuid(pwdent->pw_uid) < 0 ||
				setegid(pwdent->pw_gid) < 0 || seteuid(pwdent->pw_uid) < 0) {
			_exit(EXIT_FAILURE);
		}

		if (listen(socketfd, 1) < 0) {
			_exit(EXIT_FAILURE);
		}

		socklen_t connected_addr_size;
		int connectfd;
		struct sockaddr_un connected_addr;

		connected_addr_size = sizeof(struct sockaddr_un);
		connectfd = accept(socketfd, (struct sockaddr *)&connected_addr, &connected_addr_size);
		if (connectfd < 0) {
			_exit(EXIT_FAILURE);
		}

		int writedata;
		writedata = write(connectfd, buffer, buffer_len);

		close(connectfd);
		close(socketfd);
		free(buffer);

		if (writedata == buffer_len) {
			_exit(0);
		} else {
			_exit(EXIT_FAILURE);
		}
	} else if (pid < 0) {
		retval = PAM_SYSTEM_ERR;
		close(socketfd);
		free(buffer);
	} else {
		session_pid = pid;
	}

done:
	if (username != NULL) { free(username); }
	if (password != NULL) { free(password); }
	if (ruser != NULL)    { free(ruser); }
	if (rhost != NULL)    { free(rhost); }
	if (rdomain != NULL)  { free(rdomain); }

    return retval;
}

/* Close Session.  Make sure our little guy has died so he doesn't become
   a zombie and eat things. */
PAM_EXTERN int
pam_sm_close_session (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	if (session_pid != 0) {
		kill(session_pid, SIGKILL);
		session_pid = 0;
	}

	return PAM_IGNORE;
}

/* LightDM likes to have this function around, but we don't need it as we
   don't have a token hanging around. */
PAM_EXTERN int
pam_sm_setcred (pam_handle_t *pamh, int flags, int argc, const char ** argv)
{
	return PAM_SUCCESS;
}

#ifdef PAM_STATIC

struct pam_module _pam_freerdp_modstruct = {
     "pam_freerdp",
     pam_sm_authenticate,
     pam_sm_setcred,
     NULL,
     pam_sm_open_session,
     pam_sm_close_session,
     NULL,
};

#endif
