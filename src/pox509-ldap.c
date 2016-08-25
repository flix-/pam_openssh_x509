/*
 * Copyright (C) 2014-2016 Sebastian Roland <seroland86@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pox509-ldap.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <confuse.h>
#include <lber.h>
#include <ldap.h>
#include <openssl/x509.h>

#include "pox509-error.h"
#include "pox509-log.h"
#include "pox509-util.h"

#define LDAP_SEARCH_FILTER_BUFFER_SIZE 1024

static void
free_attr_values_as_string_array(char **values)
{
    if (values == NULL) {
        log_debug("double free?");
        return;
    }

    for (int i = 0; values[i] != NULL; i++) {
        free(values[i]);
    }
    free(values);
}

static void
free_attr_values_as_binary_array(struct berval **values)
{
    if (values == NULL) {
        log_debug("double free?");
        return;
    }
    ldap_value_free_len(values);
}

static int
get_attr_values_as_string(LDAP *ldap_handle, LDAPMessage *result, char *attr,
    char ***ret)
{
    if (ldap_handle == NULL || result == NULL || attr == NULL || ret == NULL) {
        fatal("ldap_handle, result, attr or ret == NULL");
    }

    /* get attribute values */
    result = ldap_first_entry(ldap_handle, result);
    if (result == NULL) {
        log_debug("ldap_first_entry() error");
        return POX509_LDAP_ERR;
    }

    /* retrieve attribute value(s) */
    struct berval **values = ldap_get_values_len(ldap_handle, result, attr);
    if (values == NULL) {
        return POX509_LDAP_NO_SUCH_ATTR;
    }

    int res = POX509_UNKNOWN_ERR;
    /* count values so we know how wide our array has to be */
    int count = ldap_count_values_len(values);
    if (count == 0) {
        log_debug("ldap_count_values_len() returned 0");
        res = POX509_LDAP_ERR;
        goto cleanup_a;
    }

    char **values_string = malloc(sizeof(char *) * (count + 1));
    if (values_string == NULL) {
        log_debug("malloc() error");
        res = POX509_NO_MEMORY;
        goto cleanup_a;
    }

    for (int i = 0; values[i] != NULL; i++) {
        char *value = values[i]->bv_val;
        ber_len_t len = values[i]->bv_len;
        values_string[i] = strndup(value, len);
        if (values_string[i] == NULL) {
            log_debug("strndup() error");
            res = POX509_NO_MEMORY;
            goto cleanup_b;
        }
    }
    values_string[count] = NULL;

    *ret = values_string;
    values_string = NULL;
    res = POX509_OK;

cleanup_b:
    if (values_string != NULL) {
        free_attr_values_as_string_array(values_string);
    }
cleanup_a:
    ldap_value_free_len(values);
    return res;
}

static int
get_attr_values_as_binary(LDAP *ldap_handle, LDAPMessage *result, char *attr,
    struct berval ***ret)
{
    if (ldap_handle == NULL || result == NULL || attr == NULL || ret == NULL) {
        fatal("ldap_handle, result, attr or ret == NULL");
    }

    /* get attribute values */
    result = ldap_first_entry(ldap_handle, result);
    if (result == NULL) {
        log_debug("ldap_first_entry() error");
        return POX509_LDAP_ERR;
    }
    *ret = ldap_get_values_len(ldap_handle, result, attr);
    if (*ret == NULL) {
        return POX509_LDAP_NO_SUCH_ATTR;
    }
    return POX509_OK;
}

static int
check_profile_enabled(LDAP *ldap_handle, LDAPMessage *result,
    bool *is_profile_enabled)
{
    if (ldap_handle == NULL || result == NULL || is_profile_enabled == NULL) {
        fatal("ldap_handle, result or is_profile_enabled == NULL");
    }

    /* determine state of access profile from POX509_AP_IS_ENABLED attribute */
    char **access_profile_state = NULL;
    int rc = get_attr_values_as_string(ldap_handle, result,
        POX509_AP_IS_ENABLED, &access_profile_state);
    switch (rc) {
    case POX509_OK:
        break;
    case POX509_LDAP_NO_SUCH_ATTR:
        return POX509_LDAP_SCHEMA_ERR;
    default:
        return rc;
    }
    *is_profile_enabled = strcmp(access_profile_state[0], LDAP_BOOL_TRUE) == 0 ?
        true : false;
    free_attr_values_as_string_array(access_profile_state);

    return POX509_OK;
}

static int
get_access_profile_type(LDAP *ldap_handle, LDAPMessage *result,
    enum pox509_access_profile_type *ret)
{
    if (ldap_handle == NULL || result == NULL || ret == NULL) {
        fatal("ldap_handle, result or ret == NULL");
    }

    /* determine access profile type from objectClass attribute */
    char **objectclasses = NULL;
    int rc = get_attr_values_as_string(ldap_handle, result, "objectClass",
        &objectclasses);
    switch (rc) {
    case POX509_OK:
        break;
    case POX509_LDAP_NO_SUCH_ATTR:
        return POX509_LDAP_SCHEMA_ERR;
    default:
        return rc;
    }

    /* search for access profile type */
    for (int i = 0; objectclasses[i] != NULL; i++) {
        char *objectclass = objectclasses[i];
        if (strcmp(objectclass, POX509_DAP_OBJCLASS) == 0) {
            *ret = DIRECT_ACCESS_PROFILE;
            break;
        } else if (strcmp(objectclass, POX509_AOBP_OBJCLASS) == 0) {
            *ret = ACCESS_ON_BEHALF_PROFILE;
            break;
        }
    }
    free_attr_values_as_string_array(objectclasses);

