<?php

// This is a PHP script that handles offloading of bandwidth from a web
//  server. It's a sort of poor-man's Akamai. It doesn't need anything
//  terribly complex (Apache, PHP, a writable directory).
//
// It works like this:
//  - You have a webserver with dynamic content, and static content that
//    may change arbitrarily (i.e. - various users making changes to their
//    homepages, etc). This server is under a lot of load, mostly from
//    the static content, which tends to be big. There may be multiple virtual
//    hosts on this machine. We call this the "base" server.
//  - You have at least one other webserver that you can use to offload some
//    of the bandwidth. We call this the "offload" server.
//  - You set up an Apache module (mod_offload) on the first server.
//    mod_offload inserts itself into the request chain, and decides if a
//    given file is safe static content (real file, not a script/cgi, no
//    password, etc). In those cases, it sends a 302 redirect, pointing the
//    client to the offload server.
//  - The offload server gets a request from the redirected client. It then
//    sends an HTTP HEAD request for the file in question to the base server
//    while the client waits. It decides if it has the right file based on
//    the HEAD. If it does, it serves the cached file.
//  - If the file is out of date, or doesn't exist on the offload server, it
//    sends a regular HTTP request for it to the base server and
//    begins caching it. While caching it, it also feeds it to the client
//    that has been waiting.
//  - If another request comes in while the file is being cached, it will
//    stream what is already there from disk, and then continue to feed as
//    the rest shows up.


// !!! FIXME:  issues to work out.
//   - Could have a partial file cached if server crashes or power goes out.
//     Add a "cacher's process id" to the metadata, and have those feeding
//     from the cache decide if this process died...if so, wipe the entry and
//     recache it.
//   - Need to have a way to clean out old files. If x.zip is on the base,
//     gets cached, and then is deleted, it'll stay on the offload server
//     forever. Getting a 404 from the HEAD request will clean it out, but
//     the offload server needs to know to do that.


//
// Installation:
// You need PHP with --enable-sysvsem support. You should configure PHP to not
//  have a time limit on script execution. PHP for Windows currently doesn't
//  support sysvsem, so until someone writes me a mutex implementation, we
//  assume you'll use a Unix box for this script.
//
// You need Apache to push every web request to this script, presumably in a
//  virtual host, if not the entire server.
//
// Assuming this script was at /www/scripts/index.php, you would want to add
//  this to Apache's config:
//
//   AliasMatch ^.*$ "/www/scripts/index.php"
//
// You can offload multiple base servers with one box: set up one virtual host
//  on the offload server for each base server. This lets each base server
//  have its own cache and configuration.
//
// Then edit everything down to the "END OF CONFIG VALUES..." comment in this
//  file, and restart the server.
//
// This file is written by Ryan C. Gordon (icculus@icculus.org).



// GDEBUG should be false at production time, but this lets you sanity check
//  some things before going live.
define('GDEBUG', false);

// This is a list of servers that you are offloading.
define('GBASESERVER', 'icculus.org');

// Time in seconds that i/o to base server should timeout in lieu of activity.
define('GTIMEOUT', 90);

// This is where we'll cache files.
define('GOFFLOADDIR', '/usr/local/apache/offload');

// END OF CONFIG VALUES...



require_once 'PEAR.php';


$GVersion = '0.0.5';
$GServerString = 'offload.php version ' . $GVersion;

$Guri = $_SERVER['REQUEST_URI'];
if (strcmp($Guri{0}, '/') != 0)
   $Guri = '/' . $Guri;
$GFilePath = NULL;
$GMetaDataPath = NULL;
$GSemaphore = NULL;
$GSemaphoreOwned = 0;


function debugEcho($str)
{
    if (GDEBUG)
    {
        if (is_array($str))
            print_r($str);
        else
            print($str . "\n");
    } // if
} // debugEcho


function etagToCacheFname($etag)
{
    return(trim($etag, " \t\n\r\0\x0B\"'"));
} // etagToCacheFname


function getSemaphore()
{
    global $GSemaphore, $GSemaphoreOwned;

    debugEcho("grabbing semaphore...(owned $GSemaphoreOwned time(s).)");
    if ($GSemaphoreOwned++ > 0)
        return;

    if (!isset($GSemaphore))
    {
        debugEcho('(have to create semaphore...)');
        $GSemaphore = sem_get(0x8267bc62);  // !!! FIXME: good value?
        if ($GSemaphore === false)
            failure('503 Service Unavailable', "Couldn't allocate semaphore.");
    } // if
    
    sem_acquire($GSemaphore);
} // getSemaphore


