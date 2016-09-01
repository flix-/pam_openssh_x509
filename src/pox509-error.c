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

/**
 * Definition of error values.
 *
 * @file pox509-error.c
 * @author Sebastian Roland <seroland86@gmail.com>
 * @date 2016-08-13
 * @see https://github.com/flix-/pam-openssh-x509
 */

#include "pox509-error.h"

const char *
pox509_strerror(int errnum)
{
    switch(errnum) {
    case POX509_OK:
        return "ok";
    case POX509_SYSTEM_ERR:
        return "system error";
    case POX509_NO_MEMORY:
        return "no memory";
    case POX509_NO_SUCH_VALUE:
        return "no such value";
    case POX509_PARSE_CONFIG_ERR:
        return "parse config error";
    case POX509_REGEX_ERR:
        return "regex error";
    case POX509_LDAP_ERR:
        return "ldap error";
    case POX509_LDAP_CONNECTION_ERR:
        return "ldap connection error";
    case POX509_LDAP_NO_SUCH_ENTRY:
        return "no such ldap entry";
    case POX509_LDAP_NO_SUCH_ATTR:
        return "no such ldap attribute";
    case POX509_LDAP_NO_SUCH_VALUE:
        return "no such ldap value";
    case POX509_LDAP_INVALID_RESULT:
        return "invalid ldap result";
    case POX509_NOT_RELEVANT:
        return "not relevant";
    case POX509_NO_CERT:
        return "no certificate";
    case POX509_NO_KEY_PROVIDER:
        return "no key provider";
    case POX509_X509_ERR:
        return "x509 error";
    case POX509_NO_ACCESS_PROFILE:
        return "no access profile";
    case POX509_UNKNOWN_ACCESS_PROFILE_TYPE:
        return "unknown access profile type";
    case POX509_NO_KEYSTORE_OPTION:
        return "no keystore option found";
    case POX509_INVALID_CERT:
        return "certificate invalid";
    case POX509_KEY_TRANSFORM_ERR:
        return "key transformation error";
    case POX509_NO_KEY:
        return "no key";
    case POX509_OPENSSL_ERR:
        return "openssl error";

    case POX509_UNKNOWN_ERR:
        return "unknown error";
    default:
        return "undefined error";
    }
}