    return POX509_OK;
}

static int
get_group_member_dns(LDAP *ldap_handle, cfg_t *cfg, char *group_member_dn,
    char *group_member_attr, char ***ret)
{
    if (ldap_handle == NULL || cfg == NULL || group_member_dn == NULL ||
        group_member_attr == NULL || ret == NULL) {

        fatal("ldap_handle, cfg, group_member_dn, group_member_attr or "
            "ret == NULL");
    }

    char *group_member_attrs[] = {
        group_member_attr,
        NULL
    };

    /* query ldap for group members */
    int res = POX509_UNKNOWN_ERR;
    struct timeval search_timeout = get_ldap_search_timeout(cfg);
    LDAPMessage *group_member_dns = NULL;
    int rc = ldap_search_ext_s(ldap_handle, group_member_dn, LDAP_SCOPE_BASE,
        NULL, group_member_attrs, 0, NULL, NULL, &search_timeout, 1,
        &group_member_dns);
    if (rc != LDAP_SUCCESS) {
        log_debug("ldap_search_ext_s(): base: '%s' - '%s' (%d)",
        group_member_dn, ldap_err2string(rc), rc);
        res = POX509_LDAP_ERR;
        goto cleanup;
    }

    /* get dn's of group members*/
    res = get_attr_values_as_string(ldap_handle, group_member_dns,
        group_member_attr, ret);

cleanup:
    ldap_msgfree(group_member_dns);
    return res;
}

static void
get_target_keystore_uid(LDAP *ldap_handle, cfg_t *cfg, char *target_keystore_dn,
    char ***target_keystore_uid)
{
    if (ldap_handle == NULL || cfg == NULL || target_keystore_dn == NULL ||
        target_keystore_uid == NULL) {

        fatal("ldap_handle, cfg, target_keystore_dn or target_keystore_uid == "
            "NULL");
    }

    char *target_keystore_uid_attr =
        cfg_getstr(cfg, "ldap_target_keystore_uid_attr");
    char *attrs[] = {
        target_keystore_uid_attr,
        NULL
    };
    struct timeval search_timeout = get_ldap_search_timeout(cfg);
    LDAPMessage *result = NULL;

    int rc = ldap_search_ext_s(ldap_handle, target_keystore_dn, LDAP_SCOPE_BASE,
        NULL, attrs, 0, NULL, NULL, &search_timeout, 1, &result);
    if (rc != LDAP_SUCCESS) {
        fatal("ldap_search_ext_s(): base: '%s' - '%s' (%d)", target_keystore_dn,
        ldap_err2string(rc), rc);
    }

    get_attr_values_as_string(ldap_handle, result, target_keystore_uid_attr,
        target_keystore_uid);
    ldap_msgfree(result);
}

static int
get_key_provider(LDAP *ldap_handle, cfg_t *cfg, char *key_provider_dn,
    struct pox509_key_provider **ret)
{
    if (ldap_handle == NULL || cfg == NULL || key_provider_dn == NULL ||
        ret == NULL) {

        fatal("ldap_handle, cfg, key_provider_dn or ret == NULL");
    }

    int res = POX509_UNKNOWN_ERR;
    struct pox509_key_provider *key_provider = new_key_provider();
    if (key_provider == NULL) {
        log_debug("malloc() error");
        return POX509_NO_MEMORY;
    }

    /* set dn of key provider */
    key_provider->dn = strdup(key_provider_dn);
    if (key_provider->dn == NULL) {
        log_debug("strdup() error");
        res = POX509_NO_MEMORY;
        goto cleanup_a;
    }

    /* obtain key provider data from ldap */
    char *key_provider_uid_attr = cfg_getstr(cfg, "ldap_key_provider_uid_attr");
    char *key_provider_cert_attr = cfg_getstr(cfg,
        "ldap_key_provider_cert_attr");
    char *attrs[] = {
        key_provider_uid_attr,
        key_provider_cert_attr,
        NULL
    };
    struct timeval search_timeout = get_ldap_search_timeout(cfg);
    LDAPMessage *provider = NULL;

    int rc = ldap_search_ext_s(ldap_handle, key_provider->dn, LDAP_SCOPE_BASE,
        NULL, attrs, 0, NULL, NULL, &search_timeout, 1, &provider);
    if (rc != LDAP_SUCCESS) {
        log_debug("ldap_search_ext_s(): base: '%s' - '%s' (%d)",
        key_provider->dn, ldap_err2string(rc), rc);
        res = POX509_LDAP_ERR;
        goto cleanup_b;
    }

    /* get attribute values */
    char **key_provider_uid = NULL;
    rc = get_attr_values_as_string(ldap_handle, provider, key_provider_uid_attr,
        &key_provider_uid);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup_b;
    }

    /* store key provider uid */
    key_provider->uid = strdup(key_provider_uid[0]);
    if (key_provider->uid == NULL) {
        log_debug("strdup() error");
        res = POX509_NO_MEMORY;
        goto cleanup_c;
    }

    /* get certificates */
    struct berval **key_provider_cert = NULL;
    rc = get_attr_values_as_binary(ldap_handle, provider, key_provider_cert_attr,
        &key_provider_cert);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup_c;
    }

    for (int i = 0; key_provider_cert[i] != NULL; i++) {
        char *value = key_provider_cert[i]->bv_val;
        ber_len_t len = key_provider_cert[i]->bv_len;

        struct pox509_key *key = new_key();
        if (key == NULL) {
            log_debug("malloc() error");
            res = POX509_NO_MEMORY;
            goto cleanup_d;
        }
        key->x509 = d2i_X509(NULL, (const unsigned char **) &value, len);
        if (key->x509 == NULL) {
            log_error("d2i_X509(): cannot decode certificate");
            free_key(key);
            continue;
        }
        TAILQ_INSERT_TAIL(&key_provider->keys, key, keys);
    }
    *ret = key_provider;
    key_provider = NULL;
    res = POX509_OK;

