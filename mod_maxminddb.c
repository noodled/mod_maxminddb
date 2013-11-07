/* maxminddb module
 *
 * Version 0.1
 *
 * This module sets an environment variable to the remote country
 * based on the requestor's IP address.  It uses the maxminddb library
 * to lookup the country by IP address.
 *
 * Copyright 2013, MaxMind Inc.
 *
 */

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_log.h"
#include "ap_config.h"
#include "apr_strings.h"
#include "util_script.h"
#include <netdb.h>
#include <arpa/inet.h>
#include "MMDB.h"
#include <string.h>
#include <alloca.h>

#define INFO(server_rec, ...) \
                    ap_log_error(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, server_rec, "[mod_maxminddb]: " __VA_ARGS__);

typedef struct key_value_list_s {
    const char *path;
    const char *env_key;
    struct key_value_list_s *next;
} key_value_list_s;

typedef struct {
    const char *filename;
    int enabled;
    int flags;
    key_value_list_s *next;
} maxminddb_config;

typedef struct {
    maxminddb_config mmcfg;
} maxminddb_server_config_rec;

typedef maxminddb_server_config_rec maxminddb_dir_config_rec;

module AP_MODULE_DECLARE_DATA maxminddb_module;

static void set_env_for_ip(request_rec * r, const char *filename,
                           const char *ipaddr);

static void init_maxminddb_config(maxminddb_config * cfg)
{
    cfg->enabled = 0;
    cfg->flags = 0;
    cfg->filename = NULL;
    cfg->next = NULL;
}

/* create a disabled directory entry */

static void *create_dir_config(apr_pool_t * p, char *d)
{

    maxminddb_dir_config_rec *dcfg;

    dcfg =
        (maxminddb_dir_config_rec *) apr_pcalloc(p,
                                                 sizeof
                                                 (maxminddb_dir_config_rec));
    init_maxminddb_config(&dcfg->mmcfg);

    return dcfg;
}

static void *merge_dir_config(apr_pool_t * p, void *parent, void *cur)
{
    return cur;
}

/* create a standard disabled server entry */

static void *create_server_config(apr_pool_t * p, server_rec * srec)
{
    maxminddb_server_config_rec *conf =
        apr_pcalloc(p, sizeof(maxminddb_server_config_rec));
    if (!conf) {
        return NULL;
    }

    init_maxminddb_config(&conf->mmcfg);
    INFO(srec, "create_server_config");

    return (void *)conf;
}

static apr_status_t cleanup(void *cfgdata)
{
    int i;
    maxminddb_server_config_rec *cfg = (maxminddb_server_config_rec *) cfgdata;
    return APR_SUCCESS;
}

/* initialize maxminddb once per server ( even virtal server! ) */
static void server_init(apr_pool_t * p, server_rec * s)
{
    maxminddb_server_config_rec *cfg;
    cfg = (maxminddb_server_config_rec *)
        ap_get_module_config(s->module_config, &maxminddb_module);

    apr_pool_cleanup_register(p, (void *)cfg, cleanup, cleanup);
    INFO(s, "server_init");

}

static void child_init(apr_pool_t * p, server_rec * s)
{
    maxminddb_server_config_rec *cfg;
    int i, flags;

    INFO(s, "child_init");

    cfg = (maxminddb_server_config_rec *)
        ap_get_module_config(s->module_config, &maxminddb_module);

}

/* map into the first apache */
static int post_config(apr_pool_t * p, apr_pool_t * plog,
                       apr_pool_t * ptemp, server_rec * s)
{
    INFO(s, "post_config");
    server_init(p, s);
    return OK;
}

static int maxminddb_header_parser(request_rec * r, maxminddb_config * mmcfg);

static int maxminddb_post_read_request(request_rec * r)
{
    maxminddb_server_config_rec *cfg;
    cfg = ap_get_module_config(r->server->module_config, &maxminddb_module);

    INFO(r->server, "maxminddb_post_read_request");
    if (!cfg)
        return DECLINED;

    if (!cfg->mmcfg.enabled)
        return DECLINED;

    return maxminddb_header_parser(r, &cfg->mmcfg);
}

