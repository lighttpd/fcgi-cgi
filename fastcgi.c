
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fastcgi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* some util functions */
#define GSTR_LEN(x) (x) ? (x)->str : "", (x) ? (x)->len : 0
#define UNUSED(x) ((void)(x))
#define ERROR(...) g_printerr("fastcgi.c:" G_STRINGIFY(__LINE__) ": " __VA_ARGS__)

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

static void ev_io_add_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if ((watcher->events & events) == events) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events | events);
	ev_io_start(loop, watcher);
}

static void ev_io_rem_events(struct ev_loop *loop, ev_io *watcher, int events) {
	if (0 == (watcher->events & events)) return;
	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events & ~events);
	ev_io_start(loop, watcher);
}
/* end: some util functions */

static const gchar __padding[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void append_padding(GString *s, guint8 padlen) {
	g_string_append_len(s, __padding, padlen);
}

/* returns padding length */
static guint8 stream_build_fcgi_record(GString *buf, guint8 type, guint16 requestid, guint16 datalen) {
	guint16 w;
	guint8 padlen = (8 - (datalen & 0x7)) % 8; /* padding must be < 8 */

	g_string_set_size(buf, FCGI_HEADER_LEN);
	g_string_truncate(buf, 0);

	g_string_append_c(buf, FCGI_VERSION_1);
	g_string_append_c(buf, type);
	w = htons(requestid);
	g_string_append_len(buf, (const gchar*) &w, sizeof(w));
	w = htons(datalen);
	g_string_append_len(buf, (const gchar*) &w, sizeof(w));
	g_string_append_c(buf, padlen);
	g_string_append_c(buf, 0);
	return padlen;
}

/* returns padding length */
static guint8 stream_send_fcgi_record(fastcgi_gstring_queue *out, guint8 type, guint16 requestid, guint16 datalen) {
	GString *record = g_string_sized_new(FCGI_HEADER_LEN);
	guint8 padlen = stream_build_fcgi_record(record, type, requestid, datalen);
	fastcgi_gstring_queue_append(out, record);
	return padlen;
}

static void stream_send_data(fastcgi_gstring_queue *out, guint8 type, guint16 requestid, const gchar *data, size_t datalen) {
	while (datalen > 0) {
		guint16 tosend = (datalen > G_MAXUINT16) ? G_MAXUINT16 : datalen;
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, tosend);
		GString *tmps = g_string_sized_new(tosend + padlen);
		g_string_append_len(tmps, data, tosend);
		append_padding(tmps, padlen);
		fastcgi_gstring_queue_append(out, tmps);
		data += tosend;
		datalen -= tosend;
	}
}

/* kills string */
static void stream_send_string(fastcgi_gstring_queue *out, guint8 type, guint16 requestid, GString *data) {
	if (data->len > G_MAXUINT16) {
		stream_send_data(out, type, requestid, GSTR_LEN(data));
		g_string_free(data, TRUE);
	} else {
		guint8 padlen = stream_send_fcgi_record(out, type, requestid, data->len);
		append_padding(data, padlen);
		fastcgi_gstring_queue_append(out, data);
	}
}

static void stream_send_end_request(fastcgi_gstring_queue *out, guint16 requestID, gint32 appStatus, enum FCGI_ProtocolStatus status) {
	GString *record;
	record = g_string_sized_new(16);
	stream_build_fcgi_record(record, FCGI_END_REQUEST, requestID, 8);
	appStatus = htonl(appStatus);
	g_string_append_len(record, (const gchar*) &appStatus, sizeof(appStatus));
	g_string_append_c(record, status);
	g_string_append_len(record, __padding, 3);
	fastcgi_gstring_queue_append(out, record);
}