cleanup_d:
    free_attr_values_as_binary_array(key_provider_cert);
cleanup_c:
    free_attr_values_as_string_array(key_provider_uid);
cleanup_b:
    ldap_msgfree(provider);
cleanup_a:
    if (key_provider != NULL) {
        free(key_provider);
    };
    return res;
}

static void
get_keystore_options(LDAP *ldap_handle, cfg_t *cfg, char *keystore_options_dn,
    struct pox509_keystore_options *options)
{
    if (ldap_handle == NULL || cfg == NULL || keystore_options_dn == NULL ||
        options == NULL) {

        fatal("ldap_handle, cfg, keystore_options_dn or options == NULL");
    }

    options->dn = strdup(keystore_options_dn);
    if (options->dn == NULL) {
        fatal("strdup()");
    }
    get_rdn_value_from_dn(keystore_options_dn, &options->uid);
    char *attrs[] = {
        POX509_KEYSTORE_OPTIONS_FROM_ATTR,
        POX509_KEYSTORE_OPTIONS_CMD_ATTR,
        NULL
    };
    struct timeval search_timeout = get_ldap_search_timeout(cfg);
    LDAPMessage *result = NULL;

    int rc = ldap_search_ext_s(ldap_handle, options->dn, LDAP_SCOPE_BASE, NULL,
        attrs, 0, NULL, NULL, &search_timeout, 1, &result);
    if (rc != LDAP_SUCCESS) {
        fatal("ldap_search_ext_s(): base: '%s' - '%s' (%d)", options->dn,
        ldap_err2string(rc), rc);
    }

    /* get attribute values (can be NULL as optional) */
    char **keystore_options_from = NULL;
    get_attr_values_as_string(ldap_handle, result,
        POX509_KEYSTORE_OPTIONS_FROM_ATTR, &keystore_options_from);
    if (keystore_options_from == NULL) {
        log_info("keystore_options_from_attr == NULL");
    } else {
        options->from_option = strdup(keystore_options_from[0]);
        if (options->from_option == NULL) {
            fatal("strdup()");
        }
        free_attr_values_as_string_array(keystore_options_from);
    }

    char **keystore_options_cmd = NULL;
    get_attr_values_as_string(ldap_handle, result,
        POX509_KEYSTORE_OPTIONS_CMD_ATTR, &keystore_options_cmd);
    if (keystore_options_cmd == NULL) {
        log_info("keystore_options_cmd_attr == NULL");
    } else {
        options->command_option = strdup(keystore_options_cmd[0]);
        if (options->command_option == NULL) {
            fatal("strdup()");
        }
        free_attr_values_as_string_array(keystore_options_cmd);
    }
    ldap_msgfree(result);
}

//static bool
//is_relevant_aobp(LDAP *ldap_handle, cfg_t *cfg, struct pox509_info *info,
//    struct pox509_access_on_behalf_profile *profile)
//{
//    if (ldap_handle == NULL || cfg == NULL || info == NULL ||
//        profile == NULL) {
//
//        fatal("ldap_handle, cfg, info or profile == NULL");
//    }
//
//    if (info->uid == NULL) {
//        fatal("info->uid == NULL");
//    }
//
//    /* check target keystores */
//    char *target_keystore_group_member_attr = cfg_getstr(cfg,
//        "ldap_target_keystore_group_member_attr");
//    char **target_keystore_dns = NULL;
//    get_group_member_dns(ldap_handle, cfg, profile->target_keystore_group_dn,
//        target_keystore_group_member_attr, &target_keystore_dns);
//    if (target_keystore_dns == NULL) {
//        fatal("target_keystore_dns == NULL");
//    }
//
//    char **target_keystore_uid = NULL;
//    bool target_keystore_has_uid = false;
//    for (int i = 0; target_keystore_dns[i] != NULL && !target_keystore_has_uid;
//        i++) {
//
//        get_target_keystore_uid(ldap_handle, cfg, target_keystore_dns[i],
//            &target_keystore_uid);
//        if (target_keystore_uid == NULL) {
//            fatal("target_keystore_uid == NULL");
//        }
//
//        if(strcmp(info->uid, target_keystore_uid[0]) == 0) {
//            target_keystore_has_uid = true;
//        }
//        free_attr_values_as_string_array(target_keystore_uid);
//        target_keystore_uid = NULL;
//    }
//    free_attr_values_as_string_array(target_keystore_dns);
//    return target_keystore_has_uid;
//}

