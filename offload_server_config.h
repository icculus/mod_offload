// set these for your server, and put this file in the same directory as
//  nph-offload.c ...

// GDEBUG should be false at production time, but this lets you sanity check
//  some things before going live.
#define GDEBUG 0

// This is a list of servers that you are offloading.
#define GBASESERVER "icculus.org"

// Time in seconds that i/o to base server should timeout in lieu of activity.
#define GTIMEOUT 90

// This is where we'll cache files.
#define GOFFLOADDIR "/usr/local/apache/offload"

// Set GDEBUGTOFILE to write all debug info to files in GOFFLOADDIR, if
//  GDEBUG is also true. You probably want this to be true most cases.
#define GDEBUGTOFILE 1

// end of offload_server_config.h ...