function putSemaphore()
{
    global $GSemaphore, $GSemaphoreOwned;
    if ( ($GSemaphoreOwned == 0) || (!isset($GSemaphore)) )
        return;

    if (--$GSemaphoreOwned == 0)
        sem_release($GSemaphore);

    debugEcho("released semaphore...(now owned $GSemaphoreOwned time(s).)");
} // putSemaphore


function terminate()
{
    debugEcho('offload script is terminating...');
    while ($GSemaphoreOwned > 0)
        putSemaphore();
    exit();
} // terminate


function doHeader($str)
{
    if (!GDEBUG)
        header($str);
    debugEcho("header('$str');");
} // doHeader


function sanestrpos($haystack, $needle)
{
    $rc = strpos($haystack, $needle);
    return(($rc === false) ? -1 : $rc);
} // sanestrpos


function loadMetadata($fname)
{
    $retval = array();
    $lines = @file($fname);
    if ($lines === false)
        return($retval);

    $max = count($lines);
    for ($i = 0; $i < $max; $i += 2)
    {
        $key = trim($lines[$i]);
        $val = trim($lines[$i+1]);
        debugEcho("Loaded metadata '$key' => '$val'");
        $retval[$key] = $val;
    } // for

    debugEcho("Loaded $max metadata pair(s).");
    return($retval);
} // loadMetadata


function cachedMetadataMostRecent($metadata, $head)
{
    if (!isset($metadata['Content-Length']))
        return(false);

    if (!isset($metadata['ETag']))
        return(false);

    if (!isset($metadata['Last-Modified']))
        return(false);

    if (strcmp($metadata['Content-Length'], $head['Content-Length']) != 0)
        return(false);

    if (strcmp($metadata['ETag'], $head['ETag']) != 0)
        return(false);

    if (strcmp($metadata['Last-Modified'], $head['Last-Modified']) != 0)
        return(false);

    // !!! FIXME: see if file size != Content-Length, and if it isn't,
    // !!! FIXME:  see if X-Offload-Caching-PID still exists. If process
    // !!! FIXME:  is missing, assume transfer died and recache.

    return(true);
} // cachedMetadataMostRecent


function nukeRequestFromCache()
{
    global $GMetaDataPath, $GFilePath;
    debugEcho('Nuking request from cache...');
    getSemaphore();
    if (isset($GMetaDataPath))
        @unlink($GMetaDataPath);
    if (isset($GFilePath))
        @unlink($GFilePath);
    putSemaphore();
} // nukeRequestFromCache


function failure($httperr, $errmsg, $location = NULL)
{
    global $GServerString;

    if (strncasecmp($httperr, 'HTTP', 4) == 0)
    {
        $pos = sanestrpos($httperr, ' ');
        if ($pos >= 0)
            $httperr = substr($httperr, $pos+1);
    } // if
    $responseStr = "HTTP/1.0 $httperr";

    debugEcho('failure() called:');
    debugEcho('  ' . $httperr);
    debugEcho('  ' . $errmsg);

    doHeader($responseStr);
    doHeader('Server: ' . $GServerString);
    doHeader('Date: ' . HTTP::date());
    if (isset($location))
        doHeader('Location: ' . $location);
    doHeader('Connection: close');
    doHeader('Content-type: text/plain');
    print("$errmsg\n");
    terminate();
} // failure


function microtime_float()
{
   list($usec, $sec) = explode(" ", microtime());
   return ((float)$usec + (float)$sec);
} // microtime_float


function stopwatch($id = NULL)
{
    static $storedid = NULL;
    static $tod = NULL;

    if (!GDEBUG)
        return;

    $now = microtime_float();

    if (isset($id))
        $storedid = $id;

    if (!isset($tod))
        $tod = $now;
    else
    {
        debugEcho("Stopwatch [$storedid]: " . ($now - $tod) . ' seconds.');
        $tod = NULL;
    } // else
} // stopwatch


