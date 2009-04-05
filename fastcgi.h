#ifndef _FCGI_CGI_FASTCGI_H
#define _FCGI_CGI_FASTCGI_H

/* NO multiplexing support */
/* Keep fastcgi.* independent */

#include <glib.h>
#include <ev.h>

/* FastCGI constants */
#	define FCGI_VERSION_1           1
#	define FCGI_HEADER_LEN  8

	enum FCGI_Type {
		FCGI_BEGIN_REQUEST     = 1,
		FCGI_ABORT_REQUEST     = 2,
		FCGI_END_REQUEST       = 3,
		FCGI_PARAMS            = 4,
		FCGI_STDIN             = 5,
		FCGI_STDOUT            = 6,
		FCGI_STDERR            = 7,
		FCGI_DATA              = 8,
		FCGI_GET_VALUES        = 9,
		FCGI_GET_VALUES_RESULT = 10,
		FCGI_UNKNOWN_TYPE      = 11
	};
#	define FCGI_MAXTYPE (FCGI_UNKNOWN_TYPE)

	enum FCGI_Flags {
		FCGI_KEEP_CONN  = 1
	};

	enum FCGI_Role {
		FCGI_RESPONDER  = 1,
		FCGI_AUTHORIZER = 2,
		FCGI_FILTER     = 3
	};

	enum FCGI_ProtocolStatus {
		FCGI_REQUEST_COMPLETE = 0,
		FCGI_CANT_MPX_CONN    = 1,
		FCGI_OVERLOADED       = 2,
		FCGI_UNKNOWN_ROLE     = 3
	};

#define FASTCGI_MAX_KEYLEN 1024
#define FASTCGI_MAX_VALUELEN 64*1024
/* end FastCGI constants */

struct fastcgi_server;
typedef struct fastcgi_server fastcgi_server;

struct fastcgi_callbacks;
typedef struct fastcgi_callbacks fastcgi_callbacks;

struct fastcgi_connection;
typedef struct fastcgi_connection fastcgi_connection;

struct fastcgi_queue;
typedef struct fastcgi_queue fastcgi_queue;

struct fastcgi_server {
/* custom user data */
	gpointer data;

/* private data */
	gboolean do_shutdown;

	const fastcgi_callbacks *callbacks;

	guint max_connections;
	GPtrArray *connections;
	guint cur_requests;

	gint fd;
	struct ev_loop *loop;
	ev_io fd_watcher;
	ev_prepare closing_watcher;
};

struct fastcgi_callbacks {
	void (*cb_new_connection)(fastcgi_connection *fcon); /* new connection accepted */
	void (*cb_new_request)(fastcgi_connection *fcon); /* new request on connection, env/params are ready */
	void (*cb_wrote_data)(fastcgi_connection *fcon);
	void (*cb_received_stdin)(fastcgi_connection *fcon, GString *data); /* data == NULL => eof */
	void (*cb_received_data)(fastcgi_connection *fcon, GString *data); /* data == NULL => eof */
	void (*cb_request_aborted)(fastcgi_connection *fcon);
	void (*cb_reset_connection)(fastcgi_connection *fcon); /* cleanup custom data before fcon is freed, not for keep-alive */
};

struct fastcgi_queue {
	GQueue queue;
	gsize offset; /* offset in first chunk */
	gsize length;
	gboolean closed;
};

struct fastcgi_connection {
/* custom user data */
	gpointer data;

/* read/write */
	GPtrArray *environ;

/* read only */
	fastcgi_server *fsrv;
	guint fcon_id; /* index in server con array */
	gboolean closing; /* "dead" connection */

	/* current request */
	guint16 requestID;
	guint16 role;
	guint8 flags;

/* private data */
	unsigned char headerbuf[8];
	guint headerbuf_used;
	gboolean first;

	struct {
		guint8 version;
		guint8 type;
		guint16 requestID;
		guint16 contentLength;
		guint8 paddingLength;
	} current_header;

	guint content_remaining, padding_remaining;

	GString *buffer, *parambuf;

	gint fd;
	ev_io fd_watcher;

	gboolean read_suspended;

	/* write queue */
	fastcgi_queue write_queue;
};

fastcgi_server *fastcgi_server_create(struct ev_loop *loop, gint socketfd, const fastcgi_callbacks *callbacks, guint max_connections);
void fastcgi_server_stop(fastcgi_server *fsrv); /* stop accepting new connections, closes listening socket */
void fastcgi_server_free(fastcgi_server *fsrv);

void fastcgi_suspend_read(fastcgi_connection *fcon);
void fastcgi_resume_read(fastcgi_connection *fcon);

void fastcgi_end_request(fastcgi_connection *fcon, gint32 appStatus, enum FCGI_ProtocolStatus status);
void fastcgi_send_out(fastcgi_connection *fcon, GString *data);
void fastcgi_send_err(fastcgi_connection *fcon, GString *data);

void fastcgi_connection_close(fastcgi_connection *fcon); /* shouldn't be needed */

void fastcgi_connection_environ_clear(fastcgi_connection *fcon);
const gchar* fastcgi_connection_environ_lookup(fastcgi_connection *fcon, const gchar* key, gsize keylen);

void fastcgi_queue_append_string(fastcgi_queue *queue, GString *buf);
void fastcgi_queue_append_bytearray(fastcgi_queue *queue, GByteArray *buf);
void fastcgi_queue_clear(fastcgi_queue *queue);

/* return values: 0 ok, -1 error, -2 con closed */
gint fastcgi_queue_write(int fd, fastcgi_queue *queue, gsize max_write);

#endif
