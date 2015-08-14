/**********************************************************************
 *                          gost_pmeth.c                              *
 *             Copyright (c) 2005-2013 Cryptocom LTD                  *
 *         This file is distributed under the same license as OpenSSL *
 *                                                                    *
 *   Implementation of RFC 4357 (GOST R 34.10) Publick key method     *
 *       for OpenSSL                                                  *
 *          Requires OpenSSL 1.0.0+ for compilation                   *
 **********************************************************************/
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/ec.h>
#include <openssl/x509v3.h>     /* For string_to_hex */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gost_params.h"
#include "gost_lcl.h"
#include "e_gost_err.h"

/* -----init, cleanup, copy - uniform for all algs  --------------*/
/* Allocates new gost_pmeth_data structure and assigns it as data */
static int pkey_gost_init(EVP_PKEY_CTX *ctx)
{
    struct gost_pmeth_data *data;
    EVP_PKEY *pkey = EVP_PKEY_CTX_get0_pkey(ctx);
    data = OPENSSL_malloc(sizeof(struct gost_pmeth_data));
    if (!data)
        return 0;
    memset(data, 0, sizeof(struct gost_pmeth_data));
    if (pkey && EVP_PKEY_get0(pkey)) {
        switch (EVP_PKEY_base_id(pkey)) {
        case NID_id_GostR3410_94:
            data->sign_param_nid = gost94_nid_by_params(EVP_PKEY_get0(pkey));
            break;
        case NID_id_GostR3410_2001:
        case NID_id_GostR3410_2012_256:
        case NID_id_GostR3410_2012_512:
            {
                const EC_GROUP *group =
                    EC_KEY_get0_group(EVP_PKEY_get0((EVP_PKEY *)pkey));
                if (group != NULL) {
                    data->sign_param_nid = EC_GROUP_get_curve_name(group);
                    break;
                }
                /* else */
            }
        default:
            OPENSSL_free(data);
            return 0;
        }
    }
    EVP_PKEY_CTX_set_data(ctx, data);
    return 1;
}

/* Copies contents of gost_pmeth_data structure */
static int pkey_gost_copy(EVP_PKEY_CTX *dst, EVP_PKEY_CTX *src)
{
    struct gost_pmeth_data *dst_data, *src_data;
    if (!pkey_gost_init(dst)) {
        return 0;
    }
    src_data = EVP_PKEY_CTX_get_data(src);
    dst_data = EVP_PKEY_CTX_get_data(dst);
    if (!src_data || !dst_data)
        return 0;

    *dst_data = *src_data;
    if (src_data->shared_ukm) {
        dst_data->shared_ukm = NULL;
    }
    return 1;
}

/* Frees up gost_pmeth_data structure */
static void pkey_gost_cleanup(EVP_PKEY_CTX *ctx)
{
    struct gost_pmeth_data *data = EVP_PKEY_CTX_get_data(ctx);
    if (!data)
        return;
    if (data->shared_ukm)
        OPENSSL_free(data->shared_ukm);
    OPENSSL_free(data);
}