static int maxminddb_per_dir(request_rec * r)
{

    maxminddb_dir_config_rec *dcfg;
    INFO(r->server, "maxminddb_per_dir");

    dcfg = ap_get_module_config(r->per_dir_config, &maxminddb_module);
    if (!dcfg)
        return DECLINED;

    INFO(r->server, "maxminddb_per_dir config exists");

    if (!dcfg->mmcfg.enabled)
        return DECLINED;

    INFO(r->server, "maxminddb_per_dir ( enabled )");
    return maxminddb_header_parser(r, &dcfg->mmcfg);
}

char *_get_client_ip(request_rec * r)
{
# if AP_SERVER_MAJORVERSION_NUMBER == 2 && AP_SERVER_MINORVERSION_NUMBER == 4
    return r->useragent_ip;
# else
    return r->connection->remote_ip;
#endif
}

static int maxminddb_header_parser(request_rec * r, maxminddb_config * mmcfg)
{
    char *ipaddr;
    char *free_me = NULL;
    char *ipaddr_ptr = NULL;

    ipaddr = _get_client_ip(r);
    INFO(r->server, "maxminddb_header_parser %s", ipaddr);

    if (!mmcfg || !mmcfg->filename || !mmcfg->enabled)
        return DECLINED;

    set_env_for_ip(r, mmcfg->filename, ipaddr);
    return OK;
}

void set_string(request_rec * r, MMDB_entry_s * entry, const char *env, ...)
{
    va_list keys;
    MMDB_return_s result;
    if (!entry->offset)
        return;
    va_start(keys, env);
    MMDB_s *mmdb = entry->mmdb;
    MMDB_vget_value(entry, &result, keys);
    if (result.offset) {
        uint32_t segments = mmdb->full_record_size_bytes * mmdb->node_count;
        char *value = alloca(result.data_size + 1);
        MMDB_pread(mmdb->fd, value, result.data_size,
                   segments + (off_t) (void *)result.ptr);
        value[result.data_size] = 0;
        apr_table_set(r->subprocess_env, env, value);
    }
    va_end(keys);
}

void set_double(request_rec * r, MMDB_entry_s * entry, const char *env, ...)
{
    va_list keys;
    MMDB_return_s result;
    if (!entry->offset)
        return;
    va_start(keys, env);
    MMDB_vget_value(entry, &result, keys);
    if (result.offset) {
        char *value;
        asprintf(&value, "%.5f", result.double_value);
        if (value) {
            apr_table_set(r->subprocess_env, env, value);
            free(value);
        }
    }
    va_end(keys);
}

#define K(...) __VA_ARGS__, NULL

