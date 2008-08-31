// set these for your server, and put this file in the same directory as
//  nph-offload.c ...

// GDEBUG should be false at production time, but this lets you sanity check
//  some things before going live.
#ifndef GDEBUG
#define GDEBUG 0
#endif

// This is a list of servers that you are offloading.
#ifndef GBASESERVER
#define GBASESERVER "icculus.org"
#endif

// Time in seconds that i/o to base server should timeout in lieu of activity.
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

// end of offload_server_config.h ...