/* --------------------- control functions  ------------------------------*/
static int pkey_gost_ctrl(EVP_PKEY_CTX *ctx, int type, int p1, void *p2)
{
    struct gost_pmeth_data *pctx =
        (struct gost_pmeth_data *)EVP_PKEY_CTX_get_data(ctx);
    if (pctx == NULL)
        return 0;

    switch (type) {
    case EVP_PKEY_CTRL_MD:
        {
            EVP_PKEY *key = EVP_PKEY_CTX_get0_pkey(ctx);
            int pkey_nid = (key == NULL) ? NID_undef : EVP_PKEY_base_id(key);

            OPENSSL_assert(p2 != NULL);

            switch (EVP_MD_type((const EVP_MD *)p2)) {
            case NID_id_GostR3411_94:
                if (pkey_nid == NID_id_GostR3410_2001
                    || pkey_nid == NID_id_GostR3410_94) {
                    pctx->md = (EVP_MD *)p2;
                    return 1;
                }
                break;

            case NID_id_GostR3411_2012_256:
                if (pkey_nid == NID_id_GostR3410_2012_256) {
                    pctx->md = (EVP_MD *)p2;
                    return 1;
                }
                break;

            case NID_id_GostR3411_2012_512:
                if (pkey_nid == NID_id_GostR3410_2012_512) {
                    pctx->md = (EVP_MD *)p2;
                    return 1;
                }
                break;
            }

            GOSTerr(GOST_F_PKEY_GOST_CTRL, GOST_R_INVALID_DIGEST_TYPE);
            return 0;
        }

    case EVP_PKEY_CTRL_GET_MD:
        *(const EVP_MD **)p2 = pctx->md;
        return 1;

    case EVP_PKEY_CTRL_PKCS7_ENCRYPT:
    case EVP_PKEY_CTRL_PKCS7_DECRYPT:
    case EVP_PKEY_CTRL_PKCS7_SIGN:
    case EVP_PKEY_CTRL_DIGESTINIT:
#ifndef OPENSSL_NO_CMS
    case EVP_PKEY_CTRL_CMS_ENCRYPT:
    case EVP_PKEY_CTRL_CMS_DECRYPT:
    case EVP_PKEY_CTRL_CMS_SIGN:
#endif
        return 1;

    case EVP_PKEY_CTRL_GOST_PARAMSET:
        pctx->sign_param_nid = (int)p1;
        return 1;
    case EVP_PKEY_CTRL_SET_IV:
        OPENSSL_assert(p2 != NULL);
        pctx->shared_ukm = OPENSSL_malloc((int)p1);
        if (!pctx->shared_ukm)
            return 0;
        memcpy(pctx->shared_ukm, p2, (int)p1);
        return 1;
    case EVP_PKEY_CTRL_PEER_KEY:
        if (p1 == 0 || p1 == 1) /* call from EVP_PKEY_derive_set_peer */
            return 1;
        if (p1 == 2)            /* TLS: peer key used? */
            return pctx->peer_key_used;
        if (p1 == 3)            /* TLS: peer key used! */
            return (pctx->peer_key_used = 1);
        break;
    }

    GOSTerr(GOST_F_PKEY_GOST_CTRL, GOST_R_CTRL_CALL_FAILED);
    return -2;
}

static int pkey_gost94_ctrl_str(EVP_PKEY_CTX *ctx,
                                const char *type, const char *value)
{
    int param_nid = 0;
    if (!strcmp(type, param_ctrl_string)) {
        if (!value) {
            return 0;
        }
        if (strlen(value) == 1) {
            switch (toupper((unsigned char)value[0])) {
            case 'A':
                param_nid = NID_id_GostR3410_94_CryptoPro_A_ParamSet;
                break;
            case 'B':
                param_nid = NID_id_GostR3410_94_CryptoPro_B_ParamSet;
                break;
            case 'C':
                param_nid = NID_id_GostR3410_94_CryptoPro_C_ParamSet;
                break;
            case 'D':
                param_nid = NID_id_GostR3410_94_CryptoPro_D_ParamSet;
                break;
            default:
                return 0;
            }
        } else if ((strlen(value) == 2)
                   && (toupper((unsigned char)value[0]) == 'X')) {
            switch (toupper((unsigned char)value[1])) {
            case 'A':
                param_nid = NID_id_GostR3410_94_CryptoPro_XchA_ParamSet;
                break;
            case 'B':
                param_nid = NID_id_GostR3410_94_CryptoPro_XchB_ParamSet;
                break;
            case 'C':
                param_nid = NID_id_GostR3410_94_CryptoPro_XchC_ParamSet;
                break;
            default:
                return 0;
            }
        } else {
            R3410_params *p = R3410_paramset;
            param_nid = OBJ_txt2nid(value);
            if (param_nid == NID_undef) {
                return 0;
            }
            for (; p->nid != NID_undef; p++) {
                if (p->nid == param_nid)
                    break;
            }
            if (p->nid == NID_undef) {
                GOSTerr(GOST_F_PKEY_GOST94_CTRL_STR, GOST_R_INVALID_PARAMSET);
                return 0;
            }
        }

        return pkey_gost_ctrl(ctx, EVP_PKEY_CTRL_GOST_PARAMSET,
                              param_nid, NULL);
    }
    return -2;
}

