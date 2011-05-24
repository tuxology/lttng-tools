/*
 * Copyright (C) 2011 - Julien Desfossez <julien.desfossez@polymtl.ca>
 *                      Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <urcu/list.h>
#include <poll.h>
#include <unistd.h>

#include "lttngerr.h"
#include "libkernelctl.h"
#include "liblttsessiondcomm.h"
#include "kconsumerd.h"

/* Init the list of FDs */
static struct ltt_kconsumerd_fd_list kconsumerd_fd_list = {
	.head = CDS_LIST_HEAD_INIT(kconsumerd_fd_list.head),
};

/* Number of element for the list below. */
static unsigned int fds_count;

/* If the local array of FDs needs update in the poll function */
static unsigned int update_fd_array = 1;

/* lock the fd array and structures */
static pthread_mutex_t kconsumerd_lock_fds;

/* the two threads (receive fd and poll) */
static pthread_t threads[2];

/* communication with splice */
static int thread_pipe[2];

/* socket to communicate errors with sessiond */
static int error_socket = -1;

/* Argument variables */
int opt_quiet;
int opt_verbose;
static int opt_daemon;
static const char *progname;
static char command_sock_path[PATH_MAX]; /* Global command socket path */
static char error_sock_path[PATH_MAX]; /* Global error path */

/*
 * del_fd
 *
 * Remove a fd from the global list protected by a mutex
 */
static void del_fd(struct ltt_kconsumerd_fd *lcf)
{
	DBG("Removing %d", lcf->consumerd_fd);
	pthread_mutex_lock(&kconsumerd_lock_fds);
	cds_list_del(&lcf->list);
	if (fds_count > 0) {
		fds_count--;
		DBG("Removed ltt_kconsumerd_fd");
		if (lcf != NULL) {
			close(lcf->out_fd);
			close(lcf->consumerd_fd);
			free(lcf);
			lcf = NULL;
		}
	}
	pthread_mutex_unlock(&kconsumerd_lock_fds);
}

/*
 *  cleanup
 *
 *  Cleanup the daemon's socket on exit
 */
static void cleanup()
{
	struct ltt_kconsumerd_fd *iter;

	/* remove the socket file */
	unlink(command_sock_path);

	/* unblock the threads */
	WARN("Terminating the threads before exiting");
	pthread_cancel(threads[0]);
	pthread_cancel(threads[1]);

	/* close all outfd */
	cds_list_for_each_entry(iter, &kconsumerd_fd_list.head, list) {
		del_fd(iter);
	}
}

/*
 * send_error
 *
 * send return code to ltt-sessiond
 */
static int send_error(enum lttcomm_return_code cmd)
{
	if (error_socket > 0) {
		return lttcomm_send_unix_sock(error_socket, &cmd,
				sizeof(enum lttcomm_sessiond_command));
	} else {
		return 0;
	}
}

/*
 * add_fd
 *
 * Add a fd to the global list protected by a mutex
 */
static int add_fd(struct lttcomm_kconsumerd_msg *buf, int consumerd_fd)
{
	struct ltt_kconsumerd_fd *tmp_fd;
	int ret;

	tmp_fd = malloc(sizeof(struct ltt_kconsumerd_fd));
	tmp_fd->sessiond_fd = buf->fd;
	tmp_fd->consumerd_fd = consumerd_fd;
	tmp_fd->state = buf->state;
	tmp_fd->max_sb_size = buf->max_sb_size;
	strncpy(tmp_fd->path_name, buf->path_name, PATH_MAX);

	/* Opening the tracefile in write mode */
	DBG("Opening %s for writing", tmp_fd->path_name);
	ret = open(tmp_fd->path_name,
			O_WRONLY|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO);
	if (ret < 0) {
		ERR("Opening %s", tmp_fd->path_name);
		perror("open");
		goto end;
	}
	tmp_fd->out_fd = ret;
	tmp_fd->out_fd_offset = 0;

	DBG("Adding %s (%d, %d, %d)", tmp_fd->path_name,
			tmp_fd->sessiond_fd, tmp_fd->consumerd_fd, tmp_fd->out_fd);

	pthread_mutex_lock(&kconsumerd_lock_fds);
	cds_list_add(&tmp_fd->list, &kconsumerd_fd_list.head);
	fds_count++;
	pthread_mutex_unlock(&kconsumerd_lock_fds);

end:
	return ret;
}


