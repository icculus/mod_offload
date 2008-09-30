// set these for your server, and put this file in the same directory as
//  nph-offload.c ...

// GDEBUG should be false at production time, but this lets you sanity check
//  some things before going live.
#ifndef GDEBUG
#define GDEBUG 0
#endif

// This is a list of servers that you are offloading.
#ifndef GBASESERVER
#define GBASESERVER "example.com"
#endif

// Time in seconds that i/o (to base server or client) should timeout in
//  lieu of activity.
#ifndef GTIMEOUT
#define GTIMEOUT 90
#endif

// This is where we'll cache files.
#ifndef GOFFLOADDIR
#define GOFFLOADDIR "/usr/local/apache/offload"
#endif

// Set GDEBUGTOFILE to write all debug info to files in GOFFLOADDIR, if
//  GDEBUG is also true. You probably want this to be true most cases.
#ifndef GDEBUGTOFILE
#define GDEBUGTOFILE 1
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

// end of offload_server_config.h ...