static void write_queue(fastcgi_connection *fcon) {
	const gssize max_rem_write = 256*1024;
	gssize rem_write = 256*1024;
#ifdef TCP_CORK
	int corked = 0;
#endif

	if (fcon->closing) return;

#ifdef TCP_CORK
	/* Linux: put a cork into the socket as we want to combine the write() calls
	 * but only if we really have multiple chunks
	 */
	if (fcon->write_queue->queue.length > 1) {
		corked = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

	while (rem_write > 0 && fcon->write_queue.length > 0) {
		GString *s = g_queue_peek_head(&fcon->write_queue.queue);
		gssize towrite = s->len - fcon->write_queue.offset, res;
		if (towrite > max_rem_write) towrite = max_rem_write;
		res = write(fcon->fd, s->str + fcon->write_queue.offset, towrite);
		if (-1 == res) {
#ifdef TCP_CORK
			if (corked) {
				corked = 0;
				setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
			}
#endif
			switch (errno) {
			case EINTR:
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				goto out; /* try again later */
			case ECONNRESET:
			case EPIPE:
				fastcgi_connection_close(fcon);
				return;
			default:
				ERROR("write to fd=%d failed, %s\n", fcon->fd, g_strerror(errno) );
				fastcgi_connection_close(fcon);
				return;
			}
		} else {
			fcon->write_queue.offset += res;
			rem_write -= res;
			if (fcon->write_queue.offset == s->len) {
				g_queue_pop_head(&fcon->write_queue.queue);
				fcon->write_queue.offset = 0;
				fcon->write_queue.length -= s->len;
				g_string_free(s, TRUE);
			}
		}
	}

#ifdef TCP_CORK
	if (corked) {
		corked = 0;
		setsockopt(fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
	}
#endif

out:
	if (!fcon->closing && rem_write != max_rem_write) {
		if (fcon->fsrv->callbacks->cb_wrote_data) {
			fcon->fsrv->callbacks->cb_wrote_data(fcon);
		}
	}

	if (!fcon->closing) {
		if (fcon->write_queue.length > 0) {
			ev_io_add_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_WRITE);
		} else {
			ev_io_rem_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_WRITE);
			if (0 == fcon->requestID) {
				if (!(fcon->flags & FCGI_KEEP_CONN)) {
					fastcgi_connection_close(fcon);
				}
			}
		}
	}
}

static GString* read_chunk(fastcgi_connection *fcon, guint maxlen) {
	gssize res;
	GString *str;
	int tmp_errno;

	str = g_string_sized_new(maxlen);
	g_string_set_size(str, maxlen);
	res = read(fcon->fd, str->str, maxlen);
	if (res == -1) {
		tmp_errno = errno;
		g_string_free(str, TRUE);
		errno = tmp_errno;
		return NULL;
	} else if (res == 0) {
		g_string_free(str, TRUE);
		errno = ECONNRESET;
		return NULL;
	} else {
		g_string_set_size(str, res);
		return str;
	}
}

static gboolean read_append_chunk(fastcgi_connection *fcon, GString *str) {
	gssize res;
	int tmp_errno;
	guint curlen = str->len;
	const guint maxlen = fcon->content_remaining;
	if (0 == maxlen) return TRUE;

	g_string_set_size(str, curlen + maxlen);
	res = read(fcon->fd, str->str + curlen, maxlen);
	if (res == -1) {
		tmp_errno = errno;
		g_string_set_size(str, curlen);
		errno = tmp_errno;
		return FALSE;
	} else if (res == 0) {
		g_string_set_size(str, curlen);
		errno = ECONNRESET;
		return FALSE;
	} else {
		g_string_set_size(str, curlen + res);
		fcon->content_remaining -= res;
		return TRUE;
	}
}

static gboolean read_key_value(fastcgi_connection *fcon, GString *buf, guint *pos, gchar **key, guint *keylen, gchar **value, guint *valuelen) {
	const unsigned char *data = (const unsigned char*) buf->str;
	guint32 klen, vlen;
	guint p = *pos, len = buf->len;

	if (len - p < 2) return FALSE;

	klen = data[p++];
	if (klen & 0x80) {
		if (len - p < 100) return FALSE;
		klen = ((klen & 0x7f) << 24) | (data[p] << 16) | (data[p+1] << 8) | data[p+2];
		p += 3;
	}
	vlen = data[p++];
	if (vlen & 0x80) {
		if (len - p < 100) return FALSE;
		vlen = ((vlen & 0x7f) << 24) | (data[p] << 16) | (data[p+1] << 8) | data[p+2];
		p += 3;
	}
	if (klen > FASTCGI_MAX_KEYLEN || vlen > FASTCGI_MAX_VALUELEN) {
		fastcgi_connection_close(fcon);
		return FALSE;
	}
	if (len - p < klen + vlen) return FALSE;
	*key = &buf->str[p];
	*keylen = klen;
	p += klen;
	*value = &buf->str[p];
	*valuelen = vlen;
	p += vlen;
	*pos = p;
	return TRUE;
}