//static void
//process_access_on_behalf_profiles(LDAP *ldap_handle, cfg_t *cfg,
//    struct pox509_info *info)
//{
//    if (ldap_handle == NULL || cfg == NULL || info == NULL) {
//        fatal("ldap_handle, cfg or info == NULL");
//    }
//
//    if (TAILQ_EMPTY(&info->access_on_behalf_profiles)) {
//        log_info("access on behalf profile list EMPTY");
//        return;
//    }
//
//    /* iterate access on behalf profiles */
//    struct pox509_access_on_behalf_profile *profile = NULL;
//    struct pox509_access_on_behalf_profile *tmp = NULL;
//    TAILQ_FOREACH_SAFE(profile, &info->access_on_behalf_profiles,
//        profiles, tmp) {
//
//        if (!is_relevant_aobp(ldap_handle, cfg, info, profile)) {
//            TAILQ_REMOVE(&info->access_on_behalf_profiles, profile,
//                profiles);
//            free_access_on_behalf_profile(profile);
//            continue;
//        }
//
//        /* get key provider(s) */
//        char *key_provider_group_member_attr = cfg_getstr(cfg,
//            "ldap_key_provider_group_member_attr");
//        char **key_provider_dns = NULL;
//        get_group_member_dns(ldap_handle, cfg, profile->key_provider_group_dn,
//            key_provider_group_member_attr, &key_provider_dns);
//        if (key_provider_dns == NULL) {
//            fatal("key_provider_dns == NULL");
//        }
//
//        for (int i = 0; key_provider_dns[i] != NULL; i++) {
//            struct pox509_key_provider *key_provider = new_key_provider();
//            if (key_provider == NULL) {
//                fatal("malloc()");
//            }
//            get_key_provider(ldap_handle, cfg, key_provider_dns[i],
//                &key_provider);
//            TAILQ_INSERT_TAIL(&profile->key_providers, key_provider,
//                key_providers);
//        }
//        free_attr_values_as_string_array(key_provider_dns);
//
//        /* get keystore options */
//        profile->keystore_options = new_keystore_options();
//        if (profile->keystore_options == NULL) {
//            fatal("malloc()");
//        }
//        get_keystore_options(ldap_handle, cfg, profile->keystore_options_dn,
//            profile->keystore_options);
//    }
//}

//static int
//process_direct_access_profiles(LDAP *ldap_handle, cfg_t *cfg,
//    struct pox509_info *info)
//{
//    if (ldap_handle == NULL || cfg == NULL || info == NULL) {
//        fatal("ldap_handle, cfg or info == NULL");
//    }
//
//    if (TAILQ_EMPTY(&info->access_profiles)) {
//        log_info("access profile list EMPTY");
//        return POX509_OK;
//    }
//
//    /* iterate direct access profiles */
//    struct pox509_access_profile *profile = NULL;
//    struct pox509_access_profile *tmp = NULL;
//    TAILQ_FOREACH_SAFE(profile, &info->access_profiles, access_profiles, tmp) {
//        /* get key provider dns */
//        char *key_provider_group_member_attr = cfg_getstr(cfg,
//            "ldap_key_provider_group_member_attr");
//        char **key_provider_dns = NULL;
//        int rc = get_group_member_dns(ldap_handle, cfg,
//            profile->key_provider_group_dn, key_provider_group_member_attr,
//            &key_provider_dns);
//        switch (rc) {
//        case POX509_OK:
//            break;
//        case POX509_NO_MEMORY:
//            log_debug("get_group_member_dns(): '%s'", pox509_strerror(rc));
//            return rc;
//        default:
//            log_debug("get_group_member_dns(): '%s'", pox509_strerror(rc));
//            TAILQ_REMOVE(&info->access_profiles, profile, profiles);
//            free_access_profile(profile);
//            continue;
//        }
//
//        /* add all valid key providers to dto */
//        for (int i = 0; key_provider_dns[i] != NULL; i++) {
//            struct pox509_key_provider *key_provider = NULL;
//            int rc = get_key_provider(ldap_handle, cfg, key_provider_dns[i],
//                &key_provider);
//            switch (rc) {
//            case POX509_OK:
//                break;
//            case POX509_NO_MEMORY:
//                log_debug("get_key_provider(): '%s'", pox509_strerror(rc));
//                free_attr_values_as_string_array(key_provider_dns);
//                return rc;
//            default:
//                log_debug("get_key_provider(): '%s'", pox509_strerror(rc));
//                continue;
//            }
//
//            /* check if key provider has certificate */
//            if (TAILQ_EMPTY(&key_provider->keys)) {
//                log_info("skipping key provider '%s' (%s): no certs found",
//                    key_provider->uid, key_provider->dn);
//                free_key_provider(key_provider);
//                continue;
//            }
//
//            /* check if key provider is relevant */
//            bool is_authorized = strcmp(key_provider->uid, info->uid)
//                == 0 ? true : false;
//            if (is_authorized) {
//                log_info("found key provider '%s' (%s)", key_provider->uid,
//                    key_provider->dn);
//                TAILQ_INSERT_TAIL(&profile->key_providers, key_provider,
//                    key_providers);
//            } else {
//                log_info("skipping key provider '%s' (%s): not authorized",
//                    key_provider->uid, key_provider->dn);
//                free_key_provider(key_provider);
//            }
//        } /* for */
//        free_attr_values_as_string_array(key_provider_dns);
//
//        /* check if at least one valid key provider has been found */
//        if (TAILQ_EMPTY(&profile->key_providers)) {
//            log_info("stripping direct access profile '%s' (no valid key "
//                "provider found)", profile->uid);
//            TAILQ_REMOVE(&info->access_profiles, profile, profiles);
//            free_access_profile(profile);
//            continue;
//        }
//
//        /* get keystore options */
//        if (profile->keystore_options_dn == NULL) {
//            continue;
//        }
//
//        profile->keystore_options = new_keystore_options();
//        if (profile->keystore_options == NULL) {
//            fatal("malloc()");
//        }
//        get_keystore_options(ldap_handle, cfg,
//            profile->keystore_options_dn, profile->keystore_options);
//    } /* for */
//
//    return POX509_OK;
//}

