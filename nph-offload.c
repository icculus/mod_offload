// This is a C program that handles offloading of bandwidth from a web
//  server. It's a sort of poor-man's Akamai. It doesn't need anything
//  terribly complex (a webserver with cgi-bin support, a writable directory).
// It can run as a cgi-bin program, or as a quick-and-dirty standalone HTTP
//  server.
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
//   - Need to have a way to clean out old files. If x.zip is on the base,
//     gets cached, and then is deleted, it'll stay on the offload server
//     forever. Getting a 404 from the HEAD request will clean it out, but
//     the offload server needs to know to do that.


//
// Installation:
// You need a Unix-like system, like Linux, BSD, or Mac OS X. This won't
//  work on Windows (try the PHP version there, you might have some luck).
//
// If you're building this as a standalone server, set the options you want,
//  compile it and start it running.
//
// If you want to run as a cgi-bin program:
// You need Apache (or whatever) to push every web request to this program,
//  presumably in a virtual host, if not the entire server.
//
// Assuming this program was at /www/cgi-bin/index.cgi, you would want to add
//  this to Apache's config:
//
//   AliasMatch ^.*$ "/www/cgi-bin/index.cgi"
//
// You might need a "AddHandler cgi-script .cgi" or some other magic.
//
// If you don't have control over the virtual host's config file, you can't
//  use AliasMatch, but if you can put an .htaccess file in the root of the
//  virtual host, you can get away with this:
//
//   ErrorDocument 404 /cgi-bin/index.cgi
//
// This will make all missing files (everything) run the script, which will
//  then cache and distribute the correct content, including overriding the
//  404 status code with the correct one. Be careful about files that DO exist
//  in that vhost directory, though. They won't offload.
//
// In this case, Apache will report the correct status message to the client,
//  but log all offloaded files as 404 Not Found. This can't be helped. Run
//  the server as standalone on a different port and have Apache proxy to it,
//  or don't use Apache at all.
//
// You can offload multiple base servers with one box: set up one virtual host
//  on the offload server for each base server. This lets each base server
//  have its own cache and configuration.
//
// Restart the server so the AliasMatch configuration tweak is picked up.
//
//
// This file is written by Ryan C. Gordon (icculus@icculus.org).

