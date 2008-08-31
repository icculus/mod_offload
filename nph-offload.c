// This is a C program that handles offloading of bandwidth from a web
//  server. It's a sort of poor-man's Akamai. It doesn't need anything
//  terribly complex (a webserver with cgi-bin support, a writable directory).
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
//  have a time limit on script execution (max_execution_time setting, or
//  just don't run this script in safe mode and it'll handle it). PHP for
//  Windows currently doesn't support sysvsem, so until someone writes me
//  a mutex implementation, we assume you'll use a Unix box for this script.
//
// You need Apache to push every web request to this script, presumably in a
//  virtual host, if not the entire server.
//
// Assuming this script was at /www/scripts/index.php, you would want to add
//  this to Apache's config:
//
//   AliasMatch ^.*$ "/www/scripts/index.php"
//
// If you don't have control over the virtual host's config file, you can't
//  use AliasMatch, but if you can put an .htaccess file in the root of the
//  virtual host, you can get away with this:
//
//   ErrorDocument 404 /index.php
//
// This will make all missing files (everything) run the script, which will
//  then cache and distribute the correct content, including overriding the
//  404 status code with the correct one. Be careful about files that DO exist
//  in that vhost directory, though. They won't offload.
//
// You can offload multiple base servers with one box: set up one virtual host
//  on the offload server for each base server. This lets each base server
//  have its own cache and configuration.
//
// Then edit offload_server_config.php to fit your needs.
//
// Restart the server so the AliasMatch configuration tweak is picked up.
//
// This file is written by Ryan C. Gordon (icculus@icculus.org).

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
#include <netdb.h>

#define GVERSION "1.0.0"
#define GSERVERSTRING "nph-offload.c/" GVERSION

#include "offload_server_config.h"

#ifdef __GNUC__
#define ISPRINTF(x,y) __attribute__((format (printf, x, y)))
#else
#define ISPRINTF(x,y)
#endif

#ifdef max
#undef max
#endif

// some getaddrinfo() flags that may not exist...
#ifndef AI_ADDRCONFIG
#define AI_ADDRCONFIG 0
#endif
#ifndef AI_NUMERICSERV
#define AI_NUMERICSERV 0
#endif
#ifndef AI_V4MAPPED
#define AI_V4MAPPED 0
#endif

typedef int64_t int64;

static int GIsCacheProcess = 0;
static char *Guri = NULL;
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
        snprintf(buf, sizeof(buf), GOFFLOADDIR "/debug-%d", (int) getpid());
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