static void parse_params(const fastcgi_callbacks *fcbs, fastcgi_connection *fcon) {
	if (!fcon->current_header.contentLength) {
		fcbs->cb_new_request(fcon);
		g_string_truncate(fcon->parambuf, 0);
	} else {
		guint pos = 0, keylen = 0, valuelen = 0;
		gchar *key = NULL, *value = NULL;
		while (read_key_value(fcon, fcon->parambuf, &pos, &key, &keylen, &value, &valuelen)) {
			gchar *envvar = g_malloc(keylen + valuelen + 2);
			memcpy(envvar, key, keylen);
			envvar[keylen] = '=';
			memcpy(envvar + keylen + 1, value, valuelen);
			envvar[keylen+valuelen+1] = '\0';
			g_ptr_array_add(fcon->environ, envvar);
		}
		if (!fcon->closing)
			g_string_erase(fcon->parambuf, 0, pos);
	}
}

static void parse_get_values(fastcgi_connection *fcon) {
	/* just send the request back and don't insert results */
	GString *tmp = g_string_sized_new(0);
	stream_send_string(&fcon->write_queue, FCGI_GET_VALUES_RESULT, 0, fcon->buffer);
	*fcon->buffer = *tmp;
	/* TODO: provide get-values result */
}

static void read_queue(fastcgi_connection *fcon) {
	gssize res;
	GString *buf;
	const fastcgi_callbacks *fcbs = fcon->fsrv->callbacks;

	for (;;) {
		if (fcon->closing || fcon->read_suspended) return;

		if (fcon->headerbuf_used < 8) {
			const unsigned char *data = fcon->headerbuf;
			res = read(fcon->fd, fcon->headerbuf + fcon->headerbuf_used, 8 - fcon->headerbuf_used);
			if (0 == res) { errno = ECONNRESET; goto handle_error; }
			if (-1 == res) goto handle_error;
			fcon->headerbuf_used += res;
			if (fcon->headerbuf_used < 8) return; /* need more data */

			fcon->current_header.version = data[0];
			fcon->current_header.type = data[1];
			fcon->current_header.requestID = (data[2] << 8) | (data[3]);
			fcon->current_header.contentLength = (data[4] << 8) | (data[5]);
			fcon->current_header.paddingLength = data[6];
			fcon->content_remaining = fcon->current_header.contentLength;
			fcon->padding_remaining = fcon->current_header.paddingLength;
			fcon->first = TRUE;
			g_string_truncate(fcon->buffer, 0);

			if (fcon->current_header.version != FCGI_VERSION_1) {
				fastcgi_connection_close(fcon);
				return;
			}
		}

		if (fcon->current_header.type != FCGI_BEGIN_REQUEST &&
		    (0 != fcon->current_header.requestID) && fcon->current_header.requestID != fcon->requestID) {
		    /* ignore packet data */
			if (0 == fcon->content_remaining + fcon->padding_remaining) {
				fcon->headerbuf_used = 0;
			} else {
				if (NULL == (buf = read_chunk(fcon, fcon->content_remaining + fcon->padding_remaining))) goto handle_error;
				if (buf->len >= fcon->content_remaining) {
					fcon->padding_remaining -= buf->len - fcon->content_remaining;
					if (0 == fcon->padding_remaining) fcon->headerbuf_used = 0;
				} else {
					fcon->content_remaining -= buf->len;
				}
				g_string_free(buf, TRUE);
			}
		}

		if (fcon->first || fcon->content_remaining) {
			fcon->first = FALSE;
			switch (fcon->current_header.type) {
			case FCGI_BEGIN_REQUEST:
				if (8 != fcon->current_header.contentLength || 0 == fcon->current_header.requestID) goto error;
				if (!read_append_chunk(fcon, fcon->buffer)) goto handle_error;
				if (0 == fcon->content_remaining) {
					if (fcon->requestID) {
						stream_send_end_request(&fcon->write_queue, fcon->current_header.requestID, 0, FCGI_CANT_MPX_CONN);
					} else {
						unsigned char *data = (unsigned char*) fcon->buffer->str;
						fcon->requestID = fcon->current_header.requestID;
						fcon->role = (data[0] << 8) | (data[1]);
						fcon->flags = data[2];
						g_string_truncate(fcon->parambuf, 0);
					}
				}
				break;
			case FCGI_ABORT_REQUEST:
				if (0 != fcon->current_header.contentLength || 0 == fcon->current_header.requestID) goto error;
				fcbs->cb_request_aborted(fcon);
				break;
			case FCGI_END_REQUEST:
				goto error; /* invalid type */
			case FCGI_PARAMS:
				if (0 == fcon->current_header.requestID) goto error;
				if (!read_append_chunk(fcon, fcon->parambuf)) goto handle_error;
				parse_params(fcbs, fcon);
				break;
			case FCGI_STDIN:
				if (0 == fcon->current_header.requestID) goto error;
				buf = NULL;
				if (0 != fcon->content_remaining &&
				    NULL == (buf = read_chunk(fcon, fcon->content_remaining + fcon->padding_remaining))) goto handle_error;
				if (buf) fcon->content_remaining -= buf->len;
				if (fcbs->cb_received_stdin) {
					fcbs->cb_received_stdin(fcon, buf);
				} else {
					g_string_free(buf, TRUE);
				}
				break;
			case FCGI_STDOUT:
				goto error; /* invalid type */
			case FCGI_STDERR:
				goto error; /* invalid type */
			case FCGI_DATA:
				if (0 == fcon->current_header.requestID) goto error;
				buf = NULL;
				if (0 != fcon->content_remaining &&
				    NULL == (buf = read_chunk(fcon, fcon->content_remaining + fcon->padding_remaining))) goto handle_error;
				if (buf) fcon->content_remaining -= buf->len;
				if (fcbs->cb_received_data) {
					fcbs->cb_received_data(fcon, buf);
				} else {
					g_string_free(buf, TRUE);
				}
				break;
			case FCGI_GET_VALUES:
				if (0 != fcon->current_header.requestID) goto error;
				if (!read_append_chunk(fcon, fcon->buffer)) goto handle_error;
				if (0 == fcon->content_remaining)
					parse_get_values(fcon);
				break;
			case FCGI_GET_VALUES_RESULT:
				goto error; /* invalid type */
				break;
			case FCGI_UNKNOWN_TYPE:
				/* we didn't send anything fancy, so this is not expected */
				goto error; /* invalid type */
			default:
				break;
			}
		}

		if (0 == fcon->content_remaining) {
			if (0 == fcon->padding_remaining) {
				fcon->headerbuf_used = 0;
			} else {
				if (NULL == (buf = read_chunk(fcon, fcon->content_remaining + fcon->padding_remaining))) goto handle_error;
				fcon->padding_remaining -= buf->len;
				if (0 == fcon->padding_remaining) {
					fcon->headerbuf_used = 0;
				}
				g_string_free(buf, TRUE);
			}
		}

	}

	return;

handle_error:
	switch (errno) {
	case EINTR:
	case EAGAIN:
#if EWOULDBLOCK != EAGAIN
	case EWOULDBLOCK:
#endif
		return; /* try again later */
	case ECONNRESET:
		break;
	default:
		ERROR("read from fd=%d failed, %s\n", fcon->fd, g_strerror(errno) );
		break;
	}

error:
	if (0 != fcon->requestID)
		fcbs->cb_request_aborted(fcon);
	fastcgi_connection_close(fcon);
}

