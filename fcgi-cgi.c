
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fastcgi.h"

#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stropts.h>
#include <sys/stat.h>

#define MAX_BUFFER_SIZE (64*1024)

#define CONST_STR_LEN(x) (x), sizeof(x) - 1
#define GSTR_LEN(x) (x) ? (x)->str : "", (x) ? (x)->len : 0
#define UNUSED(x) ((void)(x))

/* #define ERROR(...) g_printerr(G_STRLOC " (" G_STRFUNC "): " __VA_ARGS__) */
#define __STR(x) #x
#define ERROR(...) g_printerr("fcgi-cgi.c:" G_STRINGIFY(__LINE__) ": " __VA_ARGS__)

struct fcgi_cgi_server;
typedef struct fcgi_cgi_server fcgi_cgi_server;

struct fcgi_cgi_child;
typedef struct fcgi_cgi_child fcgi_cgi_child;

struct fcgi_cgi_server {
	struct ev_loop *loop;

	fastcgi_server *fsrv;
	GPtrArray *aborted_pending_childs;

	ev_signal
		sig_w_INT,
		sig_w_TERM,
		sig_w_HUP;
};

struct fcgi_cgi_child {
	fcgi_cgi_server *srv;
	fastcgi_connection *fcon;
	gint aborted_id;

	pid_t pid;
	gint child_status;
	ev_child child_watcher;

	gint pipe_in, pipe_out, pipe_err;
	ev_io pipe_in_watcher, pipe_out_watcher, pipe_err_watcher;

	/* write queue */
	fastcgi_queue write_queue;
};

static fcgi_cgi_child* fcgi_cgi_child_create(fcgi_cgi_server *srv, fastcgi_connection *fcon);
static void fcgi_cgi_child_check_done(fcgi_cgi_child *cld);
static void fcgi_cgi_child_close_write(fcgi_cgi_child *cld);
static void fcgi_cgi_child_close_read(fcgi_cgi_child *cld);
static void fcgi_cgi_child_close_error(fcgi_cgi_child *cld);
static void fcgi_cgi_child_free(fcgi_cgi_child *cld);
static void fcgi_cgi_child_error(fcgi_cgi_child *cld);
static void fcgi_cgi_child_start(fcgi_cgi_child *cld, const gchar *path);
static void fcgi_cgi_wrote_data(fastcgi_connection *fcon);

/* move a fd to another and close the old one */
static void move2fd(int srcfd, int dstfd) {
	if (srcfd != dstfd) {
		close(dstfd);
		dup2(srcfd, dstfd);
		close(srcfd);
	}
}

static void fd_init(int fd) {
#ifdef _WIN32
	int i = 1;
#endif
#ifdef FD_CLOEXEC
	/* close fd on exec (cgi) */
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
#ifdef O_NONBLOCK
	fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR);
#elif defined _WIN32
	ioctlsocket(fd, FIONBIO, &i);
#endif
}

static void fcgi_cgi_child_child_cb(struct ev_loop *loop, ev_child *w, int revents) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) w->data;
	UNUSED(revents);

	ev_child_stop(loop, w);

	fcgi_cgi_child_close_write(cld);

	cld->pid = -1;
	cld->child_status = w->rstatus;
	fcgi_cgi_child_check_done(cld);
}

static void write_queue(fcgi_cgi_child *cld) {
	if (-1 == cld->pipe_out) return;

	if (fastcgi_queue_write(cld->pipe_out, &cld->write_queue, 256*1024) < 0) {
		fcgi_cgi_child_close_write(cld);
		return;
	}

	if (-1 != cld->pipe_out) {
		if (cld->write_queue.length > 0) {
			ev_io_start(cld->srv->loop, &cld->pipe_out_watcher);
			if (cld->write_queue.length > MAX_BUFFER_SIZE) {
				fastcgi_suspend_read(cld->fcon);
			} else {
				fastcgi_resume_read(cld->fcon);
			}
		} else {
			if (cld->write_queue.closed) {
				fcgi_cgi_child_close_write(cld);
			} else {
				ev_io_stop(cld->srv->loop, &cld->pipe_out_watcher);
				fastcgi_resume_read(cld->fcon);
			}
		}
	}
}

