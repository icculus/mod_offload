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
 *    This can be a wildcard pattern, so both "text/html" and "text/*" are
 *    valid here.
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
 *  - Is the desired file's mimetype not listed in OffloadExcludeMimeType?
 *  - Is the request from someone other than an offload server?
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

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_protocol.h"
#include "fnmatch.h"

#define MOD_OFFLOAD_VERSION "0.0.4"
#define DEFAULT_MIN_OFFLOAD_SIZE (5 * 1024)

module MODULE_VAR_EXPORT offload_module;

typedef struct
{
    int offload_engine_on;
    int offload_debug;
    int offload_min_size;
    array_header *offload_hosts;
    array_header *offload_ips;
    array_header *offload_exclude_mime;
} offload_dir_config;


static void debugLog(request_rec *r, const offload_dir_config *cfg,
                     const char *fmt, ...)
{
    if (cfg->offload_debug)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        ap_vsnprintf(buf, sizeof (buf), fmt, ap);
        va_end(ap);
        ap_log_error(APLOG_MARK,
                     APLOG_NOERRNO|APLOG_ERR,
                     r->server, "mod_offload: %s", buf);
    } /* if */
} /* debugLog */


static int offload_handler(request_rec *r)
{
    int i = 0;
    struct in_addr *list = NULL;
    offload_dir_config *cfg = NULL;
    char *uri = NULL;
    int nelts = 0;
    int idx = 0;
    char *offload_host = NULL;

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
    if (r->connection->ap_auth_type != NULL) {
        debugLog(r, cfg, "URI '%s' requires auth", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is there any dynamic content handler? DECLINED */
    if (r->handler != NULL) {
        debugLog(r, cfg, "URI '%s' has handler", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is file missing? DECLINED */
    if ((r->finfo.st_mode == 0) || (r->path_info && *r->path_info)) {
        debugLog(r, cfg, "File '%s' missing", r->unparsed_uri);
        return DECLINED;
    } /* if */

    /* is file less than so-and-so? DECLINED */
    if (r->finfo.st_size < cfg->offload_min_size) {
        debugLog(r, cfg, "File '%s' too small (%d is less than %d)",
                  r->unparsed_uri, r->finfo.st_size, cfg->offload_min_size);
        return DECLINED;
    } /* if */

    /* is this request from one of the listed offload servers? DECLINED */
    list = (struct in_addr *) cfg->offload_ips->elts;
    for (i = 0; i < cfg->offload_ips->nelts; i++) {
        if (r->connection->remote_addr.sin_addr.s_addr == list[i].s_addr) {
            offload_host = ((char **) cfg->offload_hosts->elts)[i];
            debugLog(r, cfg, "Offload server (%s) doing cache refresh on '%s'",
                        offload_host, r->unparsed_uri);
            return DECLINED;
        } /* if */
    } /* for */

    /* is the file in the list of mimetypes to never offload? DECLINED */
    if ((r->content_type) && (cfg->offload_exclude_mime->nelts))
    {
        for (i = 0; i < cfg->offload_exclude_mime->nelts; i++)
        {
            char *mimetype = ((char **) cfg->offload_exclude_mime->elts)[i];
            if (ap_fnmatch(mimetype, r->content_type, FNM_CASE_BLIND) == 0) {
                debugLog(r, cfg,
                    "URI '%s' (%s) is excluded from offloading"
                    " by mimetype pattern '%s'", r->unparsed_uri,
                    r->content_type, mimetype);
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
    uri = ap_pstrcat(r->pool, "http://", offload_host, r->unparsed_uri, NULL);
    debugLog(r, cfg, "Redirect from '%s' to '%s'", r->unparsed_uri, uri);

    ap_table_setn(r->headers_out, "Location", uri);
    return HTTP_TEMPORARY_REDIRECT;
} /* offload_handler */


static void *create_offload_dir_config(pool *p, char *dummy)
{
    offload_dir_config *retval =
      (offload_dir_config *) ap_palloc(p, sizeof(offload_dir_config));

    retval->offload_engine_on = 0;
    retval->offload_debug = 0;
    retval->offload_hosts = ap_make_array(p, 0, sizeof (char *));
    retval->offload_exclude_mime = ap_make_array(p, 0, sizeof (char *));
    retval->offload_ips = ap_make_array(p, 0, sizeof (struct in_addr));
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
                                const char *arg)
{
    offload_dir_config *cfg = (offload_dir_config *) mconfig;
    char **hostelem = (char **) ap_push_array(cfg->offload_hosts);
    struct in_addr *addr = (struct in_addr *) ap_push_array(cfg->offload_ips);
    struct hostent *hp = ap_pgethostbyname(parms->pool, arg);
    if (hp == NULL)
        return "DNS lookup failure!";

    memcpy(addr, (struct in_addr *) (hp->h_addr), sizeof (struct in_addr));
    *hostelem = ap_pstrdup(parms->pool, arg);
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
    char **mimepattern = (char **) ap_push_array(cfg->offload_exclude_mime);
    *mimepattern = ap_pstrdup(parms->pool, arg);
    return NULL;  /* no error. */
} /* offload_host */


static void init_offload(server_rec *s, pool *p)
{
    ap_add_version_component("mod_offload/"MOD_OFFLOAD_VERSION);
} /* init_offload */


static command_rec offload_cmds[] = {
{ "OffloadEngine", offload_engine, NULL, OR_OPTIONS, FLAG,
    "Set to On or Off to enable or disable offloading" },
{ "OffloadDebug", offload_debug, NULL, OR_OPTIONS, FLAG,
    "Set to On or Off to enable or disable debug spam to error log" },
{ "OffloadHost", offload_host, NULL, OR_OPTIONS, TAKE1,
    "Hostname or IP address of offload server" },
{ "OffloadMinSize", offload_minsize, NULL, OR_OPTIONS, TAKE1,
    "Minimum size, in bytes, that a file must be to be offloaded" },
{ "OffloadExcludeMimeType", offload_excludemime, NULL, OR_OPTIONS, TAKE1,
    "Mimetype to always exclude from offloading (wildcards allowed)" },
{ NULL }
};


/* Make the name of the content handler known to Apache */
static handler_rec offload_handlers[] =
{
    { "*/*", offload_handler },
    {"offload-handler", offload_handler},
    { NULL , NULL }
};


/* Tell Apache what phases of the transaction we handle */
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

/* end of mod_offload.c ... */

