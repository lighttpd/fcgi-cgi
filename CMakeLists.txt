CMAKE_MINIMUM_REQUIRED(VERSION 2.6.0 FATAL_ERROR)

cmake_policy(VERSION 2.6.0)

INCLUDE(CheckIncludeFiles)
INCLUDE(CheckLibraryExists)
INCLUDE(FindPkgConfig)

MACRO(ADD_TARGET_PROPERTIES _target _name _properties)
  SET(_properties ${ARGV})
  LIST(REMOVE_AT _properties 0)
  LIST(REMOVE_AT _properties 0)
  GET_TARGET_PROPERTY(_old_properties ${_target} ${_name})
  #MESSAGE("adding property to ${_target} ${_name}: ${_properties}")
  IF(NOT _old_properties)
    # in case it's NOTFOUND
    SET(_old_properties)
  ELSE(NOT _old_properties)
    SET(_old_properties "${_old_properties} ")
  ENDIF(NOT _old_properties)
  SET_TARGET_PROPERTIES(${_target} PROPERTIES ${_name} "${_old_properties}${_properties}")
ENDMACRO(ADD_TARGET_PROPERTIES)

PROJECT(fcgi-cgi)
SET(PACKAGE_VERSION 0.1.5)
IF("${CMAKE_BUILD_TYPE}" STREQUAL "")
	SET(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel." FORCE)
ENDIF("${CMAKE_BUILD_TYPE}" STREQUAL "")


# libev
CHECK_INCLUDE_FILES(ev.h HAVE_EV_H)
IF(HAVE_EV_H)
	CHECK_LIBRARY_EXISTS(ev ev_loop "" HAVE_LIBEV)
	IF(HAVE_LIBEV)
		SET(EV_LIBRARIES ev)
		SET(EV_STATIC_LIBRARIES ev;m)
		CHECK_LIBRARY_EXISTS(rt clock_gettime "" NEED_RT)
		IF(NEED_RT)
			SET(EV_STATIC_LIBRARIES ${EV_STATIC_LIBRARIES} rt)
		ENDIF(NEED_RT)
	ELSE(HAVE_LIBEV)
		MESSAGE(FATAL_ERROR "Couldn't find lib ev")
	ENDIF(HAVE_LIBEV)
ELSE(HAVE_EV_H)
	MESSAGE(FATAL_ERROR "Couldn't find <ev.h>")
ENDIF(HAVE_EV_H)

# GLIB 2
pkg_check_modules (GLIB2 REQUIRED glib-2.0)
SET(GLIB_INCLUDES ${GLIB2_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS}/glib-2.0/ ${GLIB2_INCLUDE_DIRS}/glib-2.0/include/)
INCLUDE_DIRECTORIES(${GLIB_INCLUDES})

SET(MAIN_SOURCE fastcgi.c fcgi-cgi.c)

SET(PACKAGE_NAME ${CMAKE_PROJECT_NAME})
SET(PACKAGE_VERSION ${PACKAGE_VERSION})
CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake ${CMAKE_BINARY_DIR}/config.h ESCAPE_QUOTES)
ADD_DEFINITIONS(-DHAVE_CONFIG_H)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(fcgi-cgi ${MAIN_SOURCE})

ADD_TARGET_PROPERTIES(fcgi-cgi COMPILE_FLAGS "-std=gnu99 -Wall -g -Wshadow -W -pedantic -fPIC -D_GNU_SOURCE")

# libev
TARGET_LINK_LIBRARIES(fcgi-cgi "${EV_LIBRARIES}")

# GLIB 2
ADD_TARGET_PROPERTIES(fcgi-cgi LINK_FLAGS "${GLIB2_LDFLAGS}")
ADD_TARGET_PROPERTIES(fcgi-cgi COMPILE_FLAGS "${GLIB2_CFLAGS_OTHER}")

INSTALL(TARGETS fcgi-cgi DESTINATION bin)

# man page

SET(CMAKE_MAN_DIR "share/man" CACHE STRING
  "Install location for man pages (relative to prefix).")
MARK_AS_ADVANCED(CMAKE_MAN_DIR)

INSTALL(FILES fcgi-cgi.1 DESTINATION ${CMAKE_MAN_DIR}/man1)