static GByteArray* read_chunk(gint fd, guint maxlen) {
	gssize res;
	GByteArray *buf;
	int tmp_errno;

	buf = g_byte_array_sized_new(maxlen);
	g_byte_array_set_size(buf, maxlen);
	if (0 == maxlen) return buf;

	res = read(fd, buf->data, maxlen);
	if (res == -1) {
		tmp_errno = errno;
		g_byte_array_free(buf, TRUE);
		errno = tmp_errno;
		return NULL;
	} else if (res == 0) {
		g_byte_array_free(buf, TRUE);
		errno = ECONNRESET;
		return NULL;
	} else {
		g_byte_array_set_size(buf, res);
		return buf;
	}
}

static void fcgi_cgi_child_pipe_in_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) w->data;
	GByteArray *buf;
	UNUSED(loop); UNUSED(revents);

	if (NULL == (buf = read_chunk(cld->pipe_in, 64*1024))) {
		switch (errno) {
		case EINTR:
		case EAGAIN:
#if EWOULDBLOCK != EAGAIN
		case EWOULDBLOCK:
#endif
			return; /* try again later */
		case ECONNRESET:
			fcgi_cgi_child_close_read(cld);
			break;
		default:
			ERROR("read from fd=%d failed, %s\n", cld->pipe_in, g_strerror(errno));
			fcgi_cgi_child_close_read(cld);
			break;
		}
	} else if (cld->fcon) {
		fastcgi_send_out_bytearray(cld->fcon, buf);
		fcgi_cgi_wrote_data(cld->fcon);
	} else {
		g_byte_array_free(buf, TRUE);
	}
}

static void fcgi_cgi_child_pipe_out_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) w->data;
	UNUSED(loop); UNUSED(revents);

	write_queue(cld);
}

static void fcgi_cgi_child_pipe_err_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) w->data;
	GByteArray *buf;
	UNUSED(loop); UNUSED(revents);

	if (NULL == (buf = read_chunk(cld->pipe_err, 64*1024))) {
		switch (errno) {
		case EINTR:
		case EAGAIN:
#if EWOULDBLOCK != EAGAIN
		case EWOULDBLOCK:
#endif
			return; /* try again later */
		case ECONNRESET:
			fcgi_cgi_child_close_error(cld);
			break;
		default:
			ERROR("read from fd=%d failed, %s\n", cld->pipe_err, g_strerror(errno));
			fcgi_cgi_child_close_error(cld);
			break;
		}
	} else if (cld->fcon) {
		fastcgi_send_err_bytearray(cld->fcon, buf);
		fcgi_cgi_wrote_data(cld->fcon);
	} else {
		g_byte_array_free(buf, TRUE);
	}
}

static fcgi_cgi_child* fcgi_cgi_child_create(fcgi_cgi_server *srv, fastcgi_connection *fcon) {
	fcgi_cgi_child *cld = g_slice_new0(fcgi_cgi_child);

	cld->srv = srv;
	cld->fcon = fcon;
	cld->aborted_id = -1;

	cld->pid = -1;
	ev_child_init(&cld->child_watcher, fcgi_cgi_child_child_cb, -1, 0);
	cld->child_watcher.data = cld;

	cld->pipe_in = cld->pipe_out = -1;
	ev_io_init(&cld->pipe_in_watcher, fcgi_cgi_child_pipe_in_cb, -1, 0);
	cld->pipe_in_watcher.data = cld;
	ev_io_init(&cld->pipe_out_watcher, fcgi_cgi_child_pipe_out_cb, -1, 0);
	cld->pipe_out_watcher.data = cld;
	ev_io_init(&cld->pipe_err_watcher, fcgi_cgi_child_pipe_err_cb, -1, 0);
	cld->pipe_err_watcher.data = cld;

	return cld;
}

