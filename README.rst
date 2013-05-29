Description
-----------

:Homepage:
    http://redmine.lighttpd.net/projects/fcgi-cgi/wiki

fcgi-cgi is a FastCGI application to run normal cgi applications. It doesn't
make CGI applications faster, but it allows you to run them on a different
host and with different user permissions (without the need for suexec).

lighttpd2 won't have a mod_cgi, so you need this FastCGI wrapper to be able
to execute standard cgi applications like mailman and cgit.

fcgi-cgi is released under the `MIT license <http://git.lighttpd.net/fcgi-cgi.cgi/tree/COPYING>`_

Usage
-----

Examples for spawning a fcg-cgi instance with daemontools or runit::

  #!/bin/sh
  # run script

  exec spawn-fcgi -n -s /tmp/fastcgi-cgi.sock -u www-default -U www-data -- /usr/bin/fcgi-cgi


Build dependencies
------------------

* glib >= 2.16.0 (http://www.gtk.org/)
* libev (http://software.schmorp.de/pkg/libev.html)
* cmake or autotools (for snapshots/releases the autotool generated files are included)


Build
-----

* snapshot/release with autotools::

   ./configure
   make

* build from git: ``git clone git://git.lighttpd.net/fcgi-cgi.git``

 * with autotools::

    ./autogen.sh
    ./configure
    make

 * with cmake (should work with snapshots/releases too)::

    cmake .
    make