static void fastcgi_connection_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fastcgi_connection *fcon = (fastcgi_connection*) w->data;
	UNUSED(loop);

	if (revents & EV_READ) {
		read_queue(fcon);
	}

	if (revents & EV_WRITE) {
		write_queue(fcon);
	}
}

static fastcgi_connection *fastcgi_connecion_create(fastcgi_server *fsrv, gint fd, guint id) {
	fastcgi_connection *fcon = g_slice_new0(fastcgi_connection);

	fcon->fsrv = fsrv;
	fcon->fcon_id = id;

	fcon->buffer = g_string_sized_new(0);
	fcon->parambuf = g_string_sized_new(0);
	fcon->environ = g_ptr_array_new();

	fcon->fd = fd;
	fd_init(fcon->fd);
	ev_io_init(&fcon->fd_watcher, fastcgi_connection_fd_cb, fcon->fd, EV_READ);
	fcon->fd_watcher.data = fcon;
	ev_io_start(fcon->fsrv->loop, &fcon->fd_watcher);

	return fcon;
}

static void fastcgi_connection_free(fastcgi_connection *fcon) {
	fcon->fsrv->callbacks->cb_reset_connection(fcon);

	if (fcon->fd != -1) {
		ev_io_stop(fcon->fsrv->loop, &fcon->fd_watcher);
		close(fcon->fd);
		fcon->fd = -1;
	}

	fastcgi_gstring_queue_clear(&fcon->write_queue);
	fastcgi_connection_environ_clear(fcon);
	g_ptr_array_free(fcon->environ, TRUE);
	g_string_free(fcon->buffer, TRUE);
	g_string_free(fcon->parambuf, TRUE);

	g_slice_free(fastcgi_connection, fcon);
}