function debugInit()
{
    global $Guri;
    if (GDEBUG)
    {
        header('Content-type: text/plain');
        debugEcho('Offload Debug Run!');
        debugEcho('');
        debugEcho('Base server:' . GBASESERVER);
        debugEcho('You want to get ' . $Guri);
        debugEcho('Referrer string: ' . $_SERVER['HTTP_REFERER']);
        debugEcho('Timeout for HTTP HEAD request is ' . GTIMEOUT . '.');
        debugEcho('Data cache goes in "' . GOFFLOADDIR . '".');
        debugEcho('');
        debugEcho('');
    } // if
} // debugInit



// The mainline...

debugInit();

// Feed a fake robots.txt to keep webcrawlers out of the offload server.
if (strcmp($uri, "/robots.txt") == 0)
    failure('200 OK', "User-agent: *\nDisallow: /");

if (sanestrpos($Guri, '?') >= 0)
    failure('403 Forbidden', "Offload server doesn't do dynamic content.");

if ((strcasecmp($_SERVER['REQUEST_METHOD'], 'GET') != 0) &&
    (strcasecmp($_SERVER['REQUEST_METHOD'], 'HEAD') != 0))
    failure('403 Forbidden', "Offload server doesn't do dynamic content.");

$origurl = 'http://' . GBASESERVER . $Guri;
stopwatch('HEAD transaction');
$head = HTTP::head($origurl, GTIMEOUT);
stopwatch();
if (PEAR::isError($head))
    failure('503 Service Unavailable', 'Error: ' . $head->getMessage());

debugEcho("The HTTP HEAD from $baseserver ...");
debugEcho($head);

if (($head['response_code'] == 401) || (isset($head['WWW-Authenticate'])))
    failure('403 Forbidden', "Offload server doesn't do protected content.");

else if ($head['response_code'] != 200)
    failure($head['response'], $head['response'], $head['Location']);

if ( (!isset($head['ETag'])) ||
     (!isset($head['Content-Length'])) ||
     (!isset($head['Last-Modified'])) )
{
    failure('403 Forbidden', "Offload server doesn't do dynamic content.");
} // if

// !!! FIXME: Check Cache-Control, Pragma no-cache

$cacheio = NULL;  // will be non-NULL if we're WRITING to the cache...
$frombaseserver = false;
$io = NULL;  // read from this. May be file or HTTP connection.

// HTTP HEAD requests for PHP scripts otherwise run fully and throw away the
//  results: http://www.figby.com/archives/2004/06/01/2004-06-01-php/
$ishead = (strcasecmp($_SERVER['REQUEST_METHOD'], 'HEAD') == 0);

$etagfname = etagToCacheFname($head['ETag']);
$GFilePath = GOFFLOADDIR . '/filedata-' . $etagfname;
$GMetaDataPath = GOFFLOADDIR . '/metadata-' . $etagfname;
$head['X-Offload-Orig-URL'] = $Guri;

getSemaphore();

$metadata = loadMetadata($GMetaDataPath);
if (cachedMetadataMostRecent($metadata, $head))
{
    $io = @fopen($GFilePath, 'rb');
    if ($io === false)
        failure('500 Internal Server Error', "Couldn't access cached data.");
    debugEcho('File is cached.');
} // if

else if (!$ishead)
{
    // we need to pull a new copy from the base server...

    ignore_user_abort(true);  // if we're caching, we MUST run to completion!

    $frombaseserver = true;
    $io = @fopen($origurl, 'rb');  // !!! FIXME: may block, don't hold semaphore here!
    if ($io === false)
        failure('503 Service Unavailable', "Couldn't stream file to cache.");

    $cacheio = @fopen($GFilePath, 'wb');
    if ($cacheio === false)
    {
        fclose($io);
        failure('500 Internal Server Error', "Couldn't update cached data.");
    } // if

    $metaout = @fopen($GMetaDataPath, 'wb');
    if ($metaout === false)
    {
        fclose($cacheio);
        fclose($io);
        nukeRequestFromCache();
        failure('500 Internal Server Error', "Couldn't update metadata.");
    } // if

    // !!! FIXME: This is a race condition...may change between HEAD
    // !!! FIXME:  request and actual HTTP grab. We should really
    // !!! FIXME:  just use this for comparison once, and if we are
    // !!! FIXME:  recaching, throw this out and use the headers from the
    // !!! FIXME:  actual HTTP grab when really updating the metadata.
    //
    // !!! FIXME: Also, write to temp file and rename in case of write failure!
    if (!isset($head['Content-Type']))  // make sure this is sane.
        $head['Content-Type'] = 'application/octet-stream';

    $head['X-Offload-Caching-PID'] = getmypid();

    foreach ($head as $key => $val)
        fputs($metaout, $key . "\n" . $val . "\n");
    fclose($metaout);
    $metadata = $head;
    debugEcho('Cache needs refresh...pulling from base server...');
} // else