/*
 * Building:
 *
 *  Edit offload_server_config.h to fit your needs, or override #defines
 *  on the command line. I use a shell script that looks like this:
 *
 *    #!/bin/sh
 *
 *    exec gcc \
 *    -DGDEBUG=0 \
 *    -DSHM_NAME='"mod-offload-offload2-icculus-org"' \
 *    -DGBASESERVER='"icculus.org"' \
 *    -DGLISTENPORT=9090 \
 *    -DGLISTENDAEMONIZE=1 \
 *    -DGLISTENTRUSTFWD='"127.0.0.1", "66.33.209.154"' \
 *    -DGOFFLOADDIR='"/home/icculus/offload2.icculus.org/offload-cache--offload2.icculus.org"' \
 *    -DGMAXDUPEDOWNLOADS=1 \
 *    -DGLOGACTIVITY=1 \
 *    -DGLOGFILE='"/home/icculus/logs/offload2.icculus.org/http/access.log"' \
 *    -g -O0 -Wall -o offload-daemon /home/icculus/mod_offload/nph-offload.c -lrt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define GVERSION "1.1.4"
#define GSERVERSTRING "nph-offload.c/" GVERSION

#include "offload_server_config.h"

#define OFFLOAD_NUMSTR2(x) #x
#define OFFLOAD_NUMSTR(x) OFFLOAD_NUMSTR2(x)

#define GBASESERVERPORTSTR OFFLOAD_NUMSTR(GBASESERVERPORT)

#ifdef __GNUC__
#define ISPRINTF(x,y) __attribute__((format (printf, x, y)))
#else
#define ISPRINTF(x,y)
#endif

#ifdef max
#undef max
#endif

// some getaddrinfo() flags that may not exist...
#ifndef AI_ALL
#define AI_ALL 0
#endif
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif
#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif
#ifndef AI_V4MAPPED
#define AI_V4MAPPED 0
#endif

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;

extern char **environ;

static int GIsCacheProcess = 0;
static int GHttpStatus = 0;
static int64 GBytesSent = 0;
static const char *Guri = NULL;
static const char *GRemoteAddr = NULL;
static const char *GReferer = NULL;
static const char *GUserAgent = NULL;
static const char *GReqVersion = NULL;
static const char *GReqMethod = NULL;
static char *GFilePath = NULL;
static char *GMetaDataPath = NULL;
static void *GSemaphore = NULL;
static int GSemaphoreOwned = 0;
static FILE *GDebugFilePointer = NULL;

static void failure_location(const char *, const char *, const char *);
static inline void failure(const char *httperr, const char *errmsg)
{
    failure_location(httperr, errmsg, NULL);
} // failure


#if ( ((GDEBUG) && (GDEBUGTOFILE)) == 0 )
#define getDebugFilePointer() (NULL)
#else
static FILE *getDebugFilePointer(void)
{
    if (GDebugFilePointer == NULL)
    {
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), GDEBUGDIR "/debug-%d", (int) getpid());
        GDebugFilePointer = fopen(buf, "a");
    } // if
    return GDebugFilePointer;
} // getDebugFilePointer
#endif


#if ((!GDEBUG) && defined(__GNUC__))
#define debugEcho(fmt, ...) do {} while (0)
#else
static void debugEcho(const char *fmt, ...) ISPRINTF(1, 2);
static void debugEcho(const char *fmt, ...)
{
    #if GDEBUG
        #if !GDEBUGTOFILE
        FILE *fp = stdout;
        #else
        FILE *fp = getDebugFilePointer();
        #endif
        if (fp != NULL)
        {
            if (GIsCacheProcess)
                fputs("(cache process) ", fp);

            va_list ap;
            va_start(ap, fmt);
            vfprintf(fp, fmt, ap);
            va_end(ap);
            fputs("\n", fp);
            fflush(fp);
        } // else
    #endif
} // debugEcho
#endif


static void *createSemaphore(const int initialVal)
{
    void *retval = NULL;
    const int value = initialVal ? 0 : 1;
    int created = 1;

    retval = sem_open("SEM-" SHM_NAME, O_CREAT | O_EXCL, 0600, value);
    if ((retval == (void *) SEM_FAILED) && (errno == EEXIST))
    {
        created = 0;
        debugEcho("(semaphore already exists, just opening existing one.)");
        retval = sem_open("SEM-" SHM_NAME, 0);
    } // if

    if (retval == (void *) SEM_FAILED)
        return NULL;

    return retval;
} // createSemaphore


static void getSemaphore(void)
{
    debugEcho("grabbing semaphore...(owned %d time(s).)", GSemaphoreOwned);
    if (GSemaphoreOwned++ > 0)
        return;

    if (GSemaphore != NULL)
    {
        if (sem_wait(GSemaphore) == -1)
            failure("503 Service Unavailable", "Couldn't lock semaphore.");
    } // if
    else
    {
        debugEcho("(have to create semaphore...)");
        GSemaphore = createSemaphore(0);
        if (GSemaphore == NULL)
            failure("503 Service Unavailable", "Couldn't allocate semaphore.");
    } // else
} // getSemaphore


static void putSemaphore(void)
{
    if (GSemaphoreOwned == 0)
        return;

    if (--GSemaphoreOwned == 0)
    {
        if (GSemaphore != NULL)
        {
            if (sem_post(GSemaphore) == -1)
                failure("503 Service Unavailable", "Couldn't unlock semaphore.");
        } // if
    } // if
    debugEcho("released semaphore...(now owned %d time(s).)", GSemaphoreOwned);
} // putSemaphore


static inline int process_dead(const pid_t pid)
{
    return ( (pid <= 0) || ((kill(pid, 0) == -1) && (errno == ESRCH)) );
} // process_dead


#if GMAXDUPEDOWNLOADS <= 0
#define setDownloadRecord()
#define removeDownloadRecord()
#else

// we can track this many concurrent connections in a block of shared memory.
//  If DownloadRecord is 24 bytes, then 512 records is 12 kilobytes (usually,
//  three pages of memory). If you actually have more than this many concurrent
//  connections then we'll just stop checking for dupes in things that didn't
//  fit in the table...frankly, if your server is still standing with 512
//  active HTTP downloads, you probably don't care about download accelerators
//  anyhow.   :)
#define MAX_DOWNLOAD_RECORDS 512

typedef struct
{
    pid_t pid;
    uint8 sha1[20];
} DownloadRecord;

static DownloadRecord *GAllDownloads = NULL;
static DownloadRecord *GMyDownload = NULL;

#define DUPE_FORBID_TEXT \
    "403 Forbidden - " GSERVERSTRING "\n\n" \
    "Your network address has too many connections for this specific file.\n" \
    "Please disable any 'download accelerators' and try again.\n\n" \

typedef struct
{
    uint32 state[5];
    uint32 count[2];
    uint8 buffer[64];
} Sha1;

static void Sha1_init(Sha1 *context);
static void Sha1_append(Sha1 *context, const uint8 *data, uint32 len);
static void Sha1_finish(Sha1 *context, uint8 digest[20]);

static void setDownloadRecord()
{
    const pid_t mypid = getpid();
    int dupes = 0;
    int i = 0;
    int fd = -1;
    Sha1 sha1data;
    uint8 sha1[20];
    DownloadRecord *downloads = NULL;
    const size_t maplen = sizeof (DownloadRecord) * MAX_DOWNLOAD_RECORDS;
    if (GRemoteAddr == NULL)
        return;  // oh well.

    GAllDownloads = GMyDownload = NULL;

    getSemaphore();

    fd = shm_open("/" SHM_NAME, (O_CREAT|O_EXCL|O_RDWR), (S_IREAD|S_IWRITE));
    if (fd < 0)
    {
        fd = shm_open("/" SHM_NAME, (O_CREAT|O_RDWR),(S_IREAD|S_IWRITE));
        if (fd < 0)
        {
            putSemaphore();
            debugEcho("shm_open() failed: %s", strerror(errno));
            return;  // oh well.
        } // if
    } // if

    ftruncate(fd, maplen);

    void *ptr = mmap(0, maplen, (PROT_READ|PROT_WRITE), MAP_SHARED, fd, 0);
    close(fd);  // mapping remains.
    if (ptr == MAP_FAILED)
    {
        putSemaphore();
        debugEcho("mmap() failed: %s", strerror(errno));
        return;
    } // if

    GAllDownloads = downloads = (DownloadRecord *) ptr;

    Sha1_init(&sha1data);
    Sha1_append(&sha1data, (const uint8 *) GRemoteAddr, strlen(GRemoteAddr) + 1);
    Sha1_append(&sha1data, (const uint8 *) Guri, strlen(Guri) + 1);
    Sha1_finish(&sha1data, sha1);

    for (i = 0; i < MAX_DOWNLOAD_RECORDS; i++, downloads++)
    {
        const pid_t pid = downloads->pid;

        if (pid <= 0)  // unused slot.
            GMyDownload = downloads;  // take slot.

        else if (memcmp(downloads->sha1, sha1, sizeof (sha1)) == 0)
        {
            // make sure this isn't a killed process.
            if ( (pid == mypid) || (process_dead(pid)) )
            {
                debugEcho("pid #%d died at some point.", (int) pid);
                downloads->pid = 0;
                GMyDownload = downloads;   // take slot.
            } // if
            else
            {
                debugEcho("pid #%d still alive, dupe slot.", (int) pid);
                dupes++;
            } // else
        } // else if
    } // for

    debugEcho("Saw %d dupes.", dupes);

    if (dupes >= GMAXDUPEDOWNLOADS)
        failure("403 Forbidden", DUPE_FORBID_TEXT);  // will put semaphore.
    else if (GMyDownload == NULL)    // Have fun, downloader accelerator!
        debugEcho("no free download slots! Can't add ourselves.");
    else
    {
        debugEcho("Got download slot #%d", (int) (GMyDownload-GAllDownloads));
        GMyDownload->pid = mypid;
        memcpy(GMyDownload->sha1, sha1, sizeof (sha1));
    } // else

    putSemaphore();
} // setDownloadRecord


static void removeDownloadRecord()
{
    if (!GAllDownloads)
        return;

    getSemaphore();
    if (GMyDownload != NULL)
        GMyDownload->pid = 0;
    putSemaphore();
    munmap(GAllDownloads, sizeof (DownloadRecord) * MAX_DOWNLOAD_RECORDS);

    GAllDownloads = GMyDownload = NULL;
} // removeDownloadRecord
#endif

// strftime()'s "%a" gives you locale-dependent strings...
static const char *GWeekday[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

// strftime()'s "%b" gives you locale-dependent strings...
static const char *GMonth[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static void printf_date_header(FILE *out)
{
    if (out == NULL)
        return;

    time_t now = time(NULL);
    const struct tm *tm = gmtime(&now);
    fprintf(out, "Date: %s, %02d %s %d %02d:%02d:%02d GMT\r\n",
             GWeekday[tm->tm_wday], tm->tm_mday, GMonth[tm->tm_mon],
             tm->tm_year+1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
} // printf_date_header


static int64 atoi64(const char *str)
{
    int64 retval = 0;
    int64 mult = 1;
    int i = 0;

    while (*str == ' ')
        str++;

    if (*str == '-')
    {
        mult = -1;
        str++;
    } // if

    while (1)
    {
        const char ch = str[i];
        if ((ch < '0') || (ch > '9'))
            break;
        i++;
    } // for

    while (--i >= 0)
    {
        const char ch = str[i];
        retval += ((int64) (ch - '0')) * mult;
        mult *= 10;
    } // while

    return retval;
} // atoi64


static const char *makeNum(int64 num)
{
    static char buf[64];
    snprintf(buf, sizeof (buf), "%lld", (long long) num);
    return buf;
} // makeNum


static void *xmalloc(const size_t len)
{
    void *ptr = malloc(len);
    if (ptr == NULL)
        failure("500 Internal Server Error", "Out of memory.");
    return ptr;
} // xmalloc

static char *xstrdup(const char *str)
{
    char *ptr = (char *) xmalloc(strlen(str) + 1);
    strcpy(ptr, str);
    return ptr;
} // xstrdup


static char *makeStr(const char *fmt, ...) ISPRINTF(1, 2);
static char *makeStr(const char *fmt, ...)
{
    va_list ap;

    char ch;
    va_start(ap, fmt);
    const int len = vsnprintf(&ch, 1, fmt, ap);
    va_end(ap);

    char *retval = (char *) xmalloc(len + 1);
    va_start(ap, fmt);
    vsnprintf(retval, len + 1, fmt, ap);
    va_end(ap);

    return retval;
} // makeStr


// a hashtable would be more sane, but really, we're talking about a handful
//  of items, so this is probably the lower memory option, and it's fast
//  enough for the simplicity.
typedef struct list
{
    const char *key;
    const char *value;
    struct list *next;
} list;

static const char *listSet(list **l, const char *key, const char *value)
{
    // maybe substring of current item, so copy it before we free() anything.
    const char *newvalue = xstrdup(value);

    list *item = *l;
    while (item)
    {
        if (strcmp(item->key, key) == 0)
            break;
        item = item->next;
    } // while

    if (item != NULL)
        free((void *) item->value);
    else
    {
        item = (list *) xmalloc(sizeof (list));
        item->key = xstrdup(key);
        item->next = *l;
        *l = item;
    } // else

    item->value = newvalue;
    return newvalue;
} // listSet


static const char *listFind(const list *l, const char *key)
{
    const list *item = l;
    while (item)
    {
        if (strcmp(item->key, key) == 0)
            break;
        item = item->next;
    } // while
    return item ? item->value : NULL;
} // listFind


static void listFree(list **l)
{
    list *item = *l;
    while (item)
    {
        list *next = item->next;
        free((void *) item->key);
        free((void *) item->value);
        free(item);
        item = next;
    } // while

    *l = NULL;
} // listFree


#if GSETPROCTITLE
    #if defined(__linux__)
        // okay.
    #else
        #warning GSETPROCTITLE not currently supported on this platform.
        #undef GSETPROCTITLE
        #define GSETPROCTITLE 0
    #endif
#endif

#if !GSETPROCTITLE
#define copyEnv(x) getenv(x)
#define freeEnvCopies()
#else
static char **GArgv = NULL;
static char *GLastArgv = NULL;
static int GMaxArgvLen = 0;
static int GNoMoreGetEnv = 0;
static list *GEnvCopies = NULL;
static const char *copyEnv(const char *key)
{
    const char *retval = listFind(GEnvCopies, key);
    if ((retval == NULL) && (!GNoMoreGetEnv))
    {
        const char *envr = getenv(key);
        if (envr != NULL)
            retval = listSet(&GEnvCopies, key, envr);
    } // if
    return retval;
} // copyEnv

static inline void freeEnvCopies(void)
{
    listFree(&GEnvCopies);
} // freeEnvCopies
#endif


#if !GLOGACTIVITY
#define outputLogEntry()
#else
static void outputLogEntry(void)
{
    FILE *out = fopen(GLOGFILE, "a");
    if (out == NULL)
        debugEcho("Failed to open log file for append!");
    else
    {
        // Apache Combined Log Format:
        //  http://httpd.apache.org/docs/1.3/logs.html#combined
        // !!! FIXME: auth and identd?
        time_t now = time(NULL);
        const struct tm *tm = localtime(&now);
        fprintf(out,
            "%s - - [%02d/%s/%d:%02d:%02d:%02d %c%02d%02d]"
            " \"%s %s%s%s\" %d %lld \"%s\" \"%s\"\n",
            GRemoteAddr, tm->tm_mday, GMonth[tm->tm_mon],
            tm->tm_year+1900, tm->tm_hour, tm->tm_min,
            tm->tm_sec, (tm->tm_gmtoff < 0) ? '-' : '+',
            (int) (abs((int) tm->tm_gmtoff) / (60*60)),
            (int) (abs((int) tm->tm_gmtoff) % (60*60)),
            GReqMethod ? GReqMethod : "",
            Guri ? Guri : "",
            (GReqVersion && *GReqVersion) ? " " : "",
            GReqVersion ? GReqVersion : "",
            GHttpStatus, (long long) GBytesSent,
            GReferer ? GReferer : "-",
            GUserAgent ? GUserAgent : "-");
        fclose(out);
    } // else
} // outputLogEntry
#endif


static void terminate(void)
{
    if (!GIsCacheProcess)
    {
        debugEcho("offload program is terminating...");
        removeDownloadRecord();
        outputLogEntry();
        while (GSemaphoreOwned > 0)
            putSemaphore();
    } // if

    if (GDebugFilePointer != NULL)
        fclose(GDebugFilePointer);

    #if GLISTENPORT
    char ch = 0;
    shutdown(0, SHUT_RDWR);
    shutdown(1, SHUT_RDWR);
    while (recv(0, &ch, sizeof (ch), 0) > 0) {}
    while (recv(1, &ch, sizeof (ch), 0) > 0) {}
    #endif

    if (stdin) fclose(stdin);
    if (stdout) fclose(stdout);
    if (stderr) fclose(stderr);
    stdin = stdout = stderr = NULL;

    freeEnvCopies();

    exit(0);
} // terminate


static list *loadMetadata(const char *fname)
{
    list *retval = NULL;
    struct stat statbuf;
    int fd = open(fname, O_RDONLY);
    if (fd == -1)
        return NULL;

    if (fstat(fd, &statbuf) == -1)
    {
        close(fd);
        return NULL;
    } // if

    char *buf = (char *) xmalloc(statbuf.st_size + 1);
    if (read(fd, buf, statbuf.st_size) != statbuf.st_size)
    {
        free(buf);
        close(fd);
        return NULL;
    } // if

    buf[statbuf.st_size] = '\0';
    close(fd);

    char *ptr = buf;
    int total = 0;
    while (1)
    {
        char *key = ptr;
        ptr = strchr(ptr, '\n');
        if (ptr == NULL)
            break;
        *(ptr++) = '\0';
        char *value = ptr;
        ptr = strchr(ptr, '\n');
        if (ptr == NULL)
            break;
        *(ptr++) = '\0';
        if (*key != '\0')
            listSet(&retval, key, value);
        debugEcho("Loaded metadata '%s' => '%s'", key, value);
        total++;
    } // while

    free(buf);
    debugEcho("Loaded %d metadata pair(s).", total);

    return retval;
} // loadMetadata


static int cachedMetadataMostRecent(const list *metadata, const list *head)
{
    const char *contentlength = listFind(metadata, "Content-Length");
    if (!contentlength)
        return 0;

    const char *etag = listFind(metadata, "ETag");
    if (!etag)
        return 0;

    const char *lastmodified = listFind(metadata, "Last-Modified");
    if (!lastmodified)
        return 0;

    if (strcmp(contentlength, listFind(head, "Content-Length")) != 0)
        return 0;

    if (strcmp(etag, listFind(head, "ETag")) != 0)
        return 0;

    if (strcmp(lastmodified, listFind(head, "Last-Modified")) != 0)
    {
        const char *isweak = listFind(metadata, "X-Offload-Is-Weak");
        if ( (!isweak) || (strcmp(isweak, "0") != 0) )
            return 0;
    } // if

    // See if file size != Content-Length, and if it isn't,
    //  see if X-Offload-Caching-PID still exists. If process
    //  is missing, assume transfer died and recache.
    struct stat statbuf;
    if (stat(GFilePath, &statbuf) == -1)
        return 0;

    const int64 fsize = statbuf.st_size;
    if (fsize != atoi64(contentlength))
    {
        // whoa, we were supposed to cache this!
        const char *cacher = listFind(metadata, "X-Offload-Caching-PID");
        if (!cacher)
            return 0;

        const int cacherpid = atoi(cacher);
        if (process_dead(cacherpid))
        {
            debugEcho("Caching process ID died!");
            return 0;
        } // if
    } // if

    return 1;
} // cachedMetadataMostRecent


static void nukeRequestFromCache(void)
{
    debugEcho("Nuking request from cache...");
    getSemaphore();
    if (GMetaDataPath != NULL)
        unlink(GMetaDataPath);
    if (GFilePath != NULL)
        unlink(GFilePath);
    putSemaphore();
} // nukeRequestFromCache


static void failure_location(const char *httperr, const char *errmsg,
                             const char *location)
{
    if (strncasecmp(httperr, "HTTP", 4) == 0)
    {
        const char *ptr = strchr(httperr, ' ');
        if (ptr != NULL)
            httperr = ptr+1;
    } // if

    if (!GHttpStatus)
        GHttpStatus = atoi(httperr);

    debugEcho("failure() called:");
    debugEcho("  %s", httperr);
    debugEcho("  %s", errmsg);

    if (stdout != NULL)
    {
        printf("HTTP/1.1 %s\r\n", httperr);
        printf("Status: %s\r\n", httperr);
        printf("Server: %s\r\n", GSERVERSTRING);
        printf_date_header(stdout);
        if (location != NULL)
            printf("Location: %s\r\n", location);
        printf("Connection: close\r\n");
        printf("Content-type: text/plain; charset=utf-8\r\n");
        printf("\r\n");
        printf("%s\n\n", errmsg);
        GBytesSent += strlen(errmsg) + 2;
    } // if

    terminate();
} // failure_location


static int invalidContentRange(const int64 startRange, const int64 endRange,
                               const int64 max)
{
    if ((startRange < 0) || (startRange >= max))
        return 1;
    else if ((endRange < 0) || (endRange >= max))
        return 1;
    else if (startRange > endRange)
        return 1;
    return 0;
} // invalidContentRange


#if !GDEBUG
#define debugInit(argc, argv, envp)
#else
static void debugInit(int argc, char **argv, char **envp)
{
    #if !GDEBUGTOFILE
    printf("HTTP/1.1 200 OK\r\n");
    printf("Status: 200 OK\r\n");
    printf("Content-type: text/plain; charset=utf-8\r\n");
    printf_date_header(stdout);
    printf("Server: " GSERVERSTRING "\r\n");
    printf("Connection: close\r\n");
    printf("\r\n");
    GHttpStatus = 200;
    #endif

    debugEcho("%s", "");
    debugEcho("%s", "");
    debugEcho("%s", "");
    debugEcho("Offload Debug Run!");
    debugEcho("%s", "");
    printf_date_header(getDebugFilePointer());
    debugEcho("I am: %s", GSERVERSTRING);
    debugEcho("Base server: %s", GBASESERVER);
    debugEcho("User wants to get: %s", Guri);
    debugEcho("Request from address: %s", GRemoteAddr);
    debugEcho("Client User-Agent: %s", GUserAgent);
    debugEcho("Referrer string: %s", GReferer);
    debugEcho("Request method: %s", GReqMethod);
    debugEcho("Timeout for HTTP HEAD request is %d", GTIMEOUT);
    debugEcho("Data cache goes in %s", GOFFLOADDIR);
    debugEcho("My PID: %d\n", (int) getpid());
    debugEcho("%s", "");
    debugEcho("%s", "");

    int i;
    debugEcho("Command line: %d items...", argc);
    for (i = 0; i < argc; i++)
        debugEcho(" argv[%d] = '%s'", i, argv[i]);
    debugEcho("%s", "");
    debugEcho("%s", "");
    debugEcho("Environment...");
    for (i = 0; envp[i]; i++)
        debugEcho(" %s", envp[i]);
    debugEcho("%s", "");
    debugEcho("%s", "");
} // debugInit
#endif


static void readHeaders(const int fd, list **headers)
{
    const time_t endtime = time(NULL) + GTIMEOUT;
    int br = 0;
    char buf[1024];
    int seenresponse = 0;
    while (1)
    {
        const time_t now = time(NULL);
        int rc = -1;
        fd_set rfds;

        if (endtime >= now)
        {
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec = endtime - now;
            tv.tv_usec = 0;
            rc = select(fd+1, &rfds, NULL, NULL, &tv);
        } // if

        if ((rc <= 0) || (FD_ISSET(fd, &rfds) == 0))
            failure("503 Service Unavailable", "Timeout while talking to offload host.");

        // we can only read one byte at a time, since we don't want to
        //  read past end of headers, into actual content, here.
        if (read(fd, buf + br, 1) != 1)
            failure("503 Service Unavailable", "Read error while talking to offload host.");

        if (buf[br] == '\r')
            ;  // ignore these.
        else if (buf[br] == '\n')
        {
            char *ptr = NULL;
            if (br == 0)  // empty line, end of headers.
                return;
            buf[br] = '\0';
            if (seenresponse)
            {
                ptr = strchr(buf, ':');
                if (ptr != NULL)
                {
                    *(ptr++) = '\0';
                    while (*ptr == ' ')
                        ptr++;
                    listSet(headers, buf, ptr);
                } // if
            } // if
            else
            {
                listSet(headers, "response", buf);
                if (strncasecmp(buf, "HTTP/", 5) == 0)
                {
                    ptr = strchr(buf + 5, ' ');
                    if (ptr != NULL)
                    {
                        char *start = ptr + 1;
                        ptr = strchr(start, ' ');
                        if (ptr != NULL)
                            *ptr = '\0';
                        listSet(headers, "response_code", start);
                        ptr = start;
                    } // if
                } // if
                seenresponse = 1;
            } // else

            if (ptr == NULL)
                failure("503 Service Unavailable", "Bogus response from offload host server.");

            br = 0;
        } // if
        else
        {
            br++;
            if (br >= sizeof (buf))
                failure("503 Service Unavailable", "Buffer overflow.");
        } // else
    } // while
} // readHeaders


static void doWrite(const int fd, const char *str)
{
    const int len = strlen(str);
    int bw = 0;
    const time_t endtime = time(NULL) + GTIMEOUT;
    while (bw < len)
    {
        const time_t now = time(NULL);
        int rc = -1;
        fd_set wfds;

        if (endtime >= now)
        {
            struct timeval tv;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = endtime - now;
            tv.tv_usec = 0;
            rc = select(fd+1, NULL, &wfds, NULL, &tv);
        } // if

        if ((rc <= 0) || (FD_ISSET(fd, &wfds) == 0))
            failure("503 Service Unavailable", "Timeout while talking to offload base server.");

        rc = write(fd, str + bw, len - bw);
        if (rc <= 0)  // error? closed connection?
            failure("503 Service Unavailable", "Write error while talking to offload base server.");
        bw += rc;
    } // while
} // doWrite


static int doHttp(const char *method, list **headers)
{
    int rc = -1;
    struct addrinfo hints;
    memset(&hints, '\0', sizeof (hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG;

    struct addrinfo *dns = NULL;
    if ((rc = getaddrinfo(GBASESERVER, GBASESERVERPORTSTR, &hints, &dns)) != 0)
    {
        debugEcho("getaddrinfo failure: %s", gai_strerror(rc));
        failure("503 Service Unavailable", "Offload base server hostname lookup failure.");
    } // if

    int fd = -1;
    struct addrinfo *addr;
    for (addr = dns; addr != NULL; addr = addr->ai_next)
    {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd != -1)
        {
            if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
                break;
            close(fd);
            fd = -1;
        } // if
    } // for
    freeaddrinfo(dns);

    if (fd == -1)
        failure("503 Service Unavailable", "Couldn't connect to offload base server.");

    doWrite(fd, method);
    doWrite(fd, " ");
    doWrite(fd, Guri);
    doWrite(fd, " HTTP/1.1\r\n");
    doWrite(fd, "Host: " GBASESERVER "\r\n");
    doWrite(fd, "User-Agent: " GSERVERSTRING "\r\n");
    doWrite(fd, "Connection: close\r\n");
    doWrite(fd, "X-Mod-Offload-Bypass: true\r\n");
    doWrite(fd, "\r\n");
    readHeaders(fd, headers);
    return fd;
} // doHttp


static void http_head(list **head)
{
    const int fd = doHttp("HEAD", head);
    if (fd != -1)
        close(fd);
} // http_head


static int http_get(list **head)
{
    list *headers = NULL;
    const int fd = doHttp("GET", &headers);

    if ((head == NULL) || (fd == -1))
        listFree(&headers);

    if (head != NULL)
        *head = headers;
    return fd;
} // http_get


static char *etagToCacheFname(const char *etag)
{
    static const char chs[] = { ' ', '\t', 0x0B, '\"', '\'' };
    char *retval = xstrdup(etag);
    int i, j;

    for (i = 0; retval[i]; i++)
    {
        const char ch = retval[i];
        const int total = (sizeof (chs) / sizeof (chs[0]));
        for (j = 0; j < total; j++)
            if (ch == chs[j]) break;
        if (j == total)
            break;
    } // for

    if (i != 0)
        memmove(retval, retval + i, strlen(retval + i) + 1);

    for (i = strlen(retval) - 1; i >= 0; i--)
    {
        const char ch = retval[i];
        const int total = (sizeof (chs) / sizeof (chs[0]));
        for (j = 0; j < total; j++)
            if (ch == chs[j]) break;
        if (j == total)
            break;
    } // for

    retval[i+1] = '\0';

    return retval;
} // etagToCacheFname


static int selectReadable(const int fd)
{
    const time_t endtime = time(NULL) + GTIMEOUT;
    fd_set rfds;
    int rc = 0;

    while (1)
    {
        struct timeval tv;
        const time_t now = time(NULL);

        if (endtime >= now)
        {
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec = endtime - now;
            tv.tv_usec = 0;
            rc = select(fd+1, &rfds, NULL, NULL, &tv);
            if ((rc < 0) && (errno == EINTR))
                continue;   // just try again with adjusted timeout.
        } // if

        break;
    } // while

    if ((rc <= 0) || (FD_ISSET(fd, &rfds) == 0))
    {
        debugEcho("select() failed");
        return 0;
    } // if

    return 1;
} // selectReadable


static void cacheFailure(const char *err)
{
    debugEcho("%s", err);
    nukeRequestFromCache();
    terminate();
} // cacheFailure


static void cacheProcessSig(int sig)
{
    char errbuf[128];
    snprintf(errbuf, sizeof (errbuf), "caught signal #%d!", sig);
    cacheFailure(errbuf);
} // cacheProcessSig


static inline int64 Min(const int64 a, const int64 b)
{
    return (a < b) ? a : b;
} // Min


static pid_t cacheFork(const int sock, FILE *cacheio, const int64 max)
{
    debugEcho("Cache needs refresh...pulling from base server...");

    const pid_t pid = fork();

    if (pid != 0)  // don't need these any more...
    {
        fclose(cacheio);
        close(sock);
    } // if

    if (pid == -1)  // failed!
    {
        nukeRequestFromCache();
        failure("500 Internal Server Error", "Couldn't fork for caching.");
        return pid;
    } // if

    else if (pid != 0)  // we're the parent.
    {
        debugEcho("fork()'d caching process! new pid is (%d).", (int) pid);
        return pid;
    } // else if

    // we're the child.
    GIsCacheProcess = 1;
    debugEcho("caching process (%d) starting up!", (int) getpid());

    #if GMAXDUPEDOWNLOADS > 0
    if (GAllDownloads != NULL)
        munmap(GAllDownloads, sizeof (DownloadRecord) * MAX_DOWNLOAD_RECORDS);
    GAllDownloads = GMyDownload = NULL;
    #endif

    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    stdin = stderr = stdout = NULL;
    chdir("/");
    setsid();

    // try to clean up in most fatal cases.
    signal(SIGHUP, cacheProcessSig);
    signal(SIGINT, cacheProcessSig);
    signal(SIGTERM, cacheProcessSig);
    signal(SIGPIPE, cacheProcessSig);
    signal(SIGQUIT, cacheProcessSig);
    signal(SIGTRAP, cacheProcessSig);
    signal(SIGABRT, cacheProcessSig);
    signal(SIGBUS, cacheProcessSig);
    signal(SIGSEGV, cacheProcessSig);

    #if GSETPROCTITLE
        #ifdef __linux__
        {
            snprintf(GArgv[0], GMaxArgvLen, "offload: CACHE %s", Guri);
            char *p = &GArgv[0][strlen(GArgv[0])];
            while(p < GLastArgv)
                *(p++) = '\0';
            GArgv[1] = NULL;
        }
        #endif
    #endif

    int64 br = 0;
    while (br < max)
    {
        int len = 0;
        char data[32 * 1024];
        const int readsize = (int) Min(sizeof (data), (max - br));

        if (readsize == 0)
            cacheFailure("readsize is unexpectedly zero.");
        else if (!selectReadable(sock))
            cacheFailure("network timeout");
        else if ((len = read(sock, data, sizeof (data))) <= 0)
            cacheFailure("network read error");
        else if (fwrite(data, len, 1, cacheio) != 1)
            cacheFailure("fwrite() failed");
        else if (fflush(cacheio) == EOF)
            cacheFailure("fflush() failed");
        br += len;
        debugEcho("wrote %d bytes to the cache.", len);
    } // while

    if (fclose(cacheio) == EOF)
        cacheFailure("fclose() failed");

    debugEcho("Successfully cached! Terminating!");
    terminate();  // always die.
    return -1;
} // cacheFork


static int serverMainline(int argc, char **argv, char **envp)
{
    const char *httprange = copyEnv("HTTP_RANGE");
    const char *ifrange = copyEnv("HTTP_IF_RANGE");
    Guri = copyEnv("REQUEST_URI");
    GRemoteAddr = copyEnv("REMOTE_ADDR");
    GReferer = copyEnv("HTTP_REFERER");
    GUserAgent = copyEnv("HTTP_USER_AGENT");
    GReqVersion = copyEnv("REQUEST_VERSION");
    GReqMethod = copyEnv("REDIRECT_REQUEST_METHOD");
    if (GReqMethod == NULL)
        GReqMethod = copyEnv("REQUEST_METHOD");
    if (GReqMethod == NULL)
        GReqMethod = "GET";
    if (GReqVersion == NULL)
        GReqVersion = "";

    debugInit(argc, argv, envp);

    if ((Guri == NULL) || (*Guri != '/'))
        failure("500 Internal Server Error", "Bad request URI");

    // Feed a fake robots.txt to keep webcrawlers out of the offload server.
    if (strcmp(Guri, "/robots.txt") == 0)
        failure("200 OK", "User-agent: *\nDisallow: /");

    // !!! FIXME: favicon?

    #if GSETPROCTITLE
        #ifdef __linux__
        {
            // This nastiness inspired by proftpd.
            int i;
            for (i = 0; i < argc; i++)
            {
                if (!i || (GLastArgv + 1 == argv[i]))
                    GLastArgv = argv[i] + strlen(argv[i]);
            } // for
            for (i = 0; envp[i] != NULL; i++)
            {
                if ((GLastArgv + 1) == envp[i])
                    GLastArgv = envp[i] + strlen(envp[i]);
            } // for

            extern char *__progname, *__progname_full;
            __progname = xstrdup("offload");
            __progname_full = xstrdup(argv[0]);
            GNoMoreGetEnv = 1;
            static char *nullenv = NULL;
            envp = environ = &nullenv;

            GArgv = argv;
            GMaxArgvLen = (GLastArgv - GArgv[0]) - 2;
            snprintf(GArgv[0], GMaxArgvLen, "offload: %s %s %s", GRemoteAddr, GReqMethod, Guri);
            char *p = &GArgv[0][strlen(GArgv[0])];
            while(p < GLastArgv)
                *(p++) = '\0';
            GArgv[1] = NULL;
        }
        #endif
    #endif

    const int isget = (strcasecmp(GReqMethod, "GET") == 0);
    const int ishead = (strcasecmp(GReqMethod, "HEAD") == 0);
    if ( (strchr(Guri, '?') != NULL) || ((!isget) && (!ishead)) )
        failure("403 Forbidden", "Offload server doesn't do dynamic content.");

    if (!ishead)
        setDownloadRecord();

    list *head = NULL;
    http_head(&head);

    #if GDEBUG
    {
        debugEcho("The HTTP HEAD from %s ...", GBASESERVER);
        list *item;
        for (item = head; item; item = item->next)
            debugEcho("   '%s' => '%s'", item->key, item->value);
    }
    #endif

    const char *responsecodestr = listFind(head, "response_code");
    const char *response = listFind(head, "response");
    const char *etag = listFind(head, "ETag");
    const char *contentlength = listFind(head, "Content-Length");
    const char *lastmodified = listFind(head, "Last-Modified");
    const int iresponse = responsecodestr ? atoi(responsecodestr) : 0;

    if ((iresponse == 401) || (listFind(head, "WWW-Authenticate")))
        failure("403 Forbidden", "Offload server doesn't do protected content.");
    else if (iresponse != 200)
        failure_location(response, response, listFind(head, "Location"));
    else if ((!etag) || (!contentlength) || (!lastmodified))
        failure("403 Forbidden", "Offload server doesn't do dynamic content.");

    listSet(&head, "X-Offload-Orig-ETag", etag);
    if ((strlen(etag) <= 2) || (strncasecmp(etag, "W/", 2) != 0))
        listSet(&head, "X-Offload-Is-Weak", "0");
    else  // a "weak" ETag?
    {
        debugEcho("There's a weak ETag on this request.");
        listSet(&head, "X-Offload-Is-Weak", "1");
        etag = listSet(&head, "ETag", etag + 2);
        debugEcho("Chopped ETag to be [%s]", etag);
    } // if

    // !!! FIXME: Check Cache-Control, Pragma no-cache

    int io = -1;

    if (ishead)
        debugEcho("This is a HEAD request to the offload server.");

    // Partial content:
    // Does client want a range (download resume, "web accelerators", etc)?
    const int64 max = atoi64(contentlength);
    int64 startRange = 0;
    int64 endRange = max-1;
    int reportRange = 0;
    char *responseCode = "200 OK";

    if (ifrange != NULL)
    {
        // !!! FIXME: handle this.
        debugEcho("Client set If-Range: [%s]...unsupported!", ifrange);
        httprange = NULL;
    } // if

    if (httprange != NULL)
    {
        debugEcho("There's a HTTP_RANGE specified: [%s].", httprange);
        if (strncasecmp(httprange, "bytes=", 6) != 0)
            failure("400 Bad Request", "Only ranges of 'bytes' accepted.");
        else if (strchr(httprange, ',') != NULL)
            failure("400 Bad Request", "Multiple ranges not currently supported");
        else
        {
            httprange += 6;
            char *pos = strchr(httprange, '-');
            if (pos != NULL)
            {
                *(pos++) = '\0';
                startRange = *httprange == '\0' ? 0 : atoi64(httprange);
                endRange = *pos == '\0' ? max-1 : atoi64(pos);
                responseCode = "206 Partial Content";
                reportRange = 1;
            } // if
        } // else
    } // if

    if (endRange >= max)  // apparently, this is legal to request.
        endRange = max - 1;

    debugEcho("We are feeding the client bytes %lld to %lld of %lld",
                (long long) startRange, (long long) endRange, (long long) max);

    if (invalidContentRange(startRange, endRange, max))
        failure("400 Bad Request", "Bad content range requested.");

    char *etagFname = etagToCacheFname(etag);
    GFilePath = makeStr("%s/filedata-%s", GOFFLOADDIR, etagFname);
    GMetaDataPath = makeStr("%s/metadata-%s", GOFFLOADDIR, etagFname);
    free(etagFname);

    listSet(&head, "X-Offload-Orig-URL", Guri);
    listSet(&head, "X-Offload-Hostname", GBASESERVER);

    debugEcho("metadata cache is %s", GMetaDataPath);
    debugEcho("file cache is %s", GFilePath);

    list *metadata = NULL;

    if (ishead)
        metadata = head;
    else
    {
        getSemaphore();

        metadata = loadMetadata(GMetaDataPath);
        if (cachedMetadataMostRecent(metadata, head))
        {
            listFree(&head);
            debugEcho("File is cached.");
        } // if

        else
        {
            listFree(&metadata);

            // we need to pull a new copy from the base server...
            const int sock = http_get(NULL);  // !!! FIXME: may block, don't hold semaphore here!

            FILE *cacheio = fopen(GFilePath, "wb");
            if (cacheio == NULL)
            {
                close(io);
                failure("500 Internal Server Error", "Couldn't update cached data.");
            } // if

            FILE *metaout = fopen(GMetaDataPath, "wb");
            if (metaout == NULL)
            {
                fclose(cacheio);
                close(sock);
                nukeRequestFromCache();
                failure("500 Internal Server Error", "Couldn't update metadata.");
            } // if

            // !!! FIXME: This is a race condition...may change between HEAD
            // !!! FIXME:  request and actual HTTP grab. We should really
            // !!! FIXME:  just use this for comparison once, and if we are
            // !!! FIXME:  recaching, throw this out and use the headers from the
            // !!! FIXME:  actual HTTP grab when really updating the metadata.
            //
            // !!! FIXME: Also, write to temp file and rename in case of write failure!
            if (!listFind(head, "Content-Type"))  // make sure this is sane.
                listSet(&head, "Content-Type", "application/octet-stream");

            const pid_t pid = cacheFork(sock, cacheio, max);
            listSet(&head, "X-Offload-Caching-PID", makeNum(pid));

            list *item;
            for (item = head; item; item = item->next)
                fprintf(metaout, "%s\n%s\n", item->key, item->value);
            fclose(metaout);  // !!! FIXME: check for errors

            metadata = head;
        } // else

        putSemaphore();

        head = NULL;   // we either moved this to (metadata) or free()d it.

        io = open(GFilePath, O_RDONLY);
        if (io == -1)
            failure("500 Internal Server Error", "Couldn't access cached data.");
    } // else

    if (!GHttpStatus)
        GHttpStatus = atoi(responseCode);

    printf("HTTP/1.1 %s\r\n", responseCode);
    printf("Status: %s\r\n", responseCode);
    printf_date_header(stdout);
    printf("Server: %s\r\n", GSERVERSTRING);
    printf("Connection: close\r\n");
    printf("ETag: %s\r\n", listFind(metadata, "ETag"));
    printf("Last-Modified: %s\r\n", listFind(metadata, "Last-Modified"));
    printf("Content-Length: %lld\r\n", (long long) ((endRange - startRange) + 1));
    printf("Accept-Ranges: bytes\r\n");
    printf("Content-Type: %s\r\n", listFind(metadata, "Content-Type"));
    if (reportRange)
    {
        printf("Content-Range: bytes %lld-%lld/%lld\r\n",
               (long long) startRange, (long long) endRange, (long long) max);
    } // if
    printf("\r\n");

    listFree(&metadata);

    if (ishead)
    {
        debugEcho("This was a HEAD request to offload server, so we're done.");
        terminate();
    } // if

    int64 br = 0;
    endRange++;
    time_t lastReadTime = time(NULL);
    while (br < endRange)
    {
        // !!! FIXME: sendfile and TCP_CORK?
        char data[32 * 1024];
        int64 readsize = startRange - br;
        if ((readsize <= 0) || (readsize > sizeof (data)))
            readsize = sizeof (data);

        if (readsize > (endRange - br))
            readsize = (endRange - br);

        if (readsize == 0)
        {
            debugEcho("readsize is unexpectedly zero.");
            break;  // Shouldn't hit, but just in case...
        } // if

        struct stat statbuf;
        if (fstat(io, &statbuf) == -1)
        {
            debugEcho("fstat() failed.");
            break;
        } // if

        const int64 cursize = statbuf.st_size;
        const time_t now = time(NULL);
        if (cursize < max)
        {
            if ((cursize - br) <= 0)  // may be caching on another process.
            {
                if (now > (lastReadTime + GTIMEOUT))
                {
                    debugEcho("timeout: cache file seems to have stalled.");
                    // !!! FIXME: maybe try to kill() the cache process?
                    break;   // oh well, give up.
                } // if

                sleep(1);   // wait awhile...
                continue;   // ...then try again.
            } // if
        } // else

        lastReadTime = now;

        const int len = read(io, data, readsize);
        if (len <= 0)
        {
            debugEcho("read() failed");
            break;   // select() and fstat() should have caught this...
        } // if

#if 0
        // see if the remote end shutdown their end of the socket
        //  (web browser user hit cancel, etc).
        int deadsocket = 0;

        #if GLISTENPORT
        while (1)
        {
            char onebyte = 0;
            const ssize_t recvval = recv(1, &onebyte, sizeof (onebyte), MSG_DONTWAIT);
            deadsocket = (recvval == 0);
            if ( ((recvval < 0) && (errno == EAGAIN)) || (deadsocket) )
                break;
        } // while
        #endif

        if (deadsocket || feof(stdout))
        {
            debugEcho("EOF on stdout!");
            break;
        } // if
#endif

        if ((br >= startRange) && (br < endRange))
        {
            #if ((GDEBUG) && (!GDEBUGTOFILE))
            debugEcho("Would have written %d bytes", len);
            GBytesSent += len;
            #elif ((!GDEBUG) || (GDEBUGTOFILE))
            const int bw = (int) fwrite(data, 1, len, stdout);
            debugEcho("Wrote %d bytes", bw);
            GBytesSent += (int64) bw;
            if (bw != len)
            {
                debugEcho("FAILED to write %d bytes to client!", len-bw);
                break;
            } // else
            #endif
        } // else if

        br += len;
    } // while

    debugEcho("closing cache file...");
    close(io);

    debugEcho("Transfer loop is complete.");

    if (br != endRange)
    {
        debugEcho("Bogus transfer! Sent %lld, wanted to send %lld!",
                  (long long) br, (long long) endRange);
    } // else

    terminate();  // done!
    return 0;
} // serverMainline


#if GLISTENPORT
#define GLISTENPORTSTR OFFLOAD_NUMSTR(GLISTENPORT)
static const char *readClientHeaders(const int fd, const struct sockaddr *addr)
{
    debugEcho("Reading request headers...");

    // !!! FIXME: do this without network-specifics?
    void *ipptr = NULL;
    if (addr->sa_family == AF_INET)
        ipptr = &((struct sockaddr_in *) addr)->sin_addr;
    else if (addr->sa_family == AF_INET6)
        ipptr = &((struct sockaddr_in6 *) addr)->sin6_addr;

    char remoteaddr[64] = { '\0' };
    int trusted = 0;
    if ((!ipptr) || (!inet_ntop(addr->sa_family, ipptr, remoteaddr, sizeof (remoteaddr))))
        debugEcho("Don't know remote address!");
    else
    {
        debugEcho("Remote address is %s", remoteaddr);

        static const char *trust[] = { GLISTENTRUSTFWD };
        const int total = sizeof (trust) / sizeof (trust[0]);
        int i;
        for (i = 0; i < total; i++)
        {
            if ((trust[i]) && (strcmp(trust[i], remoteaddr) == 0))
                break;
        } // for
        trusted = (i < total);
        debugEcho("This address %s a trusted proxy.", trusted ? "is" : "is not");
    } // else

    const time_t endtime = time(NULL) + GTIMEOUT;
    int br = 0;
    char buf[1024];
    int seenresponse = 0;
    while (1)
    {
        const time_t now = time(NULL);
        int rc = -1;
        fd_set rfds;

        if (endtime >= now)
        {
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec = endtime - now;
            tv.tv_usec = 0;
            rc = select(fd+1, &rfds, NULL, NULL, &tv);
        } // if

        if ((rc <= 0) || (FD_ISSET(fd, &rfds) == 0))
            return "Timeout while talking to client.";

        // we can only read one byte at a time, since we don't want to
        //  read past end of headers, into actual content, here.
        if (read(fd, buf + br, 1) != 1)
            return "Read error while talking to client.";

        if (buf[br] == '\r')
            ;  // ignore these.
        else if (buf[br] == '\n')
        {
            char *ptr = NULL;
            if (br == 0)  // empty line, end of headers.
                break;

            buf[br] = '\0';
            if (seenresponse)
            {
                debugEcho("Saw request line from client: '%s'", buf);

                ptr = strchr(buf, ':');
                if (ptr != NULL)
                {
                    *(ptr++) = '\0';
                    while (*ptr == ' ')
                        ptr++;

                    if (strcasecmp(buf, "X-Forwarded-For") == 0)
                    {
                        if (trusted)
                            snprintf(remoteaddr, sizeof (remoteaddr), "%s", ptr);
                    } // if

                    else if (strcasecmp(buf, "User-Agent") == 0)
                        setenv("HTTP_USER_AGENT", ptr, 1);

                    else if (strcasecmp(buf, "Range") == 0)
                        setenv("HTTP_RANGE", ptr, 1);

                    else if (strcasecmp(buf, "If-Range") == 0)
                        setenv("HTTP_IF_RANGE", ptr, 1);

                    else if (strcasecmp(buf, "Referer") == 0)
                        setenv("HTTP_REFERER", ptr, 1);

                    // we currently don't care about anything else.
                } // if
            } // if

            else
            {
                ptr = strchr(buf, ' ');
                if (ptr != NULL)
                {
                    *(ptr++) = '\0';
                    while (*ptr == ' ')
                        ptr++;
                    setenv("REQUEST_METHOD", buf, 1);
                    const char *start = ptr;
                    ptr = strchr(ptr, ' ');
                    if (ptr != NULL)
                    {
                        *(ptr++) = '\0';
                        while (*ptr == ' ')
                            ptr++;
                        setenv("REQUEST_URI", start, 1);
                        if (strncasecmp(ptr, "HTTP/", 5) == 0)
                            setenv("REQUEST_VERSION", ptr, 1);
                        else
                            ptr = NULL;  // fail below.
                    } // if
                } // if
                seenresponse = 1;
            } // else

            if (ptr == NULL)
                return "Bogus request from client.";

            br = 0;
        } // if
        else
        {
            br++;
            if (br >= sizeof (buf))
                return "Buffer overflow.";
        } // else
    } // while

    if (remoteaddr[0])
        setenv("REMOTE_ADDR", remoteaddr, 1);

    debugEcho("done parsing request headers");
    return NULL;
} // readClientHeaders


static void daemonChildSig(int sig)
{
    debugEcho("caught signal #%d!", sig);
    terminate();
} // daemonChildSig


static inline void daemonChild(const int fd, const struct sockaddr *addr,
                               int argc, char **argv)
{
    // try to clean up in most fatal cases.
    signal(SIGHUP, daemonChildSig);
    signal(SIGINT, daemonChildSig);
    signal(SIGTERM, daemonChildSig);
    signal(SIGPIPE, daemonChildSig);
    signal(SIGQUIT, daemonChildSig);
    signal(SIGTRAP, daemonChildSig);
    signal(SIGABRT, daemonChildSig);
    signal(SIGBUS, daemonChildSig);
    signal(SIGSEGV, daemonChildSig);

    if (fd == 0)
        dup2(fd, 1);
    else if (fd == 1)
        dup2(fd, 0);
    else
    {
        dup2(fd, 0);
        dup2(fd, 1);
        close(fd);
    } // else

    stdin = fdopen(0, "rb");
    stdout = fdopen(1, "wb");
    stderr = fopen("/dev/null", "wb");

    debugEcho("New child running to handle incoming request.");

    if ((stdin) && (stdout) && (stderr))
    {
        setbuf(stdout, NULL);
        if (readClientHeaders(0, addr) == NULL)  // NULL == no error.
            serverMainline(argc, argv, environ);
    } // if

    terminate();
} // daemonChild


#if !GLISTENDAEMONIZE
#define daemonToBackground()
#else
static void daemonToBackground(void)
{
    const pid_t backpid = fork();
    if (backpid > 0)  // parent.
        exit(0);

    else if (backpid == -1)
    {
        fprintf(stderr, "Failed to fork(): %s\n", strerror(errno));
        exit(1);
    } // if

    // we're the child. Welcome to the background.
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    stdin = stderr = stdout = NULL;
    chdir("/");
    setsid();
}
#endif


static int daemonListenSocket(void)
{
    struct addrinfo hints;
    memset(&hints, '\0', sizeof (hints));
    hints.ai_family = GLISTENFAMILY;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_V4MAPPED | AI_ALL | AI_ADDRCONFIG;

    int rc = -1;
    struct addrinfo *dns = NULL;
    if ((rc = getaddrinfo(GLISTENADDR, GLISTENPORTSTR, &hints, &dns)) != 0)
    {
        if (stderr != NULL)
            fprintf(stderr, "getaddrinfo failure: %s\n", gai_strerror(rc));
        return -1;
    } // if

    int fd = -1;
    struct addrinfo *addr;
    for (addr = dns; addr != NULL; addr = addr->ai_next)
    {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd != -1)
        {
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            if (bind(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            {
                if (listen(fd, 16) == 0)
                    break;
            } // if

            close(fd);
            fd = -1;
        } // if
    } // for
    freeaddrinfo(dns);

    if (fd == -1)
    {
        if (stderr != NULL)
            fprintf(stderr, "Failed to bind socket.\n");
    } // if

    return fd;
} // daemonListenSocket


static inline int daemonMainline(int argc, char **argv, char **envp)
{
    signal(SIGCHLD, SIG_IGN);
    daemonToBackground();

    const int fd = daemonListenSocket();
    if (fd == -1)
        return 2;

    while (1)  // loop forever.
    {
        struct sockaddr addr;
        socklen_t addrlen = sizeof (addr);
        const int newfd = accept(fd, &addr, &addrlen);
        if (newfd != -1)
        {
            const pid_t pid = fork();
            if (pid != 0)  // we're NOT the child.
                close(newfd);
            else
            {
                close(fd);
                daemonChild(newfd, &addr, argc, argv);
                terminate();  // just in case.
            } // else
        } // if
    } // while

    return 0;
} // daemonMainline
#endif


int main(int argc, char **argv, char **envp)
{
    #if !GLISTENPORT
    return serverMainline(argc, argv, envp);
    #else
    return daemonMainline(argc, argv, envp);
    #endif
} // main


// end of nph-offload.c ...



#if GMAXDUPEDOWNLOADS > 0

// SHA-1 code originally from ftp://ftp.funet.fi/pub/crypt/hash/sha/sha1.c
//  License: public domain.
//  I cleaned it up a little for my specific purposes. --ryan.

/*
SHA-1 in C
By Steve Reid <steve@edmweb.com>
100% Public Domain
*/

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/* blk0() and blk() perform the initial expand. */
/* I got the idea of expanding during the round function from SSLeay */
#if !PLATFORM_BIGENDIAN
#define blk0(i) (block->l[i] = (rol(block->l[i],24)&0xFF00FF00) \
    |(rol(block->l[i],8)&0x00FF00FF))