static void fcgi_cgi_child_check_done(fcgi_cgi_child *cld) {
	if (!cld->fcon) {
		if (-1 != cld->aborted_id && (cld->pid == -1 || (cld->pipe_out == -1 && cld->pipe_in == -1))) {
			fcgi_cgi_child *t_cld;
			GPtrArray *a = cld->srv->aborted_pending_childs;
			guint i = a->len - 1;
			t_cld = g_ptr_array_index(a, cld->aborted_id) = g_ptr_array_index(a, i);
			g_ptr_array_set_size(a, i);
			t_cld->aborted_id = cld->aborted_id;
			cld->aborted_id = -1;
			fcgi_cgi_child_free(cld);
		}
	} else {
		if (cld->pid == -1 && cld->pipe_out == -1 && cld->pipe_in == -1 && cld->pipe_err == -1) {
			fastcgi_end_request(cld->fcon, cld->child_status, FCGI_REQUEST_COMPLETE);
		}
	}
}

static void fcgi_cgi_child_close_write(fcgi_cgi_child *cld) {
	if (cld->pipe_out != -1) {
		ev_io_stop(cld->srv->loop, &cld->pipe_out_watcher);
		close(cld->pipe_out);
		cld->pipe_out = -1;
		fastcgi_queue_clear(&cld->write_queue);
		cld->write_queue.closed = TRUE;
		fcgi_cgi_child_check_done(cld);
	}
}

static void fcgi_cgi_child_close_read(fcgi_cgi_child *cld) {
	if (cld->pipe_in != -1) {
		ev_io_stop(cld->srv->loop, &cld->pipe_in_watcher);
		close(cld->pipe_in);
		cld->pipe_in = -1;
		if (cld->fcon) fastcgi_send_out(cld->fcon, NULL);
		fcgi_cgi_child_check_done(cld);
	}
}

static void fcgi_cgi_child_close_error(fcgi_cgi_child *cld) {
	if (cld->pipe_err != -1) {
		ev_io_stop(cld->srv->loop, &cld->pipe_err_watcher);
		close(cld->pipe_err);
		cld->pipe_err = -1;
		if (cld->fcon) fastcgi_send_err(cld->fcon, NULL);
		fcgi_cgi_child_check_done(cld);
	}
}

static void fcgi_cgi_child_free(fcgi_cgi_child *cld) {
	if (cld->fcon) cld->fcon->data = NULL;
	cld->fcon = NULL;
	fcgi_cgi_child_close_write(cld);
	fcgi_cgi_child_close_read(cld);
	fcgi_cgi_child_close_error(cld);
	ev_child_stop(cld->srv->loop, &cld->child_watcher);
	g_slice_free(fcgi_cgi_child, cld);
}

static void fcgi_cgi_child_error(fcgi_cgi_child *cld) {
	if (cld->fcon) {
		fastcgi_connection_close(cld->fcon);
	}
}

static void fcgi_cgi_child_start(fcgi_cgi_child *cld, const gchar *path) {
	int pipes_to[2] = {-1, -1}, pipes_from[2] = {-1, -1}, pipes_err[2] = {-1, -1};
	pid_t pid;

	if (-1 == pipe(pipes_to)) {
		ERROR("couldn't create pipe: %s\n", g_strerror(errno));
		goto error;
	}

	if (-1 == pipe(pipes_from)) {
		ERROR("couldn't create pipe: %s\n", g_strerror(errno));
		goto error;
	}

	if (-1 == pipe(pipes_err)) {
		ERROR("couldn't create pipe: %s\n", g_strerror(errno));
		goto error;
	}

	pid = fork();
	switch (pid) {
	case 0: {
		GPtrArray *enva = cld->fcon->environ;
		char **newenv;
		char *const args[] = { (char *) path, NULL };

		close(pipes_to[1]); close(pipes_from[0]); close(pipes_err[0]);
		move2fd(pipes_to[0], 0);
		move2fd(pipes_from[1], 1);
		move2fd(pipes_err[1], 2);

		/* try changing the directory. don't care about memleaks, execve() coming soon :) */
		{
			char *dir = strdup(path), *sep;
			if (NULL == (sep = strrchr(dir, '/'))) {
				chdir("/");
			} else {
				*sep = '\0';
				chdir(dir);
			}
		}

		g_ptr_array_add(enva, NULL);
		newenv = (char**) g_ptr_array_free(enva, FALSE);
		execve(path, args, newenv);

		ERROR("couldn't execve '%s': %s\n", path, g_strerror(errno));
		exit(-1);

		}
		break;
	case -1:
		ERROR("couldn't fork: %s\n", g_strerror(errno));
		goto error;
	default:
		cld->pid = pid;
		ev_child_set(&cld->child_watcher, cld->pid, 0);
		ev_child_start(cld->srv->loop, &cld->child_watcher);
		close(pipes_to[0]); close(pipes_from[1]); close(pipes_err[1]);
		fd_init(pipes_to[1]); fd_init(pipes_from[0]); fd_init(pipes_err[0]);
		cld->pipe_out = pipes_to[1];
		cld->pipe_in = pipes_from[0];
		cld->pipe_err = pipes_err[0];
		ev_io_set(&cld->pipe_out_watcher, cld->pipe_out, EV_WRITE);
		ev_io_set(&cld->pipe_in_watcher, cld->pipe_in, EV_READ);
		ev_io_set(&cld->pipe_err_watcher, cld->pipe_err, EV_READ);
		if (cld->write_queue.length > 0) ev_io_start(cld->srv->loop, &cld->pipe_out_watcher);
		ev_io_start(cld->srv->loop, &cld->pipe_in_watcher);
		ev_io_start(cld->srv->loop, &cld->pipe_err_watcher);
		break;
	}

	return;
error:
	close(pipes_to[0]); close(pipes_to[1]);
	close(pipes_from[0]); close(pipes_from[1]);
	close(pipes_err[0]); close(pipes_err[1]);
	fcgi_cgi_child_error(cld);
}