/*
 *  sighandler
 *
 *  Signal handler for the daemon
 */
static void sighandler(int sig)
{
	cleanup();

	return;
}

/*
 *  set_signal_handler
 *
 *  Setup signal handler for :
 *      SIGINT, SIGTERM, SIGPIPE
 */
static int set_signal_handler(void)
{
	int ret = 0;
	struct sigaction sa;
	sigset_t sigset;

	if ((ret = sigemptyset(&sigset)) < 0) {
		perror("sigemptyset");
		return ret;
	}

	sa.sa_handler = sighandler;
	sa.sa_mask = sigset;
	sa.sa_flags = 0;
	if ((ret = sigaction(SIGTERM, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}

	if ((ret = sigaction(SIGINT, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}

	if ((ret = sigaction(SIGPIPE, &sa, NULL)) < 0) {
		perror("sigaction");
		return ret;
	}

	return ret;
}

/*
 * on_read_subbuffer
 *
 * Splice the data from the ring buffer to the tracefile.
 * Returns the number of bytes spliced
 */
static int on_read_subbuffer(struct ltt_kconsumerd_fd *kconsumerd_fd,
		unsigned long len)
{
	long ret = 0;
	loff_t offset = 0;
	off_t orig_offset = kconsumerd_fd->out_fd_offset;
	int fd = kconsumerd_fd->consumerd_fd;
	int outfd = kconsumerd_fd->out_fd;

	while (len > 0) {
		DBG("splice chan to pipe offset %lu (fd : %d)",
				(unsigned long)offset, fd);
		ret = splice(fd, &offset, thread_pipe[1], NULL, len,
				SPLICE_F_MOVE | SPLICE_F_MORE);
		DBG("splice chan to pipe ret %ld", ret);
		if (ret < 0) {
			ret = errno;
			perror("Error in relay splice");
			goto splice_error;
		}

		ret = splice(thread_pipe[0], NULL, outfd, NULL, ret,
				SPLICE_F_MOVE | SPLICE_F_MORE);
		DBG("splice pipe to file %ld", ret);
		if (ret < 0) {
			ret = errno;
			perror("Error in file splice");
			goto splice_error;
		}
		if (ret >= len) {
			len = 0;
		}
		/* This won't block, but will start writeout asynchronously */
		sync_file_range(outfd, kconsumerd_fd->out_fd_offset, ret,
				SYNC_FILE_RANGE_WRITE);
		kconsumerd_fd->out_fd_offset += ret;
	}

	/*
	 * This does a blocking write-and-wait on any page that belongs to the
	 * subbuffer prior to the one we just wrote.
	 * Don't care about error values, as these are just hints and ways to
	 * limit the amount of page cache used.
	 */
	if (orig_offset >= kconsumerd_fd->max_sb_size) {
		sync_file_range(outfd, orig_offset - kconsumerd_fd->max_sb_size,
				kconsumerd_fd->max_sb_size,
				SYNC_FILE_RANGE_WAIT_BEFORE
				| SYNC_FILE_RANGE_WRITE
				| SYNC_FILE_RANGE_WAIT_AFTER);
		/*
		 * Give hints to the kernel about how we access the file:
		 * POSIX_FADV_DONTNEED : we won't re-access data in a near
		 * future after we write it.
		 * We need to call fadvise again after the file grows because
		 * the kernel does not seem to apply fadvise to non-existing
		 * parts of the file.
		 * Call fadvise _after_ having waited for the page writeback to
		 * complete because the dirty page writeback semantic is not
		 * well defined. So it can be expected to lead to lower
		 * throughput in streaming.
		 */
		posix_fadvise(outfd, orig_offset - kconsumerd_fd->max_sb_size,
				kconsumerd_fd->max_sb_size, POSIX_FADV_DONTNEED);
	}
	goto end;

splice_error:
	/* send the appropriate error description to sessiond */
	switch(ret) {
		case EBADF:
			send_error(KCONSUMERD_SPLICE_EBADF);
			break;
		case EINVAL:
			send_error(KCONSUMERD_SPLICE_EINVAL);
			break;
		case ENOMEM:
			send_error(KCONSUMERD_SPLICE_ENOMEM);
			break;
		case ESPIPE:
			send_error(KCONSUMERD_SPLICE_ESPIPE);
			break;
	}

end:
	return ret;
}

/*
 * read_subbuffer
 *
 * Consume data on a file descriptor and write it on a trace file
 */
static int read_subbuffer(struct ltt_kconsumerd_fd *kconsumerd_fd)
{
	unsigned long len;
	int err;
	long ret = 0;
	int infd = kconsumerd_fd->consumerd_fd;

	DBG("In read_subbuffer (infd : %d)", infd);
	/* Get the next subbuffer */
	err = kernctl_get_next_subbuf(infd);
	if (err != 0) {
		ret = errno;
		perror("Reserving sub buffer failed (everything is normal, "
				"it is due to concurrency)");
		goto end;
	}

	/* read the whole subbuffer */
	err = kernctl_get_padded_subbuf_size(infd, &len);
	if (err != 0) {
		ret = errno;
		perror("Getting sub-buffer len failed.");
		goto end;
	}

	/* splice the subbuffer to the tracefile */
	ret = on_read_subbuffer(kconsumerd_fd, len);
	if (ret < 0) {
		/*
		 * display the error but continue processing to try
		 * to release the subbuffer
		 */
		ERR("Error splicing to tracefile");
	}

	err = kernctl_put_next_subbuf(infd);
	if (err != 0) {
		ret = errno;
		if (errno == EFAULT) {
			perror("Error in unreserving sub buffer\n");
		} else if (errno == EIO) {
			/* Should never happen with newer LTTng versions */
			perror("Reader has been pushed by the writer, last sub-buffer corrupted.");
		}
		goto end;
	}

end:
	return ret;
}

/*
 * change_fd_state
 *
 * Update a fd according to what we just received
 */
static void change_fd_state(int sessiond_fd,
		enum lttcomm_kconsumerd_fd_state state)
{
	struct ltt_kconsumerd_fd *iter;
	cds_list_for_each_entry(iter, &kconsumerd_fd_list.head, list) {
		if (iter->sessiond_fd == sessiond_fd) {
			iter->state = state;
			break;
		}
	}
}

/*
 * consumerd_recv_fd
 *
 * Receives an array of file descriptors and the associated
 * structures describing each fd (path name).
 * Returns the size of received data
 */
static int consumerd_recv_fd(int sfd, int size,
		enum lttcomm_consumerd_command cmd_type)
{
	struct msghdr msg;
	struct iovec iov[1];
	int ret, i;
	struct cmsghdr *cmsg;
	int nb_fd;
	char tmp[CMSG_SPACE(size)];
	struct lttcomm_kconsumerd_msg *buf;
	/* the number of fds we are about to receive */
	nb_fd = size/sizeof(struct lttcomm_kconsumerd_msg);

	buf = malloc(size);

	memset(&msg, 0, sizeof(msg));

	/* Prepare to receive the structures */
	iov[0].iov_base = buf;
	iov[0].iov_len = size;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	msg.msg_control = tmp;
	msg.msg_controllen = sizeof(tmp);

	DBG("Waiting to receive fds");
	if ((ret = recvmsg(sfd, &msg, 0)) < 0) {
		perror("recvmsg");
	}
	if (ret != size) {
		ERR("Received only %d, expected %d", ret, size);
		send_error(KCONSUMERD_ERROR_RECV_FD);
		goto end;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg) {
		ERR("Invalid control message header");
		ret = -1;
		send_error(KCONSUMERD_ERROR_RECV_FD);
		goto end;
	}

	/* if we received fds */
	if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
		DBG("Receive : expecting %d fds", nb_fd);
		for (i = 0; i < nb_fd; i++) {
			switch (cmd_type) {
				case LTTCOMM_ADD_STREAM:
					DBG("add_fd %s (%d)", buf[i].path_name, ((int *)CMSG_DATA(cmsg))[i]);
					ret = add_fd(&buf[i], ((int *)CMSG_DATA(cmsg))[i]);
					if (ret < 0) {
						send_error(KCONSUMERD_OUTFD_ERROR);
						goto end;
					}
					break;
				case LTTCOMM_UPDATE_STREAM:
					change_fd_state(buf[i].fd, buf[i].state);
					break;
				default:
					break;
			}
		}
		/* flag to tell the polling thread to update its fd array */
		update_fd_array = 1;
		send_error(KCONSUMERD_SUCCESS_RECV_FD);
	} else {
		ERR("Didn't received any fd");
		send_error(KCONSUMERD_ERROR_RECV_FD);
		ret = -1;
		goto end;
	}

end:
	if (buf != NULL) {
		free(buf);
		buf = NULL;
	}
	return ret;
}

/*
 *  thread_receive_fds
 *
 *  This thread listens on the consumerd socket and
 *  receives the file descriptors from ltt-sessiond
 */
static void *thread_receive_fds(void *data)
{
	int sock, client_socket, ret;
	struct lttcomm_kconsumerd_header tmp;

	DBG("Creating command socket %s", command_sock_path);
	unlink(command_sock_path);
	client_socket = lttcomm_create_unix_sock(command_sock_path);
	if (client_socket < 0) {
		ERR("Cannot create command socket");
		goto error;
	}

	ret = lttcomm_listen_unix_sock(client_socket);
	if (ret < 0) {
		goto error;
	}

	DBG("Sending ready command to ltt-sessiond");
	ret = send_error(KCONSUMERD_COMMAND_SOCK_READY);
	if (ret < 0) {
		ERR("Error sending ready command to ltt-sessiond");
		goto error;
	}

	/* Blocking call, waiting for transmission */
	sock = lttcomm_accept_unix_sock(client_socket);
	if (sock <= 0) {
		WARN("On accept, retrying");
		goto error;
	}
	while (1) {
		/* We first get the number of fd we are about to receive */
		ret = lttcomm_recv_unix_sock(sock, &tmp,
				sizeof(struct lttcomm_kconsumerd_header));
		if (ret <= 0) {
			ERR("Receiving the lttcomm_kconsumerd_header, exiting");
			goto error;
		}
		ret = consumerd_recv_fd(sock, tmp.payload_size, tmp.cmd_type);
		if (ret <= 0) {
			ERR("Receiving the FD, exiting");
			goto error;
		}
	}

error:
	return NULL;
}

/*
 * update_poll_array
 *
 * Allocate the pollfd structure and the local view of the out fds
 * to avoid doing a lookup in the linked list and concurrency issues
 * when writing is needed.
 * Returns the number of fds in the structures
 */
static int update_poll_array(struct pollfd **pollfd,
		struct ltt_kconsumerd_fd **local_kconsumerd_fd)
{
	struct ltt_kconsumerd_fd *iter;
	int i = 0;

	if (*pollfd != NULL) {
		free(*pollfd);
		*pollfd = NULL;
	}

	if (*local_kconsumerd_fd != NULL) {
		free(*local_kconsumerd_fd);
		*local_kconsumerd_fd = NULL;
	}

	*pollfd = malloc(fds_count * sizeof(struct pollfd));
	if (*pollfd == NULL) {
		perror("pollfd malloc");
		goto error_mem;
	}

	*local_kconsumerd_fd = malloc(fds_count * sizeof(struct ltt_kconsumerd_fd));
	if (*local_kconsumerd_fd == NULL) {
		perror("local_kconsumerd_fd malloc");
		goto error_mem;
	}

	DBG("Updating poll fd array");
	pthread_mutex_lock(&kconsumerd_lock_fds);

	cds_list_for_each_entry(iter, &kconsumerd_fd_list.head, list) {
		DBG("Inside for each");
		if (iter->state == ACTIVE_FD) {
			DBG("Active FD %d", iter->consumerd_fd);
			(*pollfd)[i].fd = iter->consumerd_fd;
			(*pollfd)[i].events = POLLIN | POLLPRI;
			local_kconsumerd_fd[i] = iter;
			i++;
		} else if (iter->state == DELETE_FD) {
			del_fd(iter);
		}
	}
	update_fd_array = 0;
	pthread_mutex_unlock(&kconsumerd_lock_fds);
	return i;

error_mem:
	return -ENOMEM;
}

/*
 *  thread_poll_fds
 *
 *  This thread polls the fds in the ltt_fd_list to consume the data
 *  and write it to tracefile if necessary.
 */
static void *thread_poll_fds(void *data)
{
	int num_rdy, num_hup, high_prio, ret, i;
	struct pollfd *pollfd = NULL;
	/* local view of the fds */
	struct ltt_kconsumerd_fd **local_kconsumerd_fd = NULL;
	/* local view of fds_count */
	int nb_fd = 0;

	ret = pipe(thread_pipe);
	if (ret < 0) {
		perror("Error creating pipe");
		goto end;
	}

	local_kconsumerd_fd = malloc(sizeof(struct ltt_kconsumerd_fd));

	while (1) {
		high_prio = 0;
		num_hup = 0;

		/*
		 * the ltt_fd_list has been updated, we need to update our
		 * local array as well
		 */
		if (update_fd_array) {
			ret = update_poll_array(&pollfd, local_kconsumerd_fd);
			if (ret < 0) {
				ERR("Error in allocating pollfd or local_outfds");
				send_error(KCONSUMERD_POLL_ERROR);
				goto end;
			}
			nb_fd = ret;
		}

		/* poll on the array of fds */
		DBG("polling on %d fd", nb_fd);
		num_rdy = poll(pollfd, nb_fd, POLL_TIMEOUT);
		DBG("poll num_rdy : %d", num_rdy);
		if (num_rdy == -1) {
			perror("Poll error");
			send_error(KCONSUMERD_POLL_ERROR);
			goto end;
		}

		/* Take care of high priority channels first. */
		for (i = 0; i < nb_fd; i++) {
			switch(pollfd[i].revents) {
				case POLLERR:
					ERR("Error returned in polling fd %d.", pollfd[i].fd);
					num_hup++;
					send_error(KCONSUMERD_POLL_ERROR);
					break;
				case POLLHUP:
					ERR("Polling fd %d tells it has hung up.", pollfd[i].fd);
					num_hup++;
					break;
				case POLLNVAL:
					ERR("Polling fd %d tells fd is not open.", pollfd[i].fd);
					send_error(KCONSUMERD_POLL_NVAL);
					num_hup++;
					break;
				case POLLPRI:
					DBG("Urgent read on fd %d", pollfd[i].fd);
					high_prio = 1;
					ret = read_subbuffer(local_kconsumerd_fd[i]);
					/* it's ok to have an unavailable sub-buffer (FIXME : is it ?) */
					if (ret == EAGAIN) {
						ret = 0;
					}
					break;
			}
		}

		/* If every buffer FD has hung up, we end the read loop here */
		if (nb_fd > 0 && num_hup == nb_fd) {
			DBG("every buffer FD has hung up\n");
			send_error(KCONSUMERD_POLL_HUP);
			goto end;
		}

		/* Take care of low priority channels. */
		if (!high_prio) {
			for (i = 0; i < nb_fd; i++) {
				switch(pollfd[i].revents) {
					case POLLIN:
						DBG("Normal read on fd %d", pollfd[i].fd);
						ret = read_subbuffer(local_kconsumerd_fd[i]);
						/* it's ok to have an unavailable subbuffer (FIXME : is it ?) */
						if (ret == EAGAIN) {
							ret = 0;
						}
						break;
				}
			}
		}
	}
end:
	if (pollfd != NULL) {
		free(pollfd);
		pollfd = NULL;
	}
	if (local_kconsumerd_fd != NULL) {
		free(local_kconsumerd_fd);
		local_kconsumerd_fd = NULL;
	}
	cleanup();
	return NULL;
}

/*
 * usage function on stderr
 */
static void usage(void)
{
	fprintf(stderr, "Usage: %s OPTIONS\n\nOptions:\n", progname);
	fprintf(stderr, "  -h, --help                         "
			"Display this usage.\n");
	fprintf(stderr, "  -c, --kconsumerd-cmd-sock PATH     "
			"Specify path for the command socket\n");
	fprintf(stderr, "  -e, --kconsumerd-err-sock PATH     "
			"Specify path for the error socket\n");
	fprintf(stderr, "  -d, --daemonize                    "
			"Start as a daemon.\n");
	fprintf(stderr, "  -q, --quiet                        "
			"No output at all.\n");
	fprintf(stderr, "  -v, --verbose                      "
			"Verbose mode. Activate DBG() macro.\n");
	fprintf(stderr, "  -V, --version                      "
			"Show version number.\n");
}

/*
 * daemon argument parsing
 */
static void parse_args(int argc, char **argv)
{
	int c;

	static struct option long_options[] = {
		{ "kconsumerd-cmd-sock", 1, 0, 'c' },
		{ "kconsumerd-err-sock", 1, 0, 'e' },
		{ "daemonize", 0, 0, 'd' },
		{ "help", 0, 0, 'h' },
		{ "quiet", 0, 0, 'q' },
		{ "verbose", 0, 0, 'v' },
		{ "version", 0, 0, 'V' },
		{ NULL, 0, 0, 0 }
	};

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "dhqvV" "c:e:", long_options, &option_index);
		if (c == -1) {
			break;
		}

		switch (c) {
			case 0:
				fprintf(stderr, "option %s", long_options[option_index].name);
				if (optarg) {
					fprintf(stderr, " with arg %s\n", optarg);
				}
				break;
			case 'c':
				snprintf(command_sock_path, PATH_MAX, "%s", optarg);
				break;
			case 'e':
				snprintf(error_sock_path, PATH_MAX, "%s", optarg);
				break;
			case 'd':
				opt_daemon = 1;
				break;
			case 'h':
				usage();
				exit(EXIT_FAILURE);
			case 'q':
				opt_quiet = 1;
				break;
			case 'v':
				opt_verbose = 1;
				break;
			case 'V':
				fprintf(stdout, "%s\n", VERSION);
				exit(EXIT_SUCCESS);
			default:
				usage();
				exit(EXIT_FAILURE);
		}
	}
}


/*
 * main
 */
int main(int argc, char **argv)
{
	int i;
	int ret = 0;
	void *status;

	/* Parse arguments */
	progname = argv[0];
	parse_args(argc, argv);

	/* Daemonize */
	if (opt_daemon) {
		ret = daemon(0, 0);
		if (ret < 0) {
			perror("daemon");
			goto error;
		}
	}

	if (strlen(command_sock_path) == 0) {
		snprintf(command_sock_path, PATH_MAX,
				KCONSUMERD_CMD_SOCK_PATH);
	}
	if (strlen(error_sock_path) == 0) {
		snprintf(error_sock_path, PATH_MAX,
				KCONSUMERD_ERR_SOCK_PATH);
	}

	if (set_signal_handler() < 0) {
		goto error;
	}

	/* Connect to the socket created by ltt-sessiond to report errors */
	DBG("Connecting to error socket %s", error_sock_path);
	error_socket = lttcomm_connect_unix_sock(error_sock_path);
	/* not a fatal error, but all communication with ltt-sessiond will fail */
	if (error_socket < 0) {
		WARN("Cannot connect to error socket, is ltt-sessiond started ?");
	}

	/* Create the thread to manage the receive of fd */
	ret = pthread_create(&threads[0], NULL, thread_receive_fds, (void *) NULL);
	if (ret != 0) {
		perror("pthread_create");
		goto error;
	}

	/* Create thread to manage the polling/writing of traces */
	ret = pthread_create(&threads[1], NULL, thread_poll_fds, (void *) NULL);
	if (ret != 0) {
		perror("pthread_create");
		goto error;
	}

	for (i = 0; i < 2; i++) {
		ret = pthread_join(threads[i], &status);
		if (ret != 0) {
			perror("pthread_join");
			goto error;
		}
	}
	ret = EXIT_SUCCESS;
	send_error(KCONSUMERD_EXIT_SUCCESS);
	goto end;

error:
	ret = EXIT_FAILURE;
	send_error(KCONSUMERD_EXIT_FAILURE);

end:
	cleanup();

	return ret;
}