putSemaphore();

// !!! FIXME: partial content:
// client...
//  Range: bytes=347776-
// server...
//  HTTP/1.1 206 Partial Content
//  Accept-Ranges: bytes
//  Content-Length: 239225299
//  Content-Range: bytes 347776-239573074/239573075

$max = $metadata['Content-Length'];
doHeader('HTTP/1.1 200 OK');
doHeader('Date: ' . HTTP::date());
doHeader('Server: ' . $GServerString);
doHeader('Connection: close');
doHeader('ETag: ' . $metadata['ETag']);
doHeader('Last-Modified: ' . $metadata['Last-Modified']);
doHeader('Content-Length: ' . $max);
doHeader('Content-Type: ' . $metadata['Content-Type']);

if ($ishead)
    terminate();

$br = 0;

while (true)
{
    if ($frombaseserver == false)
    {
        $stat = @fstat($io);
        if ($stat === false)
            break;

        $cursize = $stat['size'];
        if ($cursize < $max)
        {
            if (($cursize - $br) <= 8192)  // may be caching on another process.
            {
                sleep(1);
                continue;
            } // if
        } // if
    } // if

    $data = @fread($io, 8192);
    if (isset($cacheio))
    {
        fwrite($cacheio, $data);  // !!! FIXME: check for errors!
        fflush($cacheio);
    } // if

    $br += strlen($data);

    if (!connection_aborted())
    {
        debugEcho('Would have written ' . strlen($data) . ' bytes.');
        if (!GDEBUG)
            print($data);
    } // if

    if ($br >= $max)
        break;

    if (feof($io))
        break;
} // while

debugEcho('Transfer is complete.');

if ($br != $max)
{
    debugEcho("Bogus transfer! Sent $br, wanted to send $max!");
    nukeRequestFromCache();
} // if

terminate();

// end of offload script ...





// This is HTTP from PEAR. Copied here for my convenience.
//  I trimmed some stuff out and hacked on some other code.
//    --ryan.
class HTTP
{
    function Date($time = null)
    {
        if (!isset($time)) {
            $time = time();
        } elseif (!is_numeric($time) && (-1 === $time = strtotime($time))) {
            return(false);
        }
        
        // RFC822 or RFC850
        $format = ini_get('y2k_compliance') ? 'D, d M Y' : 'l, d-M-y';
        
        return gmdate($format .' H:i:s \G\M\T', $time);
    }

    function negotiateLanguage($supported, $default = 'en-US')
    {
        $supp = array();
        foreach ($supported as $lang => $isSupported) {
            if ($isSupported) {
                $supp[strToLower($lang)] = $lang;
            }
        }
        
        if (!count($supp)) {
            return $default;
        }

        $matches = array();
        if (isset($_SERVER['HTTP_ACCEPT_LANGUAGE'])) {
            foreach (explode(',', $_SERVER['HTTP_ACCEPT_LANGUAGE']) as $lang) {
                $lang = array_map('trim', explode(';', $lang));
                if (isset($lang[1])) {
                    $l = strtolower($lang[0]);
                    $q = (float) str_replace('q=', '', $lang[1]);
                } else {
                    $l = strtolower($lang[0]);
                    $q = null;
                }
                if (isset($supp[$l])) {
                    $matches[$l] = isset($q) ? $q : 1000 - count($matches);
                }
            }
        }

        if (count($matches)) {
            asort($matches, SORT_NUMERIC);
            return $supp[array_pop(array_keys($matches))];
        }
        
        if (isset($_SERVER['REMOTE_HOST'])) {
            $lang = strtolower(array_pop(explode('.', $_SERVER['REMOTE_HOST'])));
            if (isset($supp[$lang])) {
                return $supp[$lang];
            }
        }

        return $default;
    }

