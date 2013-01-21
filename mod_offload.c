/*
 * mod_offload
 *
 * This is a module that lets you redirect static content to "offload"
 *  servers, such that they can distribute the bandwidth load normally
 *  given to the "base" server. For sites that are completely static and/or
 *  completely replicated via other methods such as rsync, installing this
 *  module may be sufficient to spread the burden between multiple servers.
 *  This can be paired with the included offload.php script that aids in
 *  keeping servers in sync without the need to copy the entire base server's
 *  contents or install other tools and daemons on the offload servers
 *  (just this module on the base server, and a PHP script on the offload
 *  servers). icculus.org uses this to spread the bandwidth around.
 *
 * To Build/Install as a shared module:
 *  /where/i/installed/apache/bin/apxs -c -i -a mod_offload.c
 *
 * Make sure this is in your Apache config:
 *   LoadModule offload_module     libexec/mod_offload.so
 *   AddModule mod_offload.c
 *
 * Now pick a directory/location/virtualhost/htaccess and add some directives:
 *
 *   OffloadEngine On
 *   OffloadHost offload1.myhost.com
 *
 * That's all you need for basic operation. This enables offloading, and
 *  sets up one hostname ("offload1.myhost.com") as an offload server.
 *
 * All directives are listed below. They may appear anywhere in the
 *  server-wide configuration files (e.g., httpd.conf), and in .htaccess
 *  files when the scope is covered by an AllowOverride Options keyword.
 *
 *  OffloadEngine <On|Off>
 *    ...turn on or off the offload functionality. Defaults to Off.
 *    If not enabled, this module will just pass requests through to the
 *    next Apache handler untouched.
 *
 *  OffloadHost <hostname>
 *    ...An offload host. You need at least one specified for offloading
 *    to function. You can specify this directive multiple times to add
 *    more offload servers. These can be IP addresses or FQDN's...this
 *    module will try to do a DNS lookup at startup if necessary, and will
 *    fail if it can't.
 *
 *  OffloadDebug <On|Off>
 *    ...when on, will write details about every transaction to the error
 *    log. You want this turned off as soon as you are satisfied the
 *    module is functioning well.
 *
 *  OffloadMinSize <number>
 *    ...files smaller than <number> are never offloaded, since in many
 *    cases it's just as easy to serve them from the base server without
 *    any load spike and less margin of error. This defaults to 5 kilobytes
 *    if not set, but can be set higher or lower. Zero will remove any
 *    minimum size check.
 *
 *  OffloadExcludeMimeType <pattern>
 *    ...files with mimetypes matching <pattern> are never offloaded.
 *    This can be a wildcard pattern, so both "text/html" and "text/h*" are
 *    valid here.
 *
 *  OffloadExcludeUserAgent <pattern>
 *    ...clients with User-Agent fields matching <pattern> are never offloaded.
 *    This can be a wildcard pattern.
 *
 *  OffloadExcludeAddress <pattern>
 *    ...clients from an IP address matching <pattern> are never 
 *    offloaded. This can be a wildcard pattern.
 *
 * For URLs where mod_offload is in effect, it'll go through a checklist.
 *  Anything in the checklist that fails means that offloading shouldn't
 *  occur and Apache should handle this request as it would without
 *  mod_offload installed at all.
 *
 *  - Is OffloadEngine On?
 *  - Is there at least one OffloadHost?
 *  - Is this a GET method (as opposed to HEAD, POST, etc)?
 *  - Is this a request without args (potentially static content)?
 *  - Is this the last Apache content handler (potentially static content)?
 *  - Is the desired file really there?
 *  - Is the desired file more than OffloadMinSize?
 *  - Is the request from someone other than an offload server?
 *  - Is the request not explicitly trying to bypass offloading?
 *  - Is the desired file's mimetype not listed in OffloadExcludeMimeType?
 *  - Is the client's User-Agent not listed in OffloadExcludeUserAgent?
 *  - Is the client's IP address not listed in OffloadExcludeAddress?
 *
 * If the module makes it all the way through the checklist, it picks a
 *  random offload server (the server is chosen by the current
 *  time of day, with the chosen server changing once per second,
 *  rotating through the list of OffloadHost directives) and
 *  makes a 307 Redirect response to the client, pointing them to the
 *  offload server where they will receive the file.
 *
 *  This file written by Ryan C. Gordon (icculus@icculus.org).
 */

