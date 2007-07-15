<?php

// set these for your server, and put this file in the same directory as
//  offload.php ...

// GDEBUG should be false at production time, but this lets you sanity check
//  some things before going live.
define('GDEBUG', false);

// This is a list of servers that you are offloading.
define('GBASESERVER', 'icculus.org');

// Time in seconds that i/o to base server should timeout in lieu of activity.
define('GTIMEOUT', 90);

// This is where we'll cache files.
define('GOFFLOADDIR', '/usr/local/apache/offload');

// Set GDEBUGTOFILE to write all debug info to files in GOFFLOADDIR, if
//  GDEBUG is also true. You probably want this to be true most cases.
define('GDEBUGTOFILE', true);

// Set GUSESEMAPHORE to true to use sem_acquire() for locking.
// Set GUSESEMAPHORE to false to use mkdir() for locking.
//  sem_acquire is flakey, mkdir will NOT work on NFS!
//  All bets are off on Windows either way.  :)
define('GUSESEMAPHORE', false);


// END OF CONFIG VALUES...
?>