static int pkey_gost_ec_ctrl_str_256(EVP_PKEY_CTX *ctx,
                                     const char *type, const char *value)
{
    int param_nid = 0;
    if (!strcmp(type, param_ctrl_string)) {
        if (!value) {
            return 0;
        }
        if (strlen(value) == 1) {
            switch (toupper((unsigned char)value[0])) {
            case 'A':
                param_nid = NID_id_GostR3410_2001_CryptoPro_A_ParamSet;
                break;
            case 'B':
                param_nid = NID_id_GostR3410_2001_CryptoPro_B_ParamSet;
                break;
            case 'C':
                param_nid = NID_id_GostR3410_2001_CryptoPro_C_ParamSet;
                break;
            case '0':
                param_nid = NID_id_GostR3410_2001_TestParamSet;
                break;
            default:
                return 0;
            }
        } else if ((strlen(value) == 2)
                   && (toupper((unsigned char)value[0]) == 'X')) {
            switch (toupper((unsigned char)value[1])) {
            case 'A':
                param_nid = NID_id_GostR3410_2001_CryptoPro_XchA_ParamSet;
                break;
            case 'B':
                param_nid = NID_id_GostR3410_2001_CryptoPro_XchB_ParamSet;
                break;
            default:
                return 0;
            }
        } else {
            R3410_ec_params *p = R3410_2001_paramset;
            param_nid = OBJ_txt2nid(value);
            if (param_nid == NID_undef) {
                return 0;
            }
            for (; p->nid != NID_undef; p++) {
                if (p->nid == param_nid)
                    break;
            }
            if (p->nid == NID_undef) {
                GOSTerr(GOST_F_PKEY_GOST_EC_CTRL_STR_256,
                        GOST_R_INVALID_PARAMSET);
                return 0;
            }
        }

        return pkey_gost_ctrl(ctx, EVP_PKEY_CTRL_GOST_PARAMSET,
                              param_nid, NULL);
    }
    return -2;
}

static int pkey_gost_ec_ctrl_str_512(EVP_PKEY_CTX *ctx,
                                     const char *type, const char *value)
{
    int param_nid = NID_undef;

    if (strcmp(type, param_ctrl_string))
        return -2;

    if (!value)
        return 0;

    if (strlen(value) == 1) {
        switch (toupper((unsigned char)value[0])) {
        case 'A':
            param_nid = NID_id_tc26_gost_3410_2012_512_paramSetA;
            break;

        case 'B':
            param_nid = NID_id_tc26_gost_3410_2012_512_paramSetB;
            break;

        default:
            return 0;
        }
    } else {
        R3410_ec_params *p = R3410_2012_512_paramset;
        param_nid = OBJ_txt2nid(value);
        if (param_nid == NID_undef)
            return 0;

        while (p->nid != NID_undef && p->nid != param_nid)
            p++;

        if (p->nid == NID_undef) {
            GOSTerr(GOST_F_PKEY_GOST_EC_CTRL_STR_512,
                    GOST_R_INVALID_PARAMSET);
            return 0;
        }
    }

    return pkey_gost_ctrl(ctx, EVP_PKEY_CTRL_GOST_PARAMSET, param_nid, NULL);
}

/* --------------------- key generation  --------------------------------*/

static int pkey_gost_paramgen_init(EVP_PKEY_CTX *ctx)
{
    return 1;
}

static int pkey_gost94_paramgen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    struct gost_pmeth_data *data = EVP_PKEY_CTX_get_data(ctx);
    DSA *dsa = NULL;
    if (!data || data->sign_param_nid == NID_undef) {
        GOSTerr(GOST_F_PKEY_GOST94_PARAMGEN, GOST_R_NO_PARAMETERS_SET);
        return 0;
    }
    dsa = DSA_new();
    if (!fill_GOST94_params(dsa, data->sign_param_nid)
        || !EVP_PKEY_assign(pkey, NID_id_GostR3410_94, dsa)) {
        DSA_free(dsa);
        return 0;
    }
    return 1;
}