static void set_env_for_ip(request_rec * r, const char *filename,
                           const char *ipaddr)
{
    struct in6_addr v6;
    apr_table_set(r->subprocess_env, "GEOIP_ADDR", ipaddr);
    MMDB_s *mmdb = MMDB_open(filename, MMDB_MODE_STANDARD);
    MMDB_root_entry_s root = {.entry.mmdb = mmdb };

    if (!mmdb)
        return;

    int ai_family = AF_INET6;
    int ai_flags = AI_V4MAPPED;

    if ((ipaddr != NULL)
        && (0 == MMDB_lookupaddressX(ipaddr, ai_family, ai_flags, &v6))) {

        int status = MMDB_lookup_by_ipnum_128(v6, &root);
        if (status == MMDB_SUCCESS && root.entry.offset > 0) {

            MMDB_return_s result;
            MMDB_get_value(&root.entry, &result, K("location"));
            MMDB_entry_s location = {.mmdb = root.entry.mmdb,.offset =
                    result.offset
            };
            set_double(r, &location, "GEOIP_LATITUDE", K("latitude"));
            set_double(r, &location, "GEOIP_LONGITUDE", K("longitude"));
            set_string(r, &location, "GEOIP_METRO_CODE", K("metro_code"));
            set_string(r, &location, "GEOIP_TIME_ZONE", K("time_zone"));

            MMDB_get_value(&root.entry, &result, K("continent"));
            location.offset = result.offset;
            set_string(r, &location, "GEOIP_CONTINENT_CODE", K("code"));
            set_string(r, &location, "GEOIP_CONTINENT_NAME", K("names", "en"));

            MMDB_get_value(&root.entry, &result, K("country"));
            location.offset = result.offset;
            set_string(r, &location, "GEOIP_COUNTRY_CODE", K("iso_code"));
            set_string(r, &location, "GEOIP_COUNTRY_NAME", K("names", "en"));

            MMDB_get_value(&root.entry, &result, K("registered_country"));
            location.offset = result.offset;
            set_string(r, &location, "GEOIP_REGISTERED_COUNTRY_CODE",
                       K("iso_code"));
            set_string(r, &location, "GEOIP_REGISTERED_COUNTRY_NAME",
                       K("names", "en"));

            MMDB_get_value(&root.entry, &result, K("subdivisions", "0"));
            location.offset = result.offset;
            set_string(r, &location, "GEOIP_REGION_CODE", K("iso_code"));
            set_string(r, &location, "GEOIP_REGION_NAME", K("names", "en"));

            set_string(r, &root.entry, "GEOIP_CITY", K("city", "names", "en"));
            set_string(r, &root.entry, "GEOIP_POSTAL_CODE",
                       K("postal", "code"));
        }
    }
    MMDB_close(mmdb);
}

static const char *set_maxminddb_enable(cmd_parms * cmd, void *dummy, int arg)
{
    /* is per directory config? */
    if (cmd->path) {
        maxminddb_dir_config_rec *dcfg = dummy;
        dcfg->mmcfg.enabled = arg;

        INFO(cmd->server, "set_maxminddb_enable: (dir) %d", arg);

        return NULL;
    }
    /* no then it is server config */
    maxminddb_server_config_rec *conf = (maxminddb_server_config_rec *)
        ap_get_module_config(cmd->server->module_config, &maxminddb_module);

    if (!conf)
        return "mod_maxminddb: server structure not allocated";

    conf->mmcfg.enabled = arg;
    INFO(cmd->server, "set_maxminddb_enable: (server) %d", arg);

    return NULL;
}

static const char *set_maxminddb_filename(cmd_parms * cmd, void *dummy,
                                          const char *filename,
                                          const char *arg2)
{
    int i;

    if (cmd->path) {
        maxminddb_dir_config_rec *dcfg = dummy;
        dcfg->mmcfg.filename = filename;

        INFO(cmd->server, "set_maxminddb_filename (dir) %s", filename);

        return NULL;
    }

    maxminddb_server_config_rec *conf = (maxminddb_server_config_rec *)
        ap_get_module_config(cmd->server->module_config, &maxminddb_module);

    if (!filename)
        return NULL;

    conf->mmcfg.filename = (char *)apr_pstrdup(cmd->pool, filename);
    INFO(cmd->server, "set_maxminddb_filename (server) %s", filename);

    return NULL;
}

static void insert_kvlist(maxminddb_config * mmcfg, key_value_list_s * list)
{

    list->next = mmcfg->next;
    mmcfg->next = list;
}

static const char *set_maxminddb_env(cmd_parms * cmd, void *dummy,
                                     const char *dbpath, const char *env)
{
    int i;

    key_value_list_s *list = apr_palloc(cmd->pool, sizeof(key_value_list_s));
    list->path = dbpath;
    list->env_key = env;
    list->next = NULL;

    if (cmd->path) {
        maxminddb_dir_config_rec *dcfg = dummy;

        INFO(cmd->server, "set_maxminddb_env (dir) %s %s", dbpath, env);
        insert_kvlist(&dcfg->mmcfg, list);

        return NULL;
    }

    maxminddb_server_config_rec *conf = (maxminddb_server_config_rec *)
        ap_get_module_config(cmd->server->module_config, &maxminddb_module);

    INFO(cmd->server, "set_maxminddb_env (server) %s %s", dbpath, env);

    insert_kvlist(&conf->mmcfg, list);

    return NULL;
}