static void fcgi_cgi_direct_result(fastcgi_connection *fcon, int status) {
	GString *s = g_string_new(0);
	g_string_append_printf(s, "Status: %i\r\n\r\n", status);
	fastcgi_send_out(fcon, s);
	fastcgi_send_out(fcon, NULL);
	fastcgi_send_err(fcon, NULL);
	fastcgi_end_request(fcon, 0, FCGI_REQUEST_COMPLETE);
}

static void fcgi_cgi_new_request(fastcgi_connection *fcon) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) fcon->data;
	const gchar *binpath;
	struct stat st;
	if (cld) return;

	binpath = fastcgi_connection_environ_lookup(fcon, CONST_STR_LEN("INTERPRETER"));
	if (!binpath) binpath = fastcgi_connection_environ_lookup(fcon, CONST_STR_LEN("SCRIPT_FILENAME"));

	if (!binpath) {
		fcgi_cgi_direct_result(fcon, 500);
		return;
	}

	if (-1 == stat(binpath, &st)) {
		switch (errno) {
		case EACCES:
		case ENOTDIR:
			fcgi_cgi_direct_result(fcon, 403);
			break;
		case ENOENT:
			fcgi_cgi_direct_result(fcon, 404);
			break;
		default:
			fcgi_cgi_direct_result(fcon, 500);
			break;
		}
		return;
	}

	if (!S_ISREG(st.st_mode) || !((S_IXUSR | S_IXGRP | S_IXOTH) && st.st_mode)) {
		fcgi_cgi_direct_result(fcon, 403);
		return;
	}

	cld = fcgi_cgi_child_create(fcon->fsrv->data, fcon);
	fcon->data = cld;

	fcgi_cgi_child_start(cld, binpath);
}

static void fcgi_cgi_wrote_data(fastcgi_connection *fcon) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) fcon->data;
	if (!cld) return;
	if (cld->fcon->write_queue.length < MAX_BUFFER_SIZE) {
		if (-1 != cld->pipe_in) ev_io_start(cld->srv->loop, &cld->pipe_in_watcher);
		if (-1 != cld->pipe_err) ev_io_start(cld->srv->loop, &cld->pipe_err_watcher);
	} else {
		if (-1 != cld->pipe_in) ev_io_stop(cld->srv->loop, &cld->pipe_in_watcher);
		if (-1 != cld->pipe_err) ev_io_stop(cld->srv->loop, &cld->pipe_err_watcher);
	}
}