#include "ap_config.h"
#include "httpd.h"
#include "http_request.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"

#define MOD_OFFLOAD_VER "1.0.3"
#define DEFAULT_MIN_OFFLOAD_SIZE (5 * 1024)
#define VERSION_COMPONENT "mod_offload/"MOD_OFFLOAD_VER

/* some Apache 1.3/2.0 compatibility glue... */
#ifndef STANDARD20_MODULE_STUFF
   module MODULE_VAR_EXPORT offload_module;
#  define TARGET_APACHE_1_3 1
#  include "fnmatch.h"
#  define wild_match(p,s) (ap_fnmatch(p, s, FNM_CASE_BLIND) == 0)
#  define REQ_AUTH_TYPE(r) (r)->connection->ap_auth_type
#  define FINFO_MODE(r) (r)->finfo.st_mode
#  define FINFO_SIZE(r) (r)->finfo.st_size
#  define apr_vsnprintf(a,b,c,d) ap_vsnprintf(a,b,c,d)
#  define apr_table_get(a,b) ap_table_get(a,b)
#  define apr_table_setn(a,b,c) ap_table_setn(a,b,c)
#  define apr_pstrcat(a,b,c,d,e) ap_pstrcat(a,b,c,d,e)
#  define apr_pstrdup(a,b) ap_pstrdup(a,b)
#  define apr_palloc(a,b) ap_palloc(a,b)
#  define apr_array_make(a,b,c) ap_make_array(a,b,c)
#  define apr_array_push(a) ap_push_array(a)
#  define AP_INIT_FLAG(a,b,c,d,e) { a,b,c,d,FLAG,e }
#  define AP_INIT_TAKE1(a,b,c,d,e) { a,b,c,d,TAKE1,e }
   typedef struct in_addr apr_sockaddr_t;
   typedef array_header apr_array_header_t;
   typedef pool apr_pool_t;
   typedef table apr_table_t;
#else
   module AP_MODULE_DECLARE_DATA offload_module;
#  include "apr_strings.h"
#  include "apr_lib.h"
#  include "apr_fnmatch.h"
#  define wild_match(p,s) (apr_fnmatch(p,s,APR_FNM_CASE_BLIND) == APR_SUCCESS)
#  define REQ_AUTH_TYPE(r) (r)->ap_auth_type
#  define FINFO_MODE(r) (r)->finfo.protection
#  define FINFO_SIZE(r) (r)->finfo.size
#endif


typedef struct
{
    int offload_engine_on;
    int offload_debug;
    int offload_min_size;
    apr_array_header_t *offload_hosts;
    apr_array_header_t *offload_ips;
    apr_array_header_t *offload_exclude_mime;
    apr_array_header_t *offload_exclude_agents;
    apr_array_header_t *offload_exclude_addr;
} offload_dir_config;


static void debugLog(const request_rec *r, const offload_dir_config *cfg,
                     const char *fmt, ...)
{
    if (cfg->offload_debug)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        apr_vsnprintf(buf, sizeof (buf), fmt, ap);
        va_end(ap);
        ap_log_rerror(APLOG_MARK,
                      APLOG_NOERRNO|APLOG_ERR,
                      #if !TARGET_APACHE_1_3
                      APR_SUCCESS,
                      #endif
                      r, "mod_offload: %s", buf);
    } /* if */
} /* debugLog */