//static void
//add_access_on_behalf_profile(LDAP *ldap_handle, LDAPMessage *result, char *dn,
//    struct pox509_info *info)
//{
//    if (ldap_handle == NULL || result == NULL || dn == NULL || info == NULL) {
//        fatal("ldap_handle, result, dn or info == NULL");
//    }
//
//    /* create new access on behalf profile */
//    struct pox509_access_on_behalf_profile *profile = malloc(sizeof *profile);
//    if (profile == NULL) {
//        fatal("malloc()");
//    }
//
//    profile->dn = strdup(dn);
//    if (profile->dn == NULL) {
//        fatal("strdup()");
//    }
//    get_rdn_value_from_dn(dn, &profile->uid);
//
//    /* set target keystore group dn */
//    char **target_keystore_group_dn = NULL;
//    get_attr_values_as_string(ldap_handle, result,
//        POX509_AOBP_TARGET_KEYSTORE_ATTR, &target_keystore_group_dn);
//    if (target_keystore_group_dn == NULL) {
//        fatal("target_keystore_group_dn == NULL");
//    }
//    profile->target_keystore_group_dn = strdup(target_keystore_group_dn[0]);
//    if (profile->target_keystore_group_dn == NULL) {
//        fatal("strdup()");
//    }
//    free_attr_values_as_string_array(target_keystore_group_dn);
//
//    /* set key provider group dn*/
//    char **key_provider_group_dn = NULL;
//    get_attr_values_as_string(ldap_handle, result,
//        POX509_AP_KEY_PROVIDER_ATTR, &key_provider_group_dn);
//    if (key_provider_group_dn == NULL) {
//        fatal("key_provider_group_dn == NULL");
//    }
//    profile->key_provider_group_dn = strdup(key_provider_group_dn[0]);
//    if (profile->key_provider_group_dn == NULL) {
//        fatal("strdup()");
//    }
//    free_attr_values_as_string_array(key_provider_group_dn);
//
//    /* set keystore options */
//    char **keystore_options_dn = NULL;
//    get_attr_values_as_string(ldap_handle, result,
//        POX509_AP_KEYSTORE_OPTIONS_ATTR, &keystore_options_dn);
//    if (keystore_options_dn == NULL) {
//        fatal("keystore_options_dn == NULL");
//    }
//    profile->keystore_options_dn = strdup(keystore_options_dn[0]);
//    if (profile->keystore_options_dn == NULL) {
//        fatal("strdup()");
//    }
//    free_attr_values_as_string_array(keystore_options_dn);
//
//    /* add to access on behalf profile list */
//    TAILQ_INSERT_TAIL(&info->access_on_behalf_profiles, profile,
//        profiles);
//}

//static int
//add_access_profile(LDAP *ldap_handle, LDAPMessage *result,
//    char *access_profile_dn, struct pox509_info *info,
//    enum pox509_access_profile_type type)
//{
//    if (ldap_handle == NULL || result == NULL || access_profile_dn == NULL ||
//        info == NULL) {
//
//        fatal("ldap_handle, result, access_profile_dn or info == NULL");
//    }
//
//    int res = POX509_UNKNOWN_ERR;
//    /* create new access profile */
//    struct pox509_access_profile *profile = new_access_profile(type);
//    if (profile == NULL) {
//        log_debug("malloc() error");
//        return POX509_NO_MEMORY;
//    }
//
//    profile->>dn = strdup(access_profile_dn);
//    if (profile->>dn == NULL) {
//        log_debug("strdup() error");
//        res = POX509_NO_MEMORY;
//        goto cleanup_a;
//    }
//    int rc = get_rdn_value_from_dn(access_profile_dn, &profile->uid);
//    if (rc != POX509_OK) {
//        res = rc;
//        goto cleanup_a;
//    }
//
//    /* add key provider group dn */
//    char **key_provider_group_dn = NULL;
//    rc = get_attr_values_as_string(ldap_handle, result,
//        POX509_AP_KEY_PROVIDER_ATTR, &key_provider_group_dn);
//    switch (rc) {
//    case POX509_OK:
//        break;
//    case POX509_LDAP_NO_SUCH_ATTR:
//        res = POX509_LDAP_SCHEMA_ERR;
//        goto cleanup_a;
//    default:
//        res = rc;
//        goto cleanup_a;
//    }
//    profile->key_provider_group_dn = strdup(key_provider_group_dn[0]);
//    if (profile->key_provider_group_dn == NULL) {
//        log_debug("strdup() error");
//        res = POX509_NO_MEMORY;
//        goto cleanup_b;
//    }
//
//    /* add keystore options dn if present (optional attribute) */
//    char **keystore_options_dn = NULL;
//    rc = get_attr_values_as_string(ldap_handle, result,
//        POX509_AP_KEYSTORE_OPTIONS_ATTR, &keystore_options_dn);
//    switch (rc) {
//    case POX509_OK:
//        profile->keystore_options_dn = strdup(keystore_options_dn[0]);
//        if (profile->keystore_options_dn == NULL) {
//            log_debug("strdup() error");
//            res = POX509_NO_MEMORY;
//            goto cleanup_c;
//        }
//        break;
//    case POX509_LDAP_NO_SUCH_ATTR:
//        /* valid as keystore options are optional */
//        break;
//    default:
//        res = rc;
//        goto cleanup_b;
//    }
//
//    /* add to direct access profile list */
//    TAILQ_INSERT_TAIL(&info->direct_access_profiles, profile, profiles);
//    profile = NULL;
//    res = POX509_OK;
//
//cleanup_c:
//    if (keystore_options_dn != NULL) {
//        free_attr_values_as_string_array(keystore_options_dn);
//    }
//cleanup_b:
//    free_attr_values_as_string_array(key_provider_group_dn);
//cleanup_a:
//    if (profile != NULL) {
//        free_access_profile(profile);
//    }
//    return res;
//}

