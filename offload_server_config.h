// set these for your server, and put this file in the same directory as
//  nph-offload.c ...

// GDEBUG should be false at production time, but this lets you sanity check
//  some things before going live.
#ifndef GDEBUG
#define GDEBUG 0
#endif

// Set GDEBUGTOFILE to write all debug info to files in GDEBUGDIR, if
//  GDEBUG is also true. You probably want this to be true most cases.
#ifndef GDEBUGTOFILE
#define GDEBUGTOFILE 1
#endif

// Ignore this if either GDEBUG or GDEBUGTOFILE are zero.
// Set GDEBUGDIR to where debug logs should be written.
#ifndef GDEBUGDIR
#define GDEBUGDIR "/usr/local/apache/logs"
#endif

// Set this to non-zero to provide a listen server that serves HTTP requests
//  directly. Set this to zero and you need to run as a cgi-bin program
//  through another webserver. Obviously, you can't listen on port 80 if
//  something else is using it.
#ifndef GLISTENPORT
#define GLISTENPORT 0
#endif

// Ignore this if GLISTENPORT == 0. This is the address to bind to. It should
//  be a string literal ("39.15.124.111" or maybe "localhost" if another
//  process is proxying). NULL is equivalent to IN_ADDRANY; bind to everything.
#ifndef GLISTENADDR
#define GLISTENADDR NULL
#endif

// Ignore this if GLISTENPORT == 0. Should probably be PF_INET or PF_INET6.
#ifndef GLISTENFAMILY
#define GLISTENFAMILY PF_INET
#endif

// Ignore this if GLISTENPORT == 0.
// Set this to a list of IP addresses from which you trust the X-Forwarded-For
//  header. This has to match this format:
//     "xxx.xxx.xxx.xxx", "yyy.yyy.yyy.yyy", [...more like that...]
// ...an array of C string literals.
// If you don't plan to serve from behind a known proxy on your LAN, you
//  can just set this to NULL.
#ifndef GLISTENTRUSTFWD
#define GLISTENTRUSTFWD "127.0.0.1", "0.0.0.0"
#endif

// Ignore this if GLISTENPORT == 0.
// Set this to non-zero to make process fork to background on startup.
#ifndef GLISTENDAEMONIZE
#define GLISTENDAEMONIZE 0
#endif

// This works everywhere, but you probably don't want it if GLISTENPORT == 0.
// Set this to non-zero to make each transaction append a line to a log file
//  in Apache Combined Log Format.
#ifndef GLOGACTIVITY
#define GLOGACTIVITY 0
#endif

// Ignore this if GLOGACTIVITY == 0.
// Set this to the filename to write log data to. Put a full path here!
#ifndef GLOGFILE
#define GLOGFILE "/usr/local/apache/logs/access.log"
#endif

// This is the server that you are offloading's hostname.
#ifndef GBASESERVER
#define GBASESERVER "example.com"
#endif

// This is the server that you are offloading's IP address.
//  We use this for DNS lookups (GBASESERVER is used for the "Host:" field in 
//  HTTP requests, etc). If you know the IP address will never change, you can
//  save the several hundred kilobytes of address space that glibc uses...it
//  loads a separate shared library for the DNS lookup, but it doesn't need 
//  to if GBASESERVERIP is an IP address in "xxx.xxx.xxx.xxx" format.
#ifndef GBASESERVERIP
#define GBASESERVERIP GBASESERVER
#endif

// This is the port on the base server to connect to (default for HTTP is 80).
#ifndef GBASESERVERPORT
#define GBASESERVERPORT 80
#endif

// Time in seconds that i/o (to base server or client) should timeout in
//  lieu of activity.
#ifndef GTIMEOUT
#define GTIMEOUT 45
#endif

// This is where we'll cache files.
#ifndef GOFFLOADDIR
#define GOFFLOADDIR "/usr/local/apache/offload"
#endif

// Set GMAXDUPEDOWNLOADS to the number of concurrent connections one IP
//  address can have for one download. This is largely meant to prevent
//  download accelerators that open multiple connections that each grab a
//  portion of the same file. Any connections over the limit are immediately
//  rejected with a 403 Forbidden response explaining the issue.
// The same IP can download different files at the same time; we count the
//  number of simultaneous accesses to the same URL from the same IP.
// Two people on two machines behind a NAT, downloading the same file, will
//  look like a dupe.
// Set this to zero to disable it.
#ifndef GMAXDUPEDOWNLOADS
#define GMAXDUPEDOWNLOADS 1
#endif

// Set to 1 to try to change title in "ps" listings. It becomes:
//   "offload: GET /my/url.whatever" (or whatever).
#ifndef GSETPROCTITLE
#define GSETPROCTITLE 1
#endif

// if you have a PowerPC, etc, flip this to 1.
#ifndef PLATFORM_BIGENDIAN
#if defined(__powerpc64__) || defined(__ppc__) || defined(__powerpc__) || defined(__POWERPC__)
#define PLATFORM_BIGENDIAN 1
#else
#define PLATFORM_BIGENDIAN 0
#endif
#endif

// Pick a unique name, letters and dashes.
//  This should be unique for each cache, or you may have odd problems. So
//  if you have two separate offload servers with their own unique caches on
//  the same machine, you want to keep them separated.
// If you just want one offload server, the default is fine.
#ifndef SHM_NAME
#define SHM_NAME "mod-offload"
#endif

// You probably should leave this alone if you don't know what you're doing.
// Set this to non-zero to disable caching. This can be useful if you just
//  want a lightweight HTTP server to handle the heavy lifting for large
//  static files. For example, you could have Apache running on port 80 to
//  handle most requests, so you get all the PHP, SSL, mod_*, .htaccess, etc
//  goodness, but Apache eats several megabytes of RAM per connection when
//  serving static files. If you have a multi-gigabyte static file, a client
//  might consume that server's RAM for several hours to complete the
//  transfer. Since most of the memory Apache consumes is irrelevant to
//  sending static files, it might make sense to offload it to a lightweight
//  process like this, with a 302 Redirect, and free up those resources for
//  dynamic content. Very quickly you find yourself saving hundreds of
//  megabytes.
// GNOCACHE changes a few rules. This process will no longer cache files, and
//  GOFFLOADDIR refers to the DocumentRoot where files are read from. We will
//  still do HTTP HEAD requests to the GBASESERVER for metadata, and to verify
//  that we have access to the file (since we do no authorization
//  ourselves). We access the file directly, instead of caching over HTTP, so
//  be careful about file permissions, URI aliases, etc. This probably is only
//  useful with GLISTENPORT != 0.
#ifndef GNOCACHE
#define GNOCACHE 0
#endif

// end of offload_server_config.h ...