static int offload_handler(request_rec *r)
{
    int i = 0;
    apr_sockaddr_t *list = NULL;
    offload_dir_config *cfg = NULL;
    char *uri = NULL;
    int nelts = 0;
    int idx = 0;
    char *offload_host = NULL;
    const char *user_agent = NULL;
    const char *bypass = NULL;

    cfg = (offload_dir_config *) ap_get_module_config(r->per_dir_config,
                                                      &offload_module);

    /*
     * All these DECLINED returns means "pass to the next handler", which
     *  may just be an immediate failure, 404, etc, but then it behaves how
     *  you'd expect your Apache to under normal circumstances.
     */

    /* is OffloadEngine disabled? DECLINED */
    if (!cfg->offload_engine_on) {
        debugLog(r, cfg, "OffloadEngine is Off");
        return DECLINED;
    } /* if */

    /* are there no offload servers defined? DECLINED */
    nelts = cfg->offload_hosts->nelts;
    if (nelts == 0) {
        debugLog(r, cfg, "No offload hosts defined");
        return DECLINED;
    } /* if */

    /* is it a HEAD request? DECLINED */
    if (r->header_only) {
        debugLog(r, cfg, "HEAD request for URI '%s'", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is it not a GET? DECLINED */
    if (r->method_number != M_GET) {
        debugLog(r, cfg, "Not a GET method for URI '%s'", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* are there any args? DECLINED */
    if (r->args != NULL) {
        debugLog(r, cfg, "URI '%s' has args...dynamic?", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is there a password? DECLINED */
    if (REQ_AUTH_TYPE(r) != NULL) {
        debugLog(r, cfg, "URI '%s' requires auth", r->unparsed_uri);
        return DECLINED;
    } /* if */

    #if TARGET_APACHE_1_3   /* we just insert as the last hook in 2.0 API. */
    /* is there any dynamic content handler? DECLINED */
    if (r->handler != NULL) {
        debugLog(r, cfg, "URI '%s' has handler '%s'.",
                 r->unparsed_uri, r->handler);
        return DECLINED;
    } /* if */
    #endif

    /* is file missing? DECLINED */
    if ((FINFO_MODE(r) == 0) || (r->path_info && *r->path_info)) {
        debugLog(r, cfg, "File '%s' missing", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is file less than so-and-so? DECLINED */
    if (FINFO_SIZE(r) < cfg->offload_min_size) {
        debugLog(r, cfg, "File '%s' too small (%d is less than %d)",
                  r->unparsed_uri, (int) FINFO_SIZE(r), 
                  (int) cfg->offload_min_size);
        return DECLINED;
    } /* if */

    /* is this client's IP excluded from offloading? DECLINED */
    if (cfg->offload_exclude_addr->nelts)
    {
        char ipstr[256];
        #if TARGET_APACHE_1_3
        unsigned int x = (unsigned int) r->connection->remote_addr.sin_addr.s_addr;
        snprintf(ipstr, sizeof (ipstr), "%u.%u.%u.%u",
                 x & 0xFF, (x >> 8) & 0xFF, (x >> 16) & 0xFF, (x >> 24) & 0xFF);
        #else
        apr_sockaddr_ip_getbuf(ipstr, sizeof (ipstr), r->connection->remote_addr);
        #endif
        for (i = 0; i < cfg->offload_exclude_addr->nelts; i++)
        {
            char *ip = ((char **) cfg->offload_exclude_addr->elts)[i];
            if (wild_match(ip, ipstr))
            {
                debugLog(r, cfg,
                    "URI request '%s' from address '%s' is excluded from"
                    " offloading by address pattern '%s'",
                    r->unparsed_uri, ipstr, ip);
                return DECLINED;
            } /* if */
        } /* for */
    } /* if */

    /* is this request from one of the listed offload servers? DECLINED */
    list = (apr_sockaddr_t *) cfg->offload_ips->elts;
    for (i = 0; i < cfg->offload_ips->nelts; i++) {
        #if TARGET_APACHE_1_3
        int match=(r->connection->remote_addr.sin_addr.s_addr==list[i].s_addr);
        #else
        int match = apr_sockaddr_equal(r->connection->remote_addr, &list[i]);
        #endif
        if (match)
        {
            offload_host = ((char **) cfg->offload_hosts->elts)[i];
            debugLog(r, cfg, "Offload server (%s) doing cache refresh on '%s'",
                        offload_host, r->unparsed_uri);
            return DECLINED;
        } /* if */
    } /* for */

    /* Is this an explicit request to bypass offloading? DECLINED */
    bypass = (const char *) apr_table_get(r->headers_in, "X-Mod-Offload-Bypass");
    if (bypass) {
        debugLog(r, cfg, "Client explicitly bypassing offloading for '%s'",
                 r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is the file in the list of mimetypes to never offload? DECLINED */
    if ((r->content_type) && (cfg->offload_exclude_mime->nelts))
    {
        for (i = 0; i < cfg->offload_exclude_mime->nelts; i++)
        {
            char *mimetype = ((char **) cfg->offload_exclude_mime->elts)[i];
            if (wild_match(mimetype, r->content_type))
            {
                debugLog(r, cfg,
                    "URI '%s' (%s) is excluded from offloading"
                    " by mimetype pattern '%s'", r->unparsed_uri,
                    r->content_type, mimetype);
                return DECLINED;
            } /* if */
        } /* for */
    } /* if */

    /* is this User-Agent excluded from offloading (like Google)? DECLINED */
    user_agent = (const char *) apr_table_get(r->headers_in, "User-Agent");
    if ((user_agent) && (cfg->offload_exclude_agents->nelts))
    {
        for (i = 0; i < cfg->offload_exclude_agents->nelts; i++)
        {
            char *agent = ((char **) cfg->offload_exclude_agents->elts)[i];
            if (wild_match(agent, user_agent))
            {
                debugLog(r, cfg,
                    "URI request '%s' from agent '%s' is excluded from"
                    " offloading by User-Agent pattern '%s'",
                    r->unparsed_uri, user_agent, agent);
                return DECLINED;
            } /* if */
        } /* for */
    } /* if */

    /* We can offload this. Pick a random offload servers from defined list. */
    debugLog(r, cfg, "Offloading URI '%s'", r->unparsed_uri);
    idx = (int)(time(NULL) % nelts);
    offload_host = ((char **) cfg->offload_hosts->elts)[idx];
    debugLog(r, cfg, "Chose server #%d (%s)", idx, offload_host);

    /* Offload it: set a "Location:" header and 302 redirect. */
    uri = apr_pstrcat(r->pool, "http://", offload_host, r->unparsed_uri, NULL);
    debugLog(r, cfg, "Redirect from '%s' to '%s'", r->unparsed_uri, uri);

    apr_table_setn(r->headers_out, "Location", uri);
    return HTTP_TEMPORARY_REDIRECT;
} /* offload_handler */


static void *create_offload_dir_config(apr_pool_t *p, char *dummy)
{
    offload_dir_config *retval =
      (offload_dir_config *) apr_palloc(p, sizeof (offload_dir_config));

    retval->offload_engine_on = 0;
    retval->offload_debug = 0;
    retval->offload_hosts = apr_array_make(p, 0, sizeof (char *));
    retval->offload_exclude_mime = apr_array_make(p, 0, sizeof (char *));
    retval->offload_exclude_agents = apr_array_make(p, 0, sizeof (char *));
    retval->offload_exclude_addr = apr_array_make(p, 0, sizeof (char *));
    retval->offload_ips = apr_array_make(p, 0, sizeof (apr_sockaddr_t));
    retval->offload_min_size = DEFAULT_MIN_OFFLOAD_SIZE;
    
    return retval;
} /* create_offload_dir_config */


static const char *offload_engine(cmd_parms *parms, void *mconfig, int flag)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    cfg->offload_engine_on = flag;
    return NULL;  /* no error. */
} /* offload_engine */


static const char *offload_debug(cmd_parms *parms, void *mconfig, int flag)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    cfg->offload_debug = flag;
    return NULL;  /* no error. */
} /* offload_debug */


static const char *offload_host(cmd_parms *parms, void *mconfig,
                                const char *_arg)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    char **hostelem = (char **) apr_array_push(cfg->offload_hosts);
    apr_sockaddr_t *addr = (apr_sockaddr_t *) apr_array_push(cfg->offload_ips);
    char *ptr = NULL;
    char arg[512];

    apr_cpystrn(arg, _arg, sizeof (arg));
    ptr = strchr(arg, ':');
    if (ptr != NULL)
        *ptr = '\0';   /* chop off port number if it's there. */

    #if TARGET_APACHE_1_3
    struct hostent *hp = ap_pgethostbyname(parms->pool, arg);
    if (hp == NULL)
        return "DNS lookup failure!";
    memcpy(addr, (apr_sockaddr_t *) (hp->h_addr), sizeof (apr_sockaddr_t));
    #else
    apr_sockaddr_t *resolved = NULL;
    apr_status_t rc;
    rc = apr_sockaddr_info_get(&resolved, arg, APR_UNSPEC, 0, 0, parms->pool);
    if (rc != APR_SUCCESS)
        return "DNS lookup failure!";
    memcpy(addr, resolved, sizeof (apr_sockaddr_t));
    #endif

    *hostelem = apr_pstrdup(parms->pool, _arg);
    return NULL;  /* no error. */
} /* offload_host */


static const char *offload_minsize(cmd_parms *parms, void *mconfig,
                                   const char *arg)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    cfg->offload_min_size = atoi(arg);
    return NULL;  /* no error. */
} /* offload_minsize */


static const char *offload_excludemime(cmd_parms *parms, void *mconfig,
                                       const char *arg)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    char **mimepattern = (char **) apr_array_push(cfg->offload_exclude_mime);
    *mimepattern = apr_pstrdup(parms->pool, arg);
    return NULL;  /* no error. */
} /* offload_excludemime */


static const char *offload_excludeagent(cmd_parms *parms, void *mconfig,
                                        const char *arg)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    char **agentpattern = (char **) apr_array_push(cfg->offload_exclude_agents);
    *agentpattern = apr_pstrdup(parms->pool, arg);
    return NULL;  /* no error. */
} /* offload_excludeagent */


static const char *offload_excludeaddr(cmd_parms *parms, void *mconfig,
                                       const char *arg)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    char **addrpattern = (char **) apr_array_push(cfg->offload_exclude_addr);
    *addrpattern = apr_pstrdup(parms->pool, arg);
    return NULL;  /* no error. */
} /* offload_excludeaddr */


static const command_rec offload_cmds[] =
{
    AP_INIT_FLAG("OffloadEngine", offload_engine, NULL, OR_OPTIONS,
      "Set to On or Off to enable or disable offloading"),
    AP_INIT_FLAG("OffloadDebug", offload_debug, NULL, OR_OPTIONS,
      "Set to On or Off to enable or disable debug spam to error log"),
    AP_INIT_TAKE1("OffloadHost", offload_host, NULL, OR_OPTIONS,
      "Hostname or IP address of offload server"),
    AP_INIT_TAKE1("OffloadMinSize", offload_minsize, NULL, OR_OPTIONS,
      "Minimum size, in bytes, that a file must be to be offloaded"),
    AP_INIT_TAKE1("OffloadExcludeMimeType",offload_excludemime,0,OR_OPTIONS,
      "Mimetype to always exclude from offloading (wildcards allowed)"),
    AP_INIT_TAKE1("OffloadExcludeUserAgent",offload_excludeagent,0,OR_OPTIONS,
      "User-Agent to always exclude from offloading (wildcards allowed)"),
    AP_INIT_TAKE1("OffloadExcludeAddress",offload_excludeaddr,0,OR_OPTIONS,
      "IP address to always exclude from offloading (wildcards allowed)"),
    { NULL }
};


/* Tell Apache what phases of the transaction we handle */
#if TARGET_APACHE_1_3
static void init_offload(server_rec *s, apr_pool_t *p)
{
    ap_add_version_component(VERSION_COMPONENT);
} /* init_offload */

/* Make the name of the content handler known to Apache */
static handler_rec offload_handlers[] =
{
    { "*/*", offload_handler },
    {"offload-handler", offload_handler},
    { NULL , NULL }
};

module MODULE_VAR_EXPORT offload_module =
{
    STANDARD_MODULE_STUFF,
    init_offload,               /* module initializer                 */
    create_offload_dir_config,  /* per-directory config creator       */
    NULL,                       /* dir config merger                  */
    NULL,                       /* server config creator              */
    NULL,                       /* server config merger               */
    offload_cmds,               /* command table                      */
    offload_handlers,           /* [9]  content handlers              */
    NULL,                       /* [2]  URI-to-filename translation   */
    NULL,                       /* [5]  check/validate user_id        */
    NULL,                       /* [6]  check user_id is valid *here* */
    NULL,                       /* [4]  check access by host address  */
    NULL,                       /* [7]  MIME type checker/setter      */
    NULL,                       /* [8]  fixups                        */
    NULL,                       /* [10] logger                        */
    NULL,                       /* [3]  header parser                 */
    NULL,                       /* process initialization             */
    NULL,                       /* process exit/cleanup               */
    NULL                        /* [1]  post read_request handling    */
};

#else  /* Apache 2.0 module API ... */

int offload_init(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp,
                 server_rec *base_server)
{
    ap_add_version_component(p, VERSION_COMPONENT);
    return OK;
} /* init_offload */

static void offload_register_hooks(apr_pool_t *p)
{
    ap_hook_post_config(offload_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(offload_handler, NULL, NULL, APR_HOOK_LAST);
} /* offload_register_hooks */

module AP_MODULE_DECLARE_DATA offload_module =
{
    STANDARD20_MODULE_STUFF,
    create_offload_dir_config,  /* create per-directory config structures */
    NULL,                       /* merge per-directory config structures  */
    NULL,                       /* create per-server config structures    */
    NULL,                       /* merge per-server config structures     */
    offload_cmds,               /* command handlers */
    offload_register_hooks      /* register hooks */
};
#endif

/* end of mod_offload.c ... */