static int
check_access_profile_relevance_general(LDAP *ldap_handle,
    LDAPMessage *access_profile, bool *is_relevant)
{
    if (ldap_handle == NULL || access_profile == NULL || is_relevant == NULL) {
        fatal("ldap_handle, access_profile or is_relevant == NULL");
    }

    *is_relevant = true;
    /* check if acccess profile entry is enabled */
    bool is_profile_enabled;
    int rc = check_profile_enabled(ldap_handle, access_profile,
        &is_profile_enabled);
    if (rc != POX509_OK) {
        return rc;
    }

    if (!is_profile_enabled) {
        log_info("profile disabled");
        *is_relevant = false;
        return POX509_OK;
    }

    /* do further checks here in the future */
    return POX509_OK;
}

static int
get_access_profiles(LDAP *ldap_handle, cfg_t *cfg,
    struct pox509_access_profile **ret, char **access_profile_dns)
{
    if (ldap_handle == NULL || cfg == NULL || ret == NULL ||
        access_profile_dns == NULL) {

        fatal("ldap_handle, cfg, ret or access_profile_dns == NULL");
    }
    return POX509_OK;
//
//    /* get access profile dn's as strings */
//    char **access_profile_dns = NULL;
//    char *access_profile_attr = cfg_getstr(cfg,
//        "ldap_ssh_server_access_profile_attr");
//    rc = get_attr_values_as_string(ldap_handle, ssh_server_entry,
//        access_profile_attr, &access_profile_dns);
//    if (rc != POX509_OK) {
//        res = rc;
//        goto cleanup_a;
//    }
//
//    struct timeval search_timeout = get_ldap_search_timeout(cfg);
//    /* iterate access profile dns */
//    for (int i = 0; access_profile_dns[i] != NULL; i++) {
//        char *access_profile_dn = access_profile_dns[i];
//        log_info("%s", access_profile_dn);
//
//        /* get access profile entry */
//        LDAPMessage *access_profile_entry = NULL;
//        int rc = ldap_search_ext_s(ldap_handle, access_profile_dn,
//            LDAP_SCOPE_BASE, NULL, NULL, 0, NULL, NULL, &search_timeout, 1,
//            &access_profile_entry);
//        if (rc != LDAP_SUCCESS) {
//            log_debug("ldap_search_ext_s(): base: '%s' - '%s' (%d)",
//            access_profile_dn, ldap_err2string(rc), rc);
//            goto cleanup_a_inner;
//        }
//
//        /* check if acccess profile is relevant */
//        bool is_access_profile_relevant;
//        rc = check_access_profile_relevance_general(ldap_handle,
//            access_profile_entry, &is_access_profile_relevant);
//        switch (rc) {
//        case POX509_OK:
//            break;
//        case POX509_NO_MEMORY:
//            /* cleanup access profile entry and break */
//            ldap_msgfree(access_profile_entry);
//            res = rc;
//            goto cleanup_b;
//        default:
//            /* continue */
//            log_debug("check_access_profile_relevance_general(): '%s'",
//                pox509_strerror(rc));
//            goto cleanup_a_inner;
//        }
//        if (!is_access_profile_relevant) {
//            /* continue */
//            goto cleanup_a_inner;
//        }
//
//        /* get access profile type from relevant profiles */
//        enum pox509_access_profile_type access_profile_type;
//        rc = get_access_profile_type(ldap_handle, access_profile_entry,
//            &access_profile_type);
//        switch (rc) {
//        case POX509_OK:
//            break;
//        case POX509_NO_MEMORY:
//            /* cleanup access profile entry and break */
//            ldap_msgfree(access_profile_entry);
//            res = rc;
//            goto cleanup_b;
//        default:
//            /* continue */
//            log_debug("get_access_profile_type(): '%s'", pox509_strerror(rc));
//            goto cleanup_a_inner;
//        }
//
//        /* retrieve access profile and add to dto */
//        rc = add_access_profile(ldap_handle, access_profile_entry,
//            access_profile_dn, info, access_profile_type);
//        switch (rc) {
//        case POX509_OK:
//            break;
//        case POX509_NO_MEMORY:
//            ldap_msgfree(access_profile_entry);
//            res = rc;
//            goto cleanup_b;
//        default:
//            /* continue */
//            log_debug("add_direct_access_profile(): '%s'",
//                pox509_strerror(rc));
//            goto cleanup_a_inner;
//        }
//cleanup_a_inner:
//        ldap_msgfree(access_profile_entry);
//    } /* for */
//    res = POX509_OK;
//
//cleanup_b:
//    free_attr_values_as_string_array(access_profile_dns);
//cleanup_a:
//    ldap_msgfree(ssh_server_entry);
//    return res;
}