static const command_rec maxminddb_cmds[] = {
    AP_INIT_FLAG("MaxMindDBEnable", set_maxminddb_enable, NULL,
                 OR_FILEINFO, "Turn on mod_maxminddb"),
    AP_INIT_TAKE12("MaxMindDBFile", set_maxminddb_filename, NULL,
                   OR_ALL, "Path to the Database File"),
    AP_INIT_ITERATE2("MaxMindDBEnv", set_maxminddb_env, NULL,
                     OR_ALL, "Set desired env var"),
    {NULL}
};

static void maxminddb_register_hooks(apr_pool_t * p)
{
    /* make sure we run before mod_rewrite's handler */
    static const char *const aszSucc[] =
        { "mod_setenvif.c", "mod_rewrite.c", NULL };

    /* we have two entry points, the header_parser hook, right before
     * the authentication hook used for Dirctory specific enabled maxminddblookups
     * or right before directory rewrite rules.
     */
    ap_hook_header_parser(maxminddb_per_dir, NULL, aszSucc, APR_HOOK_MIDDLE);

    /* and the servectly wide hook, after reading the request. Perfecly
     * suitable to serve serverwide mod_rewrite actions
     */
    ap_hook_post_read_request(maxminddb_post_read_request, NULL, aszSucc,
                              APR_HOOK_MIDDLE);

    /* setup our childs maxminddb database once for every child */
    ap_hook_child_init(child_init, NULL, NULL, APR_HOOK_MIDDLE);

    /* static const char * const list[]={ "mod_maxminddb.c", NULL }; */
    /* mmap the database(s) into the master process */
    ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);

}

/* Dispatch list for API hooks */
AP_DECLARE_MODULE(maxminddb) = {
    STANDARD20_MODULE_STUFF,    /* */
        create_dir_config,      /* create per-dir    config structures */
        merge_dir_config,       /* merge  per-dir    config structures */
        create_server_config,   /* create per-server config structures */
        NULL,                   /* merge  per-server config structures */
        maxminddb_cmds,         /* table of config file commands       */
        maxminddb_register_hooks        /* register hooks                      */
};

static void set_env_for_ip_conf(request_rec * r, const maxminddb_config * mmcfg,
                                const char *ipaddr)
{

    struct in6_addr v6;
    apr_table_set(r->subprocess_env, "GEOIP_ADDR", ipaddr);
    MMDB_s *mmdb = MMDB_open(filename, MMDB_MODE_STANDARD);
    MMDB_root_entry_s root = {.entry.mmdb = mmdb };

    if (!mmdb)
        return;

    int ai_family = AF_INET6;
    int ai_flags = AI_V4MAPPED;

    if ((ipaddr != NULL)
        && (0 == MMDB_lookupaddressX(ipaddr, ai_family, ai_flags, &v6))) {

        int status = MMDB_lookup_by_ipnum_128(v6, &root);
        if (status == MMDB_SUCCESS && root.entry.offset > 0) {

            for (key_value_list_s * key_value = mmcfg->next; key_value;
                 key_value = key_value->next) {
                set_env(r, mmdb, &root, key_value);
            }

        }
    }
}

static void set_env(request_rec * r, MMDB_s * mmdb, MMDB_root_entry_s * root,
                    key_value_list_s * key_value)
{

    const int max_list = 80;
    char *list[max_list + 1];
    int i;
    char *ptr, *cur, *tok;
    cur = ptr = strdup(key_value->path);
    for (i = 0; i < max_list; i++) {
        list[i] = strsep(&cur, "/");
        if (*list[i] == '\0') {
            --i;
        }
    }
    list[i] = NULL;
    MMDB_return_s result;
    MMDB_vget_value(&root.entry, &result, list);
    if (root.entry.offset > 0) {
        setenv(key_value->env_key, "123", 1);
    }

    free(ptr);
}