#else
#define blk0(i) block->l[i]
#endif
#define blk(i) (block->l[i&15] = rol(block->l[(i+13)&15]^block->l[(i+8)&15] \
    ^block->l[(i+2)&15]^block->l[i&15],1))

/* (R0+R1), R2, R3, R4 are the different operations used in SHA1 */
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk0(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+blk(i)+0x5A827999+rol(v,5);w=rol(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0x6ED9EBA1+rol(v,5);w=rol(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+blk(i)+0x8F1BBCDC+rol(v,5);w=rol(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+blk(i)+0xCA62C1D6+rol(v,5);w=rol(w,30);


/* Hash a single 512-bit block. This is the core of the algorithm. */

static void Sha1_transform(uint32 state[5], const uint8 buffer[64])
{
    uint32 a, b, c, d, e;
    typedef union {
        uint8 c[64];
        uint32 l[16];
    } CHAR64LONG16;
    CHAR64LONG16* block;
    static uint8 workspace[64];
    block = (CHAR64LONG16*)workspace;
    memcpy(block, buffer, 64);
    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);
    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    /* Wipe variables */
    a = b = c = d = e = 0;
}

static void Sha1_init(Sha1 *context)
{
    /* SHA1 initialization constants */
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}


/* Run your data through this. */

static void Sha1_append(Sha1 *context, const uint8 *data, uint32 len)
{
    uint32 i, j;

    j = (context->count[0] >> 3) & 63;
    if ((context->count[0] += len << 3) < (len << 3)) context->count[1]++;
    context->count[1] += (len >> 29);
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64-j));
        Sha1_transform(context->state, context->buffer);
        for ( ; i + 63 < len; i += 64) {
            Sha1_transform(context->state, &data[i]);
        }
        j = 0;
    }
    else i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}


/* Add padding and return the message digest. */

static void Sha1_finish(Sha1 *context, uint8 digest[20])
{
    uint32 i, j;
    uint8 finalcount[8];

    for (i = 0; i < 8; i++) {
        finalcount[i] = (uint8)((context->count[(i >= 4 ? 0 : 1)]
         >> ((3-(i & 3)) * 8) ) & 255);  /* Endian independent */
    }
    Sha1_append(context, (uint8 *)"\200", 1);
    while ((context->count[0] & 504) != 448) {
        Sha1_append(context, (uint8 *)"\0", 1);
    }
    Sha1_append(context, finalcount, 8);  /* Should cause a Sha1_transform() */
    for (i = 0; i < 20; i++) {
        digest[i] = (uint8)
         ((context->state[i>>2] >> ((3-(i & 3)) * 8) ) & 255);
    }
    /* Wipe variables */
    i = j = 0;
    memset(context->buffer, 0, 64);
    memset(context->state, 0, 20);
    memset(context->count, 0, 8);
    memset(&finalcount, 0, 8);
    Sha1_transform(context->state, context->buffer);
}

#endif