static void fcgi_cgi_received_stdin(fastcgi_connection *fcon, GByteArray *data) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) fcon->data;
	/* if proc is running but pipe closed -> drop data */
	if (!cld || cld->write_queue.closed) {
		if (data) g_byte_array_free(data, TRUE);
		return;
	}
	fastcgi_queue_append_bytearray(&cld->write_queue, data);
	write_queue(cld); /* if we don't call this we have to check the write-queue length */
}

static void fcgi_cgi_request_aborted(fastcgi_connection *fcon) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) fcon->data;
	if (!cld) return;
	fcgi_cgi_child_close_write(cld);
}

static void fcgi_cgi_reset_connection(fastcgi_connection *fcon) {
	fcgi_cgi_child *cld = (fcgi_cgi_child*) fcon->data;
	if (!cld) return;
	fcon->data = NULL;
	cld->fcon = NULL;
	if (cld->pid == -1 || (cld->pipe_out == -1 && cld->pipe_in == -1)) {
		fcgi_cgi_child_free(cld);
	} else {
		fcgi_cgi_child_close_write(cld);
		cld->aborted_id = cld->srv->aborted_pending_childs->len;
		g_ptr_array_add(cld->srv->aborted_pending_childs, cld);
	}
}

static const fastcgi_callbacks cgi_callbacks = {
	/* cb_new_connection: */ NULL,
	/* cb_new_request: */ fcgi_cgi_new_request,
	/* cb_wrote_data: */ fcgi_cgi_wrote_data,
	/* cb_received_stdin: */ fcgi_cgi_received_stdin,
	/* cb_received_data: */ NULL,
	/* cb_request_aborted: */ fcgi_cgi_request_aborted,
	/* cb_reset_connection: */ fcgi_cgi_reset_connection
};

static fcgi_cgi_server* fcgi_cgi_server_create(struct ev_loop *loop, int fd) {
	fcgi_cgi_server* srv = g_slice_new0(fcgi_cgi_server);
	srv->loop = loop;
	srv->aborted_pending_childs = g_ptr_array_new();
	srv->fsrv = fastcgi_server_create(loop, fd, &cgi_callbacks, 10);
	srv->fsrv->data = srv;
	return srv;
}

static void fcgi_cgi_server_free(fcgi_cgi_server* srv) {
	guint i;
	for (i = 0; i < srv->aborted_pending_childs->len; i++) {
		fcgi_cgi_child_free(g_ptr_array_index(srv->aborted_pending_childs, i));
	}
	g_ptr_array_free(srv->aborted_pending_childs, TRUE);
	fastcgi_server_free(srv->fsrv);
	g_slice_free(fcgi_cgi_server, srv);
}

#define CATCH_SIGNAL(loop, cb, n) do {\
	ev_init(&srv->sig_w_##n, cb); \
	ev_signal_set(&srv->sig_w_##n, SIG##n); \
	ev_signal_start(loop, &srv->sig_w_##n); \
	srv->sig_w_##n.data = srv; \
	ev_unref(loop); /* Signal watchers shouldn't keep loop alive */ \
} while (0)

#define UNCATCH_SIGNAL(loop, n) do {\
	ev_ref(loop); \
	ev_signal_stop(loop, &srv->sig_w_##n); \
} while (0)

static void sigint_cb(struct ev_loop *loop, struct ev_signal *w, int revents) {
	fcgi_cgi_server *srv = (fcgi_cgi_server*) w->data;
	UNUSED(revents);

	if (!srv->fsrv->do_shutdown) {
		ERROR("Got signal, shutdown\n");
		fastcgi_server_stop(srv->fsrv);
	} else {
		ERROR("Got second signal, force shutdown\n");
		ev_unloop(loop, EVUNLOOP_ALL);
	}
}

int main(int argc, char **argv) {
	struct ev_loop *loop;
	fcgi_cgi_server* srv;
	UNUSED(argc);
	UNUSED(argv);

	loop = ev_default_loop(0);
	srv = fcgi_cgi_server_create(loop, 0);

	signal(SIGPIPE, SIG_IGN);
	CATCH_SIGNAL(loop, sigint_cb, INT);
	CATCH_SIGNAL(loop, sigint_cb, TERM);
	CATCH_SIGNAL(loop, sigint_cb, HUP);

	ev_loop(loop, 0);
	fcgi_cgi_server_free(srv);
	return 0;
}