    function head($url, $timeout = 10)
    {
        $p = parse_url($url);
        if (!isset($p['scheme'])) {
            $p = parse_url(HTTP::absoluteURI($url));
        } elseif ($p['scheme'] != 'http') {
            return HTTP::raiseError('Unsupported protocol: '. $p['scheme']);
        }

        $port = isset($p['port']) ? $p['port'] : 80;

        //debugEcho(array($p['host'], $port, $eno, $estr, $timeout));
        if (!$fp = @fsockopen($p['host'], $port, $eno, $estr, $timeout)) {
            if ($eno == 0) {  // dns lookup failure seems to trigger this. --ryan.
                sleep(3);
                if (!$fp = @fsockopen($p['host'], $port, $eno, $estr, $timeout)) {
                    return HTTP::raiseError("Connection error: $estr ($eno)");
                }
            }
        }

        $path  = !empty($p['path']) ? $p['path'] : '/';
        $path .= !empty($p['query']) ? '?' . $p['query'] : '';

        fputs($fp, "HEAD $path HTTP/1.0\r\n");
        fputs($fp, 'Host: ' . $p['host'] . ':' . $port . "\r\n");
        fputs($fp, "Connection: close\r\n\r\n");

        $response = rtrim(fgets($fp, 4096));
        if (preg_match("|^HTTP/[^\s]*\s(.*?)\s|", $response, $status)) {
            $headers['response_code'] = $status[1];
        }
        $headers['response'] = $response;

        while ($line = fgets($fp, 4096)) {
            if (!trim($line)) {
                break;
            }
            if (($pos = strpos($line, ':')) !== false) {
                $header = substr($line, 0, $pos);
                $value  = trim(substr($line, $pos + 1));
                $headers[$header] = $value;
            }
        }
        fclose($fp);
        return $headers;
    }

    function absoluteURI($url = null, $protocol = null, $port = null)
    {
        // filter CR/LF
        $url = str_replace(array("\r", "\n"), ' ', $url);
        
        // Mess around with already absolute URIs
        if (preg_match('!^([a-z0-9]+)://!i', $url)) {
            if (empty($protocol) && empty($port)) {
                return $url;
            }
            if (!empty($protocol)) {
                $url = $protocol .':'. array_pop(explode(':', $url, 2));
            }
            if (!empty($port)) {
                $url = preg_replace('!^(([a-z0-9]+)://[^/:]+)(:[\d]+)?!i', 
                    '\1:'. $port, $url);
            }
            return $url;
        }
            
        $host = 'localhost';
        if (!empty($_SERVER['HTTP_HOST'])) {
            list($host) = explode(':', $_SERVER['HTTP_HOST']);
        } elseif (!empty($_SERVER['SERVER_NAME'])) {
            list($host) = explode(':', $_SERVER['SERVER_NAME']);
        }

        if (empty($protocol)) {
            if (isset($_SERVER['HTTPS']) && !strcasecmp($_SERVER['HTTPS'], 'on')) {
                $protocol = 'https';
            } else {
                $protocol = 'http';
            }
            if (!isset($port) || $port != intval($port)) {
                $port = isset($_SERVER['SERVER_PORT']) ? $_SERVER['SERVER_PORT'] : 80;
            }
        }
        
        if ($protocol == 'http' && $port == 80) {
            unset($port);
        }
        if ($protocol == 'https' && $port == 443) {
            unset($port);
        }

        $server = $protocol .'://'. $host . (isset($port) ? ':'. $port : '');
        
        if (!strlen($url)) {
            $url = isset($_SERVER['REQUEST_URI']) ? 
                $_SERVER['REQUEST_URI'] : $_SERVER['PHP_SELF'];
        }
        
        if ($url{0} == '/') {
            return $server . $url;
        }
        
        // Check for PATH_INFO
        if (isset($_SERVER['PATH_INFO']) && strlen($_SERVER['PATH_INFO']) && 
                $_SERVER['PHP_SELF'] != $_SERVER['PATH_INFO']) {
            $path = dirname(substr($_SERVER['PHP_SELF'], 0, -strlen($_SERVER['PATH_INFO'])));
        } else {
            $path = dirname($_SERVER['PHP_SELF']);
        }
        
        if (substr($path = strtr($path, '\\', '/'), -1) != '/') {
            $path .= '/';
        }
        
        return $server . $path . $url;
    }

    function raiseError($error = null, $code = null)
    {
        require_once 'PEAR.php';
        return PEAR::raiseError($error, $code);
    }
}
// end HTTP class.


?>