static int pkey_gost2001_paramgen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    struct gost_pmeth_data *data = EVP_PKEY_CTX_get_data(ctx);
    EC_KEY *ec = NULL;

    if (!data || data->sign_param_nid == NID_undef) {
        GOSTerr(GOST_F_PKEY_GOST01_PARAMGEN, GOST_R_NO_PARAMETERS_SET);
        return 0;
    }

    ec = EC_KEY_new();
    if (!fill_GOST_EC_params(ec, data->sign_param_nid)
        || !EVP_PKEY_assign(pkey, NID_id_GostR3410_2001, ec)) {
        EC_KEY_free(ec);
        return 0;
    }
    return 1;
}

static int pkey_gost2012_paramgen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    struct gost_pmeth_data *data = EVP_PKEY_CTX_get_data(ctx);
    EC_KEY *ec;
    int result = 0;

    if (!data || data->sign_param_nid == NID_undef) {
        GOSTerr(GOST_F_PKEY_GOST12_PARAMGEN, GOST_R_NO_PARAMETERS_SET);
        return 0;
    }

    ec = EC_KEY_new();
    if (!fill_GOST_EC_params(ec, data->sign_param_nid)) {
        EC_KEY_free(ec);
        return 0;
    }

    switch (data->sign_param_nid) {
    case NID_id_tc26_gost_3410_2012_512_paramSetA:
    case NID_id_tc26_gost_3410_2012_512_paramSetB:
        result =
            (EVP_PKEY_assign(pkey, NID_id_GostR3410_2012_512, ec)) ? 1 : 0;
        break;

    case NID_id_GostR3410_2001_CryptoPro_A_ParamSet:
    case NID_id_GostR3410_2001_CryptoPro_B_ParamSet:
    case NID_id_GostR3410_2001_CryptoPro_C_ParamSet:
    case NID_id_GostR3410_2001_CryptoPro_XchA_ParamSet:
    case NID_id_GostR3410_2001_CryptoPro_XchB_ParamSet:
    case NID_id_GostR3410_2001_TestParamSet:
        result =
            (EVP_PKEY_assign(pkey, NID_id_GostR3410_2012_256, ec)) ? 1 : 0;
        break;
    default:
        result = 0;
        break;
    }

    if (result == 0)
        EC_KEY_free(ec);

    return result;
}

/* ----------- keygen callbacks --------------------------------------*/
/* Generates Gost_R3410_94_cp key */
static int pkey_gost94cp_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    DSA *dsa;
    if (!pkey_gost94_paramgen(ctx, pkey))
        return 0;
    dsa = EVP_PKEY_get0(pkey);
    gost_sign_keygen(dsa);
    return 1;
}

/* Generates GOST_R3410 2001 key and assigns it using specified type */
static int pkey_gost2001cp_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    EC_KEY *ec;
    if (!pkey_gost2001_paramgen(ctx, pkey))
        return 0;
    ec = EVP_PKEY_get0(pkey);
    gost_ec_keygen(ec);
    return 1;
}

/* Generates GOST_R3410 2012 key and assigns it using specified type */
static int pkey_gost2012cp_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    if (!pkey_gost2012_paramgen(ctx, pkey))
        return 0;

    gost_ec_keygen(EVP_PKEY_get0(pkey));
    return 1;
}

/* ----------- sign callbacks --------------------------------------*/

static int pkey_gost94_cp_sign(EVP_PKEY_CTX *ctx, unsigned char *sig,
                               size_t *siglen, const unsigned char *tbs,
                               size_t tbs_len)
{
    DSA_SIG *unpacked_sig = NULL;
    EVP_PKEY *pkey = EVP_PKEY_CTX_get0_pkey(ctx);
    if (!siglen)
        return 0;
    if (!sig) {
        *siglen = 64;           /* better to check size of pkey->pkey.dsa-q */
        return 1;
    }
    unpacked_sig = gost_do_sign(tbs, tbs_len, EVP_PKEY_get0(pkey));
    if (!unpacked_sig) {
        return 0;
    }
    return pack_sign_cp(unpacked_sig, 32, sig, siglen);
}

static int pkey_gost_ec_cp_sign(EVP_PKEY_CTX *ctx, unsigned char *sig,
                                size_t *siglen, const unsigned char *tbs,
                                size_t tbs_len)
{
    DSA_SIG *unpacked_sig = NULL;
    EVP_PKEY *pkey = EVP_PKEY_CTX_get0_pkey(ctx);
    int order = 0;

    if (!siglen)
        return 0;
    if (!pkey)
        return 0;

    switch (EVP_PKEY_base_id(pkey)) {
    case NID_id_GostR3410_2001:
    case NID_id_GostR3410_2012_256:
        order = 64;
        break;
    case NID_id_GostR3410_2012_512:
        order = 128;
        break;
    default:
        return 0;
    }

    if (!sig) {
        *siglen = order;
        return 1;
    }
    unpacked_sig = gost_ec_sign(tbs, tbs_len, EVP_PKEY_get0(pkey));
    if (!unpacked_sig) {
        return 0;
    }
    return pack_sign_cp(unpacked_sig, order / 2, sig, siglen);
}

/* ------------------- verify callbacks ---------------------------*/

static int pkey_gost94_cp_verify(EVP_PKEY_CTX *ctx, const unsigned char *sig,
                                 size_t siglen, const unsigned char *tbs,
                                 size_t tbs_len)
{
    int ok = 0;
    EVP_PKEY *pub_key = EVP_PKEY_CTX_get0_pkey(ctx);
    DSA_SIG *s = (sig) ? unpack_cp_signature(sig, siglen) : NULL;
    if (!s)
        return 0;
    if (pub_key)
        ok = gost_do_verify(tbs, tbs_len, s, EVP_PKEY_get0(pub_key));
    DSA_SIG_free(s);
    return ok;
}

static int pkey_gost_ec_cp_verify(EVP_PKEY_CTX *ctx, const unsigned char *sig,
                                  size_t siglen, const unsigned char *tbs,
                                  size_t tbs_len)
{
    int ok = 0;
    EVP_PKEY *pub_key = EVP_PKEY_CTX_get0_pkey(ctx);
    DSA_SIG *s = (sig) ? unpack_cp_signature(sig, siglen) : NULL;
    if (!s)
        return 0;
#ifdef DEBUG_SIGN
    fprintf(stderr, "R=");
    BN_print_fp(stderr, s->r);
    fprintf(stderr, "\nS=");
    BN_print_fp(stderr, s->s);
    fprintf(stderr, "\n");
#endif
    if (pub_key)
        ok = gost_ec_verify(tbs, tbs_len, s, EVP_PKEY_get0(pub_key));
    DSA_SIG_free(s);
    return ok;
}

/* ------------- encrypt init -------------------------------------*/
/* Generates ephermeral key */
static int pkey_gost_encrypt_init(EVP_PKEY_CTX *ctx)
{
    return 1;
}

/* --------------- Derive init ------------------------------------*/
static int pkey_gost_derive_init(EVP_PKEY_CTX *ctx)
{
    return 1;
}

/* -------- PKEY_METHOD for GOST MAC algorithm --------------------*/
static int pkey_gost_mac_init(EVP_PKEY_CTX *ctx)
{
    struct gost_mac_pmeth_data *data;
    data = OPENSSL_malloc(sizeof(struct gost_mac_pmeth_data));
    if (!data)
        return 0;
    memset(data, 0, sizeof(struct gost_mac_pmeth_data));
    EVP_PKEY_CTX_set_data(ctx, data);
    return 1;
}

static void pkey_gost_mac_cleanup(EVP_PKEY_CTX *ctx)
{
    struct gost_mac_pmeth_data *data = EVP_PKEY_CTX_get_data(ctx);
    if (data)
        OPENSSL_free(data);
}

static int pkey_gost_mac_copy(EVP_PKEY_CTX *dst, EVP_PKEY_CTX *src)
{
    struct gost_mac_pmeth_data *dst_data, *src_data;
    if (!pkey_gost_mac_init(dst)) {
        return 0;
    }
    src_data = EVP_PKEY_CTX_get_data(src);
    dst_data = EVP_PKEY_CTX_get_data(dst);
    if (!src_data || !dst_data)
        return 0;

    *dst_data = *src_data;
    return 1;
}