static void printf_date_header(FILE *out)
{
    // strftime()'s "%a" gives you locale-dependent strings...
    static const char *weekday[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
    };

    // strftime()'s "%b" gives you locale-dependent strings...
    static const char *month[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    if (out == NULL)
        return;

    time_t now = time(NULL);
    const struct tm *tm = gmtime(&now);
    fprintf(out, "Date: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n",
             weekday[tm->tm_wday], tm->tm_mday, month[tm->tm_mon],
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


static void *createSemaphore(const int initialVal)
{
    char semname[64];
    void *retval = NULL;
    const int value = initialVal ? 0 : 1;
    int created = 1;

    snprintf(semname, sizeof (semname), "MOD-OFFLOAD-%d", (int) getuid());
    retval = sem_open(semname, O_CREAT | O_EXCL, 0600, value);
    if ((retval == (void *) SEM_FAILED) && (errno == EEXIST))
    {
        created = 0;
        debugEcho("(semaphore already exists, just opening existing one.)");
        retval = sem_open(semname, 0);
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


static void terminate(void)
{
    if (!GIsCacheProcess)
    {
        debugEcho("offload program is terminating...");
        while (GSemaphoreOwned > 0)
            putSemaphore();
    } // if

    if (GDebugFilePointer != NULL)
        fclose(GDebugFilePointer);
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


static int process_dead(int pid)
{
    return ( (pid <= 0) || ((kill(pid, 0) == -1) && (errno == ESRCH)) );
} // process_dead


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
    debugEcho("Request from address: %s", getenv("REMOTE_ADDR"));
    debugEcho("Client User-Agent: %s", getenv("HTTP_USER_AGENT"));
    debugEcho("Referrer string: %s", getenv("HTTP_REFERER"));
    debugEcho("Request method: %s", getenv("REQUEST_METHOD"));
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
                if (strncmp(buf, "HTTP/", 5) == 0)
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
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV | AI_V4MAPPED | AI_ADDRCONFIG;

    struct addrinfo *dns = NULL;
    if ((rc = getaddrinfo(GBASESERVER, "80", &hints, &dns)) != 0)
    {
        debugEcho("getaddrinfo failure: %s", gai_strerror(rc));
        failure("503 Service Unavailable", "Offload base server hostname lookup failure.");
    } // if

    int fd = -1;
    struct addrinfo *addr;
    for (addr = dns; addr != NULL; addr = addr->ai_next)
    {
        if (addr->ai_socktype != SOCK_STREAM)
            continue;

        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0)
            break;
        else
        {
            close(fd);
            fd = -1;
        } // else
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


static void catchsig(int sig)
{
    char errbuf[128];
    snprintf(errbuf, sizeof (errbuf), "caught signal #%d!", sig);
    cacheFailure(errbuf);
} // catchsig


static inline int64 Min(const int64 a, const int64 b)
{
    return (a < b) ? a : b;
} // Min


static int cacheFork(const int sock, FILE *cacheio, const int64 max)
{
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
        return 0;
    } // if

    else if (pid != 0)  // we're the parent.
    {
        debugEcho("fork()'d caching process! new pid is (%d).", (int) pid);
        return 1;
    } // else if

    // we're the child.
    GIsCacheProcess = 1;
    debugEcho("caching process (%d) starting up!", (int) getpid());
    fclose(stdin);
    fclose(stdout);
    fclose(stderr);
    stdin = NULL;
    stderr = NULL;
    stdout = NULL;
    chdir("/");
    setsid();

    // try to clean up in most fatal cases.
    signal(SIGHUP, catchsig);
    signal(SIGINT, catchsig);
    signal(SIGTERM, catchsig);
    signal(SIGPIPE, catchsig);
    signal(SIGQUIT, catchsig);
    signal(SIGTRAP, catchsig);
    signal(SIGABRT, catchsig);
    signal(SIGBUS, catchsig);
    signal(SIGSEGV, catchsig);

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
    return 0;
} // cacheFork


int main(int argc, char **argv, char **envp)
{
    Guri = getenv("REQUEST_URI");

    debugInit(argc, argv, envp);
    if ((Guri == NULL) || (*Guri != '/'))
        failure("500 Internal Server Error", "Bad request URI");

    // Feed a fake robots.txt to keep webcrawlers out of the offload server.
    if (strcmp(Guri, "/robots.txt") == 0)
        failure("200 OK", "User-agent: *\nDisallow: /");

    const char *reqmethod = getenv("REDIRECT_REQUEST_METHOD");
    if (reqmethod == NULL)
        reqmethod = getenv("REQUEST_METHOD");
    if (reqmethod == NULL)
        reqmethod = "GET";

    const int isget = (strcasecmp(reqmethod, "GET") == 0);
    const int ishead = (strcasecmp(reqmethod, "HEAD") == 0);
    if ( (strchr(Guri, '?') != NULL) || ((!isget) && (!ishead)) )
        failure("403 Forbidden", "Offload server doesn't do dynamic content.");

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
    const char *httprange = getenv("HTTP_RANGE");
    const char *ifrange = getenv("HTTP_IF_RANGE");

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

            listSet(&head, "X-Offload-Caching-PID", makeNum(getpid()));

            list *item;
            for (item = head; item; item = item->next)
                fprintf(metaout, "%s\n%s\n", item->key, item->value);
            fclose(metaout);  // !!! FIXME: check for errors

            metadata = head;
            debugEcho("Cache needs refresh...pulling from base server...");
            cacheFork(sock, cacheio, max);
        } // else

        putSemaphore();

        head = NULL;   // we either moved this to (metadata) or free()d it.

        io = open(GFilePath, O_RDONLY);
        if (io == -1)
            failure("500 Internal Server Error", "Couldn't access cached data.");
    } // else

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

        if (feof(stdout))
        {
            debugEcho("EOF on stdout!");
            break;
        } // if

        else if ((br >= startRange) && (br < endRange))
        {
            #if ((GDEBUG) && (!GDEBUGTOFILE))
            debugEcho("Would have written %d bytes", len);
            #elif ((!GDEBUG) || (GDEBUGTOFILE))
            if (fwrite(data, len, 1, stdout) == 1)
                debugEcho("Wrote %d bytes", len);
            else
            {
                debugEcho("FAILED to write %d bytes to client!", len);
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
} // main

// end of nph-offload.c ...