static int
get_ssh_server_entry(LDAP *ldap_handle, cfg_t *cfg, LDAPMessage **ret)
{
    if (ldap_handle == NULL || cfg == NULL || ret == NULL) {
        fatal("ldap_handle, cfg or ret == NULL");
    }

    char *ssh_server_base_dn = cfg_getstr(cfg, "ldap_ssh_server_base_dn");
    int ssh_server_search_scope =
        cfg_getint(cfg, "ldap_ssh_server_search_scope");
    /* construct search filter */
    char filter[LDAP_SEARCH_FILTER_BUFFER_SIZE];
    char *ssh_server_uid_attr = cfg_getstr(cfg, "ldap_ssh_server_uid_attr");
    char *ssh_server_uid = cfg_getstr(cfg, "ssh_server_uid");
    int rc = create_ldap_search_filter(ssh_server_uid_attr, ssh_server_uid,
        filter, sizeof filter);
    if (rc != POX509_OK) {
        return rc;
    }
    char *ssh_server_access_profile_attr = cfg_getstr(cfg,
        "ldap_ssh_server_access_profile_attr");
    char *attrs[] = {
        ssh_server_access_profile_attr,
        NULL
    };
    struct timeval search_timeout = get_ldap_search_timeout(cfg);

    int res = POX509_UNKNOWN_ERR;
    /* query ldap for ssh server entry */
    LDAPMessage *ssh_server_entry = NULL;
    rc = ldap_search_ext_s(ldap_handle, ssh_server_base_dn,
        ssh_server_search_scope, filter, attrs, 0, NULL, NULL, &search_timeout,
        1, &ssh_server_entry);
    if (rc != LDAP_SUCCESS) {
        log_debug("ldap_search_ext_s(): base: '%s' - '%s' (%d)",
        ssh_server_base_dn, ldap_err2string(rc), rc);
        res = POX509_LDAP_ERR;
        goto cleanup;
    }

    /* check if ssh server entry has been found */
    rc = ldap_count_entries(ldap_handle, ssh_server_entry);
    switch (rc) {
    case 0:
        log_error("ssh server entry not existent");
        res = POX509_LDAP_NO_SUCH_ENTRY;
        goto cleanup;
    case 1:
        *ret = ssh_server_entry;
        ssh_server_entry = NULL;
        res = POX509_OK;
        break;
    default:
        log_debug("ldap_count_entries() error");
        res = POX509_LDAP_ERR;
        goto cleanup;
    }

cleanup:
    if (ssh_server_entry != NULL) {
        ldap_msgfree(ssh_server_entry);
    }
    return res;
}

static int
init_starttls(LDAP *ldap_handle)
{
    if (ldap_handle == NULL) {
        fatal("ldap_handle == NULL");
    }

    /* establishes connection to ldap */
    int rc = ldap_start_tls_s(ldap_handle, NULL, NULL);
    if (rc != LDAP_SUCCESS) {
        char *msg;
        rc = ldap_get_option(ldap_handle, LDAP_OPT_DIAGNOSTIC_MESSAGE, &msg);
        if (rc == LDAP_OPT_SUCCESS) {
            log_debug("ldap_start_tls_s(): '%s'", msg);
            ldap_memfree(msg);
            return POX509_LDAP_CONNECTION_ERR;
        }
        log_debug("ldap_start_tls_s()");
        return POX509_LDAP_CONNECTION_ERR;
    }
    return POX509_OK;
}

static int
connect_to_ldap(LDAP *ldap_handle, cfg_t *cfg)
{
    if (ldap_handle == NULL || cfg == NULL) {
        fatal("ldap_handle or cfg == NULL");
    }

    int rc;
    int ldap_starttls = cfg_getint(cfg, "ldap_starttls");
    if (ldap_starttls) {
        rc = init_starttls(ldap_handle);
        if (rc != POX509_OK) {
            return rc;
        }
    }

    char *ldap_bind_dn = cfg_getstr(cfg, "ldap_bind_dn");
    char *ldap_bind_pwd = cfg_getstr(cfg, "ldap_bind_pwd");
    size_t ldap_bind_pwd_length = strlen(ldap_bind_pwd);
    struct berval cred = {
        .bv_len = ldap_bind_pwd_length,
        .bv_val = ldap_bind_pwd
    };
    rc = ldap_sasl_bind_s(ldap_handle, ldap_bind_dn, LDAP_SASL_SIMPLE, &cred,
        NULL, NULL, NULL);
    memset(ldap_bind_pwd, 0, ldap_bind_pwd_length);
    if (rc != LDAP_SUCCESS) {
        log_debug("ldap_sasl_bind_s(): '%s' (%d)", ldap_err2string(rc), rc);
        return POX509_LDAP_CONNECTION_ERR;
    }
    return POX509_OK;
}