static int pkey_gost_mac_ctrl(EVP_PKEY_CTX *ctx, int type, int p1, void *p2)
{
    struct gost_mac_pmeth_data *data =
        (struct gost_mac_pmeth_data *)EVP_PKEY_CTX_get_data(ctx);

    switch (type) {
    case EVP_PKEY_CTRL_MD:
        {
            int nid = EVP_MD_type((const EVP_MD *)p2);
            if (nid != NID_id_Gost28147_89_MAC && nid != NID_gost_mac_12) {
                GOSTerr(GOST_F_PKEY_GOST_MAC_CTRL,
                        GOST_R_INVALID_DIGEST_TYPE);
                return 0;
            }
            data->md = (EVP_MD *)p2;
            return 1;
        }
        break;

    case EVP_PKEY_CTRL_GET_MD:
        *(const EVP_MD **)p2 = data->md;
        return 1;

    case EVP_PKEY_CTRL_PKCS7_ENCRYPT:
    case EVP_PKEY_CTRL_PKCS7_DECRYPT:
    case EVP_PKEY_CTRL_PKCS7_SIGN:
        return 1;
    case EVP_PKEY_CTRL_SET_MAC_KEY:
        if (p1 != 32) {
            GOSTerr(GOST_F_PKEY_GOST_MAC_CTRL, GOST_R_INVALID_MAC_KEY_LENGTH);
            return 0;
        }

        memcpy(data->key, p2, 32);
        data->key_set = 1;
        return 1;
    case EVP_PKEY_CTRL_DIGESTINIT:
        {
            EVP_MD_CTX *mctx = p2;
            void *key;
            if (!data->key_set) {
                EVP_PKEY *pkey = EVP_PKEY_CTX_get0_pkey(ctx);
                if (!pkey) {
                    GOSTerr(GOST_F_PKEY_GOST_MAC_CTRL,
                            GOST_R_MAC_KEY_NOT_SET);
                    return 0;
                }
                key = EVP_PKEY_get0(pkey);
                if (!key) {
                    GOSTerr(GOST_F_PKEY_GOST_MAC_CTRL,
                            GOST_R_MAC_KEY_NOT_SET);
                    return 0;
                }
            } else {
                key = &(data->key);
            }
            return mctx->digest->md_ctrl(mctx, EVP_MD_CTRL_SET_KEY, 32, key);
        }
    }
    return -2;
}

static int pkey_gost_mac_ctrl_str(EVP_PKEY_CTX *ctx,
                                  const char *type, const char *value)
{
    if (!strcmp(type, key_ctrl_string)) {
        if (strlen(value) != 32) {
            GOSTerr(GOST_F_PKEY_GOST_MAC_CTRL_STR,
                    GOST_R_INVALID_MAC_KEY_LENGTH);
            return 0;
        }
        return pkey_gost_mac_ctrl(ctx, EVP_PKEY_CTRL_SET_MAC_KEY,
                                  32, (char *)value);
    }
    if (!strcmp(type, hexkey_ctrl_string)) {
        long keylen = 0;
        int ret;
        unsigned char *keybuf = string_to_hex(value, &keylen);
        if (!keybuf || keylen != 32) {
            GOSTerr(GOST_F_PKEY_GOST_MAC_CTRL_STR,
                    GOST_R_INVALID_MAC_KEY_LENGTH);
            OPENSSL_free(keybuf);
            return 0;
        }
        ret = pkey_gost_mac_ctrl(ctx, EVP_PKEY_CTRL_SET_MAC_KEY, 32, keybuf);
        OPENSSL_free(keybuf);
        return ret;

    }
    return -2;
}

