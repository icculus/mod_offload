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
//  GDEBUG is also true. You want this to be false in normal use.
define('GDEBUGTOFILE', true);

// END OF CONFIG VALUES...
?>