void fastcgi_connection_close(fastcgi_connection *fcon) {
	fcon->closing = TRUE;
	if (fcon->fd != -1) {
		ev_io_stop(fcon->fsrv->loop, &fcon->fd_watcher);
		close(fcon->fd);
		fcon->fd = -1;
	}

	fastcgi_gstring_queue_clear(&fcon->write_queue);

	g_string_truncate(fcon->buffer, 0);
	g_string_truncate(fcon->parambuf, 0);
	fastcgi_connection_environ_clear(fcon);

	ev_prepare_start(fcon->fsrv->loop, &fcon->fsrv->closing_watcher);
}

static void fastcgi_server_fd_cb(struct ev_loop *loop, ev_io *w, int revents) {
	fastcgi_server *fsrv = (fastcgi_server*) w->data;
	fastcgi_connection *fcon;
	void (*cb_new_connection)(fastcgi_connection *fcon) = fsrv->callbacks->cb_new_connection;

	g_assert(revents & EV_READ);

	for (;;) {
		gint fd = accept(fsrv->fd, NULL, NULL);
		if (-1 == fd) {
			switch (errno) {
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
			case EINTR:
				/* we were stopped _before_ we had a connection */
			case ECONNABORTED: /* this is a FreeBSD thingy */
				/* we were stopped _after_ we had a connection */
				return;
			case EMFILE:
				if (0 == fsrv->max_connections) {
					fsrv->max_connections = fsrv->connections->len / 2;
				} else {
					fsrv->max_connections = fsrv->max_connections / 2;
				}
				ERROR("dropped connection limit to %u as we got EMFILE\n", fsrv->max_connections);
				ev_io_rem_events(loop, w, EV_READ);
				return;
			default:
				ERROR("accept failed on fd=%d with error: %s\nshutting down\n", fsrv->fd, g_strerror(errno));
				fastcgi_server_stop(fsrv);
				return;
			}
		}

		fcon = fastcgi_connecion_create(fsrv, fd, fsrv->connections->len);
		g_ptr_array_add(fsrv->connections, fcon);
		if (cb_new_connection) {
			cb_new_connection(fcon);
		}

		if (fsrv->connections->len >= fsrv->max_connections) {
			ev_io_rem_events(loop, w, EV_READ);
			return;
		}

		if (fsrv->do_shutdown) return;
	}
}

static void fastcgi_cleanup_connections(fastcgi_server *fsrv) {
	guint i;

	for (i = 0; i < fsrv->connections->len; ) {
		fastcgi_connection *fcon = g_ptr_array_index(fsrv->connections, i);
		if (fcon->closing) {
			fastcgi_connection *t_fcon;
			guint l = fsrv->connections->len-1;
			t_fcon = g_ptr_array_index(fsrv->connections, i) = g_ptr_array_index(fsrv->connections, l);
			g_ptr_array_set_size(fsrv->connections, l);
			t_fcon->fcon_id = i;
			fastcgi_connection_free(fcon);
		} else {
			i++;
		}
	}
}

static void fastcgi_closing_cb(struct ev_loop *loop, ev_prepare *w, int revents) {
	UNUSED(revents);
	ev_prepare_stop(loop, w);
	fastcgi_cleanup_connections((fastcgi_server*) w->data);
}

fastcgi_server *fastcgi_server_create(struct ev_loop *loop, gint socketfd, const fastcgi_callbacks *callbacks, guint max_connections) {
	fastcgi_server *fsrv = g_slice_new0(fastcgi_server);

	fsrv->callbacks = callbacks;

	fsrv->max_connections = max_connections;

	fsrv->connections = g_ptr_array_sized_new(fsrv->max_connections);

	fsrv->loop = loop;
	fsrv->fd = socketfd;
	fd_init(fsrv->fd);
	ev_io_init(&fsrv->fd_watcher, fastcgi_server_fd_cb, fsrv->fd, EV_READ);
	fsrv->fd_watcher.data = fsrv;
	ev_io_start(fsrv->loop, &fsrv->fd_watcher);

	ev_prepare_init(&fsrv->closing_watcher, fastcgi_closing_cb);
	fsrv->closing_watcher.data = fsrv;

	return fsrv;
}