static int pkey_gost_mac_keygen_base(EVP_PKEY_CTX *ctx,
                                     EVP_PKEY *pkey, int mac_nid)
{
    struct gost_mac_pmeth_data *data = EVP_PKEY_CTX_get_data(ctx);
    unsigned char *keydata;
    if (!data || !data->key_set) {
        GOSTerr(GOST_F_PKEY_GOST_MAC_KEYGEN, GOST_R_MAC_KEY_NOT_SET);
        return 0;
    }
    keydata = OPENSSL_malloc(32);
    if (!keydata)
        return 0;
    memcpy(keydata, data->key, 32);
    EVP_PKEY_assign(pkey, mac_nid, keydata);
    return 1;
}

static int pkey_gost_mac_keygen_12(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    return pkey_gost_mac_keygen_base(ctx, pkey, NID_gost_mac_12);
}

static int pkey_gost_mac_keygen(EVP_PKEY_CTX *ctx, EVP_PKEY *pkey)
{
    return pkey_gost_mac_keygen_base(ctx, pkey, NID_id_Gost28147_89_MAC);
}

static int pkey_gost_mac_signctx_init(EVP_PKEY_CTX *ctx, EVP_MD_CTX *mctx)
{
    return 1;
}

static int pkey_gost_mac_signctx(EVP_PKEY_CTX *ctx, unsigned char *sig,
                                 size_t *siglen, EVP_MD_CTX *mctx)
{
    unsigned int tmpsiglen;
    int ret;

    if (!siglen)
        return 0;
    tmpsiglen = *siglen;        /* for platforms where sizeof(int) !=
                                 * sizeof(size_t) */

    if (!sig) {
        *siglen = 4;
        return 1;
    }
    ret = EVP_DigestFinal_ex(mctx, sig, &tmpsiglen);
    *siglen = tmpsiglen;
    return ret;
}

