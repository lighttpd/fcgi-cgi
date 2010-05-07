EXTRA_DIST=autogen.sh CMakeLists.txt config.h.cmake fcgi-cgi.1
man1_MANS=fcgi-cgi.1

AM_CFLAGS=$(GLIB_CFLAGS)
fcgi_cgi_LDADD=$(GLIB_LIBS)

bin_PROGRAMS=fcgi-cgi
fcgi_cgi_SOURCES=fastcgi.c fcgi-cgi.c