void fastcgi_server_stop(fastcgi_server *fsrv) {
	if (fsrv->do_shutdown) return;
	fsrv->do_shutdown = TRUE;

	ev_io_stop(fsrv->loop, &fsrv->fd_watcher);
	close(fsrv->fd);
	fsrv->fd = -1;
}

void fastcgi_server_free(fastcgi_server *fsrv) {
	guint i;
	void (*cb_request_aborted)(fastcgi_connection *fcon) = fsrv->callbacks->cb_request_aborted;
	if (!fsrv->do_shutdown) fastcgi_server_stop(fsrv);
	ev_prepare_stop(fsrv->loop, &fsrv->closing_watcher);

	for (i = 0; i < fsrv->connections->len; i++) {
		fastcgi_connection *fcon = g_ptr_array_index(fsrv->connections, i);
		cb_request_aborted(fcon);
		fcon->closing = TRUE;
	}
	fastcgi_cleanup_connections(fsrv);
	g_ptr_array_free(fsrv->connections, TRUE);

	g_slice_free(fastcgi_server, fsrv);
}

void fastcgi_end_request(fastcgi_connection *fcon, gint32 appStatus, enum FCGI_ProtocolStatus status) {
	gboolean had_data = (fcon->write_queue.length > 0);

	if (0 == fcon->requestID) return;
	stream_send_end_request(&fcon->write_queue, fcon->requestID, appStatus, status);
	fcon->requestID = 0;
	if (!had_data) write_queue(fcon);
}

void fastcgi_suspend_read(fastcgi_connection *fcon) {
	fcon->read_suspended = TRUE;
	ev_io_rem_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_READ);
}

void fastcgi_resume_read(fastcgi_connection *fcon) {
	fcon->read_suspended = FALSE;
	ev_io_add_events(fcon->fsrv->loop, &fcon->fd_watcher, EV_READ);
}

void fastcgi_send_out(fastcgi_connection *fcon, GString *data) {
	gboolean had_data = (fcon->write_queue.length > 0);
	if (!data) {
		stream_send_fcgi_record(&fcon->write_queue, FCGI_STDOUT, fcon->requestID, 0);
	} else {
		stream_send_string(&fcon->write_queue, FCGI_STDOUT, fcon->requestID, data);
	}
	if (!had_data) write_queue(fcon);
}

void fastcgi_send_err(fastcgi_connection *fcon, GString *data) {
	gboolean had_data = (fcon->write_queue.length > 0);
	if (!data) {
		stream_send_fcgi_record(&fcon->write_queue, FCGI_STDERR, fcon->requestID, 0);
	} else {
		stream_send_string(&fcon->write_queue, FCGI_STDERR, fcon->requestID, data);
	}
	if (!had_data) write_queue(fcon);
}

void fastcgi_connection_environ_clear(fastcgi_connection *fcon) {
	guint i;
	for (i = 0; i < fcon->environ->len; i++) {
		gchar *s = (gchar*) g_ptr_array_index(fcon->environ, i);
		if (s) g_free(s);
	}
	g_ptr_array_set_size(fcon->environ, 0);
}

const gchar* fastcgi_connection_environ_lookup(fastcgi_connection *fcon, const gchar* key, gsize keylen) {
	guint i;
	for (i = 0; i < fcon->environ->len; i++) {
		gchar *s = (gchar*) g_ptr_array_index(fcon->environ, i);
		if (s && 0 == strncmp(s, key, keylen) && s[keylen] == '=') {
			return &s[keylen+1];
		}
	}
	return NULL;
}

void fastcgi_gstring_queue_append(fastcgi_gstring_queue *queue, GString *buf) {
	if (!buf) return;
	g_queue_push_tail(&queue->queue, buf);
	queue->length += buf->len;
}

void fastcgi_gstring_queue_clear(fastcgi_gstring_queue *queue) {
	GString *s;
	queue->length = 0;
	queue->offset = 0;
	while (NULL != (s = g_queue_pop_head(&queue->queue))) {
		g_string_free(s, TRUE);
	}
}