/* ----------------------------------------------------------------*/
int register_pmeth_gost(int id, EVP_PKEY_METHOD **pmeth, int flags)
{
    *pmeth = EVP_PKEY_meth_new(id, flags);
    if (!*pmeth)
        return 0;

    switch (id) {
    case NID_id_GostR3410_94:
        EVP_PKEY_meth_set_ctrl(*pmeth, pkey_gost_ctrl, pkey_gost94_ctrl_str);
        EVP_PKEY_meth_set_keygen(*pmeth, NULL, pkey_gost94cp_keygen);
        EVP_PKEY_meth_set_sign(*pmeth, NULL, pkey_gost94_cp_sign);
        EVP_PKEY_meth_set_verify(*pmeth, NULL, pkey_gost94_cp_verify);
        EVP_PKEY_meth_set_encrypt(*pmeth,
                                  pkey_gost_encrypt_init,
                                  pkey_GOST94cp_encrypt);
        EVP_PKEY_meth_set_decrypt(*pmeth, NULL, pkey_GOST94cp_decrypt);
        EVP_PKEY_meth_set_derive(*pmeth,
                                 pkey_gost_derive_init, pkey_gost94_derive);
        EVP_PKEY_meth_set_paramgen(*pmeth, pkey_gost_paramgen_init,
                                   pkey_gost94_paramgen);
        break;
    case NID_id_GostR3410_2001:
        EVP_PKEY_meth_set_ctrl(*pmeth,
                               pkey_gost_ctrl, pkey_gost_ec_ctrl_str_256);
        EVP_PKEY_meth_set_sign(*pmeth, NULL, pkey_gost_ec_cp_sign);
        EVP_PKEY_meth_set_verify(*pmeth, NULL, pkey_gost_ec_cp_verify);

        EVP_PKEY_meth_set_keygen(*pmeth, NULL, pkey_gost2001cp_keygen);

        EVP_PKEY_meth_set_encrypt(*pmeth,
                                  pkey_gost_encrypt_init,
                                  pkey_GOST_ECcp_encrypt);
        EVP_PKEY_meth_set_decrypt(*pmeth, NULL, pkey_GOST_ECcp_decrypt);
        EVP_PKEY_meth_set_derive(*pmeth,
                                 pkey_gost_derive_init, pkey_gost_ec_derive);
        EVP_PKEY_meth_set_paramgen(*pmeth, pkey_gost_paramgen_init,
                                   pkey_gost2001_paramgen);
        break;
    case NID_id_GostR3410_2012_256:
        EVP_PKEY_meth_set_ctrl(*pmeth,
                               pkey_gost_ctrl, pkey_gost_ec_ctrl_str_256);
        EVP_PKEY_meth_set_sign(*pmeth, NULL, pkey_gost_ec_cp_sign);
        EVP_PKEY_meth_set_verify(*pmeth, NULL, pkey_gost_ec_cp_verify);

        EVP_PKEY_meth_set_keygen(*pmeth, NULL, pkey_gost2012cp_keygen);

        EVP_PKEY_meth_set_encrypt(*pmeth,
                                  pkey_gost_encrypt_init,
                                  pkey_GOST_ECcp_encrypt);
        EVP_PKEY_meth_set_decrypt(*pmeth, NULL, pkey_GOST_ECcp_decrypt);
        EVP_PKEY_meth_set_derive(*pmeth,
                                 pkey_gost_derive_init, pkey_gost_ec_derive);
        EVP_PKEY_meth_set_paramgen(*pmeth,
                                   pkey_gost_paramgen_init,
                                   pkey_gost2012_paramgen);
        break;
    case NID_id_GostR3410_2012_512:
        EVP_PKEY_meth_set_ctrl(*pmeth,
                               pkey_gost_ctrl, pkey_gost_ec_ctrl_str_512);
        EVP_PKEY_meth_set_sign(*pmeth, NULL, pkey_gost_ec_cp_sign);
        EVP_PKEY_meth_set_verify(*pmeth, NULL, pkey_gost_ec_cp_verify);

        EVP_PKEY_meth_set_keygen(*pmeth, NULL, pkey_gost2012cp_keygen);

        EVP_PKEY_meth_set_encrypt(*pmeth,
                                  pkey_gost_encrypt_init,
                                  pkey_GOST_ECcp_encrypt);
        EVP_PKEY_meth_set_decrypt(*pmeth, NULL, pkey_GOST_ECcp_decrypt);
        EVP_PKEY_meth_set_derive(*pmeth,
                                 pkey_gost_derive_init, pkey_gost_ec_derive);
        EVP_PKEY_meth_set_paramgen(*pmeth,
                                   pkey_gost_paramgen_init,
                                   pkey_gost2012_paramgen);
        break;
    case NID_id_Gost28147_89_MAC:
        EVP_PKEY_meth_set_ctrl(*pmeth, pkey_gost_mac_ctrl,
                               pkey_gost_mac_ctrl_str);
        EVP_PKEY_meth_set_signctx(*pmeth, pkey_gost_mac_signctx_init,
                                  pkey_gost_mac_signctx);
        EVP_PKEY_meth_set_keygen(*pmeth, NULL, pkey_gost_mac_keygen);
        EVP_PKEY_meth_set_init(*pmeth, pkey_gost_mac_init);
        EVP_PKEY_meth_set_cleanup(*pmeth, pkey_gost_mac_cleanup);
        EVP_PKEY_meth_set_copy(*pmeth, pkey_gost_mac_copy);
        return 1;
    case NID_gost_mac_12:
        EVP_PKEY_meth_set_ctrl(*pmeth, pkey_gost_mac_ctrl,
                               pkey_gost_mac_ctrl_str);
        EVP_PKEY_meth_set_signctx(*pmeth, pkey_gost_mac_signctx_init,
                                  pkey_gost_mac_signctx);
        EVP_PKEY_meth_set_keygen(*pmeth, NULL, pkey_gost_mac_keygen_12);
        EVP_PKEY_meth_set_init(*pmeth, pkey_gost_mac_init);
        EVP_PKEY_meth_set_cleanup(*pmeth, pkey_gost_mac_cleanup);
        EVP_PKEY_meth_set_copy(*pmeth, pkey_gost_mac_copy);
        return 1;
    default:                   /* Unsupported method */
        return 0;
    }
    EVP_PKEY_meth_set_init(*pmeth, pkey_gost_init);
    EVP_PKEY_meth_set_cleanup(*pmeth, pkey_gost_cleanup);

    EVP_PKEY_meth_set_copy(*pmeth, pkey_gost_copy);
    /*
     * FIXME derive etc...
     */

    return 1;
}