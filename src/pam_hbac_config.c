/*
    Copyright (C) 2012 Jakub Hrozek <jakub.hrozek@posteo.se>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include "pam_hbac.h"
#include "pam_hbac_compat.h"
#include "config.h"

#define MAX_LINE    1024
#define SEPARATOR   '='

void
ph_cleanup_config(struct pam_hbac_config *conf)
{
    if (conf == NULL) {
        return;
    }

    free_const(conf->uri);
    free_const(conf->search_base);
    free(conf->hostname);

    free(conf);
}

static struct pam_hbac_config *
default_config(struct pam_hbac_config *conf)
{
    int ret;

    if (conf->uri == NULL) {
        conf->uri = strdup(PAM_HBAC_DEFAULT_URI);
    }
    if (conf->search_base == NULL) {
        conf->search_base = strdup(PAM_HBAC_DEFAULT_SEARCH_BASE);
    }

    if (conf->search_base == NULL ||
            conf->uri == NULL) {
        goto fail;
    }

    if (conf->hostname == NULL) {
        conf->hostname = malloc(HOST_NAME_MAX);
        if (conf->hostname == NULL) {
            goto fail;
        }

        ret = gethostname(conf->hostname, HOST_NAME_MAX);
        if (ret == -1) {
            ret = errno;
            goto fail;
        }
        conf->hostname[HOST_NAME_MAX-1] = '\0';
    }

    if (conf->timeout == 0) {
        conf->timeout = PAM_HBAC_DEFAULT_TIMEOUT;
    }

    return conf;

fail:
    ph_cleanup_config(conf);
    return NULL;
}

static char *
strip(char *s)
{
    char *start, *end;

    start = s;
    end = s + strlen(s) - 1;

    /* Trim leading whitespace */
    while (*start && isspace(*start)) ++start;
    /* Trim trailing whitespace */
    while (end > start && isspace(*end)) *end-- = '\0';

    return start;
}

static int
get_key_value(pam_handle_t *pamh,
              const char *line,
              const char **_key,
              const char **_value)
{
    char *sep;
    const char *key;
    char *value;
    char *l;

    sep = strchr(line, SEPARATOR);
    if (sep == NULL) {
        logger(pamh, LOG_ERR, "Malformed line; no separator\n");
        return EINVAL;
    }

    l = strdup(line);
    l[sep-line] = '\0';
    key = strdup(strip(l));
    value = strdup(strip(sep+1));
    free(l);
    if (!key || !value) {
        return ENOMEM;
    }

    *_key = key;
    *_value = value;
    return 0;
}

static int
read_config_line(pam_handle_t *pamh,
                 const char *line,
                 struct pam_hbac_config *conf)
{
    const char *key = NULL;
    const char *value = NULL;
    const char *l;
    int ret;

    l = line;

    /* Skip leading whitespace */
    while(isspace(*l)) {
        ++l;
    }

    /* Skip comments */
    if (*l == '#') {
        ret = EAGAIN;
        goto fail;
    }

    ret = get_key_value(pamh, l, &key, &value);
    if (ret) {
        logger(pamh, LOG_ERR,
               "Cannot split \"%s\" into a key-value pair [%d]: %s\n",
               l, ret, strerror(ret));
        goto fail;
    }

    if (strcasecmp(key, PAM_HBAC_CONFIG_URI) == 0) {
        conf->uri = value;
        logger(pamh, LOG_DEBUG, "URI: %s", conf->uri);
    } else if (strcasecmp(key, PAM_HBAC_CONFIG_BIND_DN) == 0) {
        conf->bind_dn = value;
        logger(pamh, LOG_DEBUG, "bind dn: %s", conf->bind_dn);
    } else if (strcasecmp(key, PAM_HBAC_CONFIG_BIND_PW) == 0) {
        conf->bind_pw = value;
    } else if (strcasecmp(key, PAM_HBAC_CONFIG_SEARCH_BASE) == 0) {
        conf->search_base = value;
        logger(pamh, LOG_DEBUG, "search base: %s", conf->search_base);
    } else if (strcasecmp(key, PAM_HBAC_CONFIG_HOST_NAME) == 0) {
        conf->hostname = discard_const(value);
        logger(pamh, LOG_DEBUG, "host name: %s", conf->hostname);
    } else {
        /* Skip unknown key/values */
        free_const(value);
    }

    free_const(key);
    return 0;

fail:
    logger(pamh, LOG_CRIT,
           "cannot read config [%d]: %s\n", ret, strerror(ret));
    free_const(key);
    free_const(value);
    return ret;
}

int
ph_read_config(pam_handle_t *pamh,
               const char *config_file,
               struct pam_hbac_config **_conf)
{
    FILE *fp;
    int ret;
    char line[MAX_LINE];
    struct pam_hbac_config *conf;

    logger(pamh, LOG_DEBUG, "config file: %s", config_file);

    errno = 0;
    fp = fopen(config_file, "r");
    if (fp == NULL) {
        /* According to PAM Documentation, such an error in a config file
         * SHOULD be logged at LOG_ALERT level
         */
        ret = errno;
        logger(pamh, LOG_ALERT,
               "pam_hbac: cannot open config file %s [%d]: %s\n",
               config_file, ret, strerror(ret));
        return ret;
    }

    conf = calloc(1, sizeof(struct pam_hbac_config));
    if (conf == NULL) {
        ret = ENOMEM;
        goto done;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* Try to parse a line */
        ret = read_config_line(pamh, line, conf);
        if (ret == EAGAIN) {
            continue;
        } else if (ret != 0) {
            logger(pamh, LOG_ERR,
                   "couldn't read from the config file [%d]: %s",
                   ret, strerror(ret));
            goto done;
        }
    }

    /* Set all values that were not set explicitly */
    conf = default_config(conf);
    if (conf == NULL) {
        ret = ENOMEM;
        goto done;
    }

    ret = 0;
    *_conf = conf;
done:
    if (ret) {
        ph_cleanup_config(conf);
    }
    fclose(fp);
    return ret;
}

void
ph_dump_config(pam_handle_t *pamh, struct pam_hbac_config *conf)
{
    if (conf == NULL) {
        logger(pamh, LOG_NOTICE, "NULL config pointer\n");
        return;
    }

    logger(pamh, LOG_DEBUG, "URI: %s\n", conf->uri);
    logger(pamh, LOG_DEBUG, "search base %s\n", conf->search_base);
    logger(pamh, LOG_DEBUG, "bind DN %s\n", conf->bind_dn);
    logger(pamh, LOG_DEBUG, "timeout %d\n", conf->timeout);
    logger(pamh, LOG_DEBUG, "client hostname %s\n", conf->hostname);
}
