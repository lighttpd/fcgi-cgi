# fcgi-cgi.1
EXTRA_DIST=autogen.sh CMakeLists.txt config.h.cmake
#man1_MANS=fcgi-cgi.1

AM_CFLAGS=$(GLIB_CFLAGS)
fcgi_cgi_LDADD=$(GLIB_LIBS)

bin_PROGRAMS=fcgi-cgi
fcgi_cgi_SOURCES=fastcgi.c fcgi-cgi.c