static int
set_ldap_options(LDAP *ldap_handle, cfg_t *cfg)
{
    if (ldap_handle == NULL || cfg == NULL) {
        fatal("ldap_handle or cfg == NULL");
    }

    /* set protocol version */
    int ldap_version = LDAP_VERSION3;
    int rc = ldap_set_option(ldap_handle, LDAP_OPT_PROTOCOL_VERSION,
        &ldap_version);
    if (rc != LDAP_OPT_SUCCESS) {
        log_debug("ldap_set_option(): key: LDAP_OPT_PROTOCOL_VERSION, value: "
            "%d", ldap_version);
        return POX509_LDAP_ERR;
    }

    /* force validation of certificates when using ldaps */
    int req_cert = LDAP_OPT_X_TLS_HARD;
    rc = ldap_set_option(ldap_handle, LDAP_OPT_X_TLS_REQUIRE_CERT, &req_cert);
    if (rc != LDAP_OPT_SUCCESS) {
        log_debug("ldap_set_option(): key: LDAP_OPT_X_TLS_REQUIRE_CERT, value: "
            "%d", req_cert);
        return POX509_LDAP_ERR;
    }

    /* set trusted ca path */
    char *cacerts_dir = cfg_getstr(cfg, "cacerts_dir");
    rc = ldap_set_option(ldap_handle, LDAP_OPT_X_TLS_CACERTDIR, cacerts_dir);
    if (rc != LDAP_OPT_SUCCESS) {
        log_debug("ldap_set_option(): key: LDAP_OPT_X_TLS_CACERTDIR, value: %s",
            cacerts_dir);
        return POX509_LDAP_ERR;
    }

    /*
     * new context has to be set in order to apply options set above regarding
     * tls.
     */
    int new_ctx = 0x56;
    rc = ldap_set_option(ldap_handle, LDAP_OPT_X_TLS_NEWCTX, &new_ctx);
    if (rc != LDAP_OPT_SUCCESS) {
        log_debug("ldap_set_option(): key: LDAP_OPT_X_TLS_NEWCTX, value: %d",
            new_ctx);
        return POX509_LDAP_ERR;
    }
    return POX509_OK;
}

static int
init_ldap_handle(LDAP **ret, cfg_t *cfg)
{
    if (ret == NULL || cfg == NULL) {
        fatal("ret or cfg == NULL");
    }

    LDAP *ldap_handle = NULL;
    char *ldap_uri = cfg_getstr(cfg, "ldap_uri");
    int rc = ldap_initialize(&ldap_handle, ldap_uri);
    if (rc != LDAP_SUCCESS) {
        log_debug("ldap_initialize(): '%s' (%d)", ldap_err2string(rc), rc);
        return POX509_LDAP_ERR;
    }

    int res = POX509_UNKNOWN_ERR;
    rc = set_ldap_options(ldap_handle, cfg);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup;
    }
    *ret = ldap_handle;
    ldap_handle = NULL;
    res = POX509_OK;

cleanup:
    if (ldap_handle != NULL) {
        rc = ldap_unbind_ext_s(ldap_handle, NULL, NULL);
        if (rc != LDAP_SUCCESS) {
            log_debug("ldap_unbind_ext_s(): '%s' (%d)", ldap_err2string(rc),
                rc);
        }
    }
    return res;
}

int
get_keystore_data_from_ldap(cfg_t *cfg, struct pox509_info *info)
{
    if (cfg == NULL || info == NULL) {
        fatal("cfg or info == NULL");
    }

    /* init ldap handle */
    LDAP *ldap_handle;
    int rc = init_ldap_handle(&ldap_handle, cfg);
    if (rc != POX509_OK) {
        return rc;
    }

    int res = POX509_UNKNOWN_ERR;
    /* connect to ldap server */
    rc = connect_to_ldap(ldap_handle, cfg);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup_a;
    }
    info->ldap_online = 1;
    log_info("connection to ldap established");

    /* get ssh server entry */
    LDAPMessage *ssh_server_entry = NULL;
    rc = get_ssh_server_entry(ldap_handle, cfg, &ssh_server_entry);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup_a;
    }

    /* create ssh_server entity */
    struct pox509_ssh_server *ssh_server = new_ssh_server();
    if (ssh_server == NULL) {
        res = POX509_NO_MEMORY;
        goto cleanup_b;
    }

    ssh_server->dn = ldap_get_dn(ldap_handle, ssh_server_entry);
    if (ssh_server->dn == NULL) {
        log_debug("ldap_get_dn() error");
        res = POX509_LDAP_ERR;
        goto cleanup_c;
    }
    ssh_server->uid = cfg_getstr(cfg, "ssh_server_uid");
    log_info("ssh server entry found '%s' (%s)", ssh_server->uid,
        ssh_server->dn);

    /* get access profiles */
    char **access_profile_dns = NULL;
    char *access_profile_attr = cfg_getstr(cfg,
        "ldap_ssh_server_access_profile_attr");
    rc = get_attr_values_as_string(ldap_handle, ssh_server_entry,
        access_profile_attr, &access_profile_dns);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup_c;
    }

    struct pox509_access_profile *access_profiles = NULL;
    log_info("get access profiles");
    rc = get_access_profiles(ldap_handle, cfg, &access_profiles,
        access_profile_dns);
    if (rc != POX509_OK) {
        res = rc;
        goto cleanup_d;
    }

    /* assemble */
    info->ssh_server = ssh_server;
    ssh_server = NULL;
    TAILQ_INSERT_TAIL(&info->access_profiles, access_profiles, access_profiles);
    access_profiles = NULL;

    res = POX509_OK;

    if (access_profiles != NULL) {
        //free_access_profiles(access_profiles);
    }
cleanup_d:
    free_attr_values_as_string_array(access_profile_dns);
cleanup_c:
    if (ssh_server != NULL) {
        free_ssh_server(ssh_server);
    }
cleanup_b:
    ldap_msgfree(ssh_server_entry);
cleanup_a:
    rc = ldap_unbind_ext_s(ldap_handle, NULL, NULL);
    if (rc == LDAP_SUCCESS) {
        log_info("unbound from ldap");
    } else {
        log_debug("ldap_unbind_ext_s(): '%s' (%d)", ldap_err2string(rc), rc);
    }
    return res;
}

