/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Linaro Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#define _GNU_SOURCE

#include "py/mpconfig.h"
#if MICROPY_PY_USSL && MICROPY_SSL_MBEDTLS

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <esp_log.h>


#include "py/nlr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/obj.h"

// mbedtls_time_t
#include "mbedtls/platform.h"
#include "mbedtls/net.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"

#include "letsencrypt.h"

#define TAG "modussl_mbedtls.c"

typedef struct _mp_obj_ssl_socket_t {
    mp_obj_base_t base;
    mp_obj_t sock;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pkey;
} mp_obj_ssl_socket_t;

struct ssl_args {
    mp_arg_val_t key;
    mp_arg_val_t cert;
    mp_arg_val_t server_side;
    mp_arg_val_t server_hostname;
};

STATIC const mp_obj_type_t ussl_socket_type;

static void mbedtls_debug(void *ctx, int level, const char *file, int line, const char *str) {
    printf("DBG:%s:%04d: %s\n", file, line, str);
}

int _mbedtls_ssl_send(void *ctx, const byte *buf, size_t len) {
    mp_obj_ssl_socket_t *o = (mp_obj_ssl_socket_t*)ctx;
    mp_obj_t sock = o->sock;

    const mp_stream_p_t *sock_stream = mp_get_stream_raise(sock, MP_STREAM_OP_WRITE);
    int err;

    int out_sz = sock_stream->write(sock, buf, len, &err);
    if (out_sz == MP_STREAM_ERROR) {
        if (mp_is_nonblocking_error(err)) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }

        char errstr[256];
        mbedtls_strerror(err, errstr, sizeof(errstr));
        ESP_LOGW(TAG, "sock_stream->write(): error %d: %s", -err, errstr);

        return -err;
    }
    return out_sz;
}

int _mbedtls_ssl_recv(void *ctx, byte *buf, size_t len) {
    mp_obj_ssl_socket_t *o = (mp_obj_ssl_socket_t*)ctx;
    mp_obj_t sock = o->sock;

    const mp_stream_p_t *sock_stream = mp_get_stream_raise(sock, MP_STREAM_OP_READ);
    int err;
    int out_sz = sock_stream->read(sock, buf, len, &err);
    if (out_sz == MP_STREAM_ERROR) {
        if (mp_is_nonblocking_error(err)) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }

        char errstr[256];
        mbedtls_strerror(err, errstr, sizeof(errstr));
        ESP_LOGW(TAG, "sock_stream->read(): error %d: %s", -err, errstr);

        return -err;
    }
    return out_sz;
}

STATIC mp_obj_ssl_socket_t *socket_new(mp_obj_t sock, struct ssl_args *args) {
    mp_obj_ssl_socket_t *o = m_new_obj(mp_obj_ssl_socket_t);
    o->base.type = &ussl_socket_type;

    int ret;
    mbedtls_ssl_init(&o->ssl);
    mbedtls_ssl_config_init(&o->conf);
    mbedtls_x509_crt_init(&o->cacert);
    mbedtls_x509_crt_init(&o->cert);
    mbedtls_pk_init(&o->pkey);
    mbedtls_ctr_drbg_init(&o->ctr_drbg);
    // Debug level (0-4)
    mbedtls_debug_set_threshold(0);

    mbedtls_entropy_init(&o->entropy);
    ret = mbedtls_ctr_drbg_seed(&o->ctr_drbg, mbedtls_entropy_func, &o->entropy, NULL, 0);
    if (ret != 0) {
      char errstr[256];
      mbedtls_strerror(ret, errstr, sizeof(errstr));
      ESP_LOGW(TAG, "mbedtls_ctr_drbg_seed(): error %d: %s", -ret, errstr);

      mp_raise_OSError(MP_EIO);
    }

    bool sha2017_subdomain = false;
    if (args->server_hostname.u_obj != mp_const_none) {
      const char *sni = mp_obj_str_get_str(args->server_hostname.u_obj);
      char *ptr;
      sha2017_subdomain = ((ptr = strcasestr(sni, ".sha2017.org")) != NULL && ptr[12] == 0);
      if (sha2017_subdomain) {
        printf("Validating certificate for: %s\n", sni);
      } else {
        printf("Warning: %s SSL certificate is not validated\n", sni);
      }
    }

    if (sha2017_subdomain) {
        ret = mbedtls_x509_crt_parse_der(&o->cacert, letsencrypt, 856);
        if(ret < 0) {
            char errstr[256];
            mbedtls_strerror(ret, errstr, sizeof(errstr));
            ESP_LOGW(TAG, "mbedtls_x509_crt_parse_der(): error %d: %s", -ret, errstr);

            mp_raise_OSError(MP_EIO);
        }
    }

    ret = mbedtls_ssl_config_defaults(&o->conf,
                    args->server_side.u_bool ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                    MBEDTLS_SSL_TRANSPORT_STREAM,
                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
      char errstr[256];
      mbedtls_strerror(ret, errstr, sizeof(errstr));
      ESP_LOGW(TAG, "mbedtls_ssl_config_defaults(): error %d: %s", -ret, errstr);

      mp_raise_OSError(MP_EIO);
    }

    if (sha2017_subdomain) {
      mbedtls_ssl_conf_authmode(&o->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
      mbedtls_ssl_conf_ca_chain(&o->conf, &o->cacert, NULL);
    } else {
      mbedtls_ssl_conf_authmode(&o->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&o->conf, mbedtls_ctr_drbg_random, &o->ctr_drbg);
    mbedtls_ssl_conf_dbg(&o->conf, mbedtls_debug, NULL);

    ret = mbedtls_ssl_setup(&o->ssl, &o->conf);
    if (ret != 0) {
      char errstr[256];
      mbedtls_strerror(ret, errstr, sizeof(errstr));
      ESP_LOGW(TAG, "mbedtls_ssl_setup(): error %d: %s", -ret, errstr);

      mp_raise_OSError(MP_EIO);
    }

    if (args->server_hostname.u_obj != mp_const_none) {
        const char *sni = mp_obj_str_get_str(args->server_hostname.u_obj);
        ret = mbedtls_ssl_set_hostname(&o->ssl, sni);
        if (ret != 0) {
            char errstr[256];
            mbedtls_strerror(ret, errstr, sizeof(errstr));
            ESP_LOGW(TAG, "mbedtls_ssl_set_hostname(): error %d: %s", -ret, errstr);

            mp_raise_OSError(MP_EIO);
        }
    }

    o->sock = sock;

    mbedtls_ssl_set_bio(&o->ssl, o, _mbedtls_ssl_send, _mbedtls_ssl_recv, NULL);

    if (args->key.u_obj != MP_OBJ_NULL) {
        size_t key_len;
        const byte *key = (const byte*)mp_obj_str_get_data(args->key.u_obj, &key_len);
        // len should include terminating null
        ret = mbedtls_pk_parse_key(&o->pkey, key, key_len + 1, NULL, 0);
        if (ret != 0) {
            char errstr[256];
            mbedtls_strerror(ret, errstr, sizeof(errstr));
            ESP_LOGW(TAG, "mbedtls_pk_parse_key(): error %d: %s", -ret, errstr);

            mp_raise_OSError(MP_EIO);
        }

        size_t cert_len;
        const byte *cert = (const byte*)mp_obj_str_get_data(args->cert.u_obj, &cert_len);
        // len should include terminating null
        ret = mbedtls_x509_crt_parse(&o->cert, cert, cert_len + 1);
        if (ret != 0) {
            char errstr[256];
            mbedtls_strerror(ret, errstr, sizeof(errstr));
            ESP_LOGW(TAG, "mbedtls_x509_crt_parse(): error %d: %s", -ret, errstr);

            mp_raise_OSError(MP_EIO);
        }

        ret = mbedtls_ssl_conf_own_cert(&o->conf, &o->cert, &o->pkey);
        if (ret != 0) {
            char errstr[256];
            mbedtls_strerror(ret, errstr, sizeof(errstr));
            ESP_LOGW(TAG, "mbedtls_ssl_conf_own_cert(): error %d: %s", -ret, errstr);

            mp_raise_OSError(MP_EIO);
        }
    }

    if (args->server_side.u_bool) {
        ESP_LOGW(TAG, "args->server_side.u_bool set");
        mp_raise_OSError(MP_EIO);
    } else {
        while ((ret = mbedtls_ssl_handshake(&o->ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char errstr[256];
                mbedtls_strerror(ret, errstr, sizeof(errstr));
                ESP_LOGW(TAG, "mbedtls_ssl_handshake(): error %d: %s", -ret, errstr);

                mp_raise_OSError(MP_EIO);
            }
        }
    }

    return o;
}

STATIC mp_obj_t mod_ssl_getpeercert(mp_obj_t o_in, mp_obj_t binary_form) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(o_in);
    if (!mp_obj_is_true(binary_form)) {
        mp_raise_NotImplementedError(NULL);
    }
    const mbedtls_x509_crt* peer_cert = mbedtls_ssl_get_peer_cert(&o->ssl);
    return mp_obj_new_bytes(peer_cert->raw.p, peer_cert->raw.len);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_ssl_getpeercert_obj, mod_ssl_getpeercert);

STATIC void socket_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<_SSLSocket %p>", self);
}

STATIC mp_uint_t socket_read(mp_obj_t o_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(o_in);

    int ret = mbedtls_ssl_read(&o->ssl, buf, size);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        // end of stream
        return 0;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        *errcode = EWOULDBLOCK;
        return 0;
    }

    if (ret >= 0) {
        return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
        ret = MP_EWOULDBLOCK;
    }
    *errcode = ret;
    return MP_STREAM_ERROR;
}

STATIC mp_uint_t socket_write(mp_obj_t o_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(o_in);

    int ret = mbedtls_ssl_write(&o->ssl, buf, size);
    if (ret >= 0) {
        return ret;
    }

    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        ret = MP_EWOULDBLOCK;
    }
    *errcode = ret;
    return MP_STREAM_ERROR;
}

STATIC mp_obj_t socket_setblocking(mp_obj_t self_in, mp_obj_t flag_in) {
    mp_obj_ssl_socket_t *o = MP_OBJ_TO_PTR(self_in);
    mp_obj_t sock = o->sock;
    mp_obj_t dest[3];
    mp_load_method(sock, MP_QSTR_setblocking, dest);
    dest[2] = flag_in;
    return mp_call_method_n_kw(1, 0, dest);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_setblocking_obj, socket_setblocking);

STATIC mp_obj_t socket_close(mp_obj_t self_in) {
    mp_obj_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);

    mbedtls_pk_free(&self->pkey);
    mbedtls_x509_crt_free(&self->cert);
    mbedtls_x509_crt_free(&self->cacert);
    mbedtls_ssl_free(&self->ssl);
    mbedtls_ssl_config_free(&self->conf);
    mbedtls_ctr_drbg_free(&self->ctr_drbg);
    mbedtls_entropy_free(&self->entropy);

    return mp_stream_close(self->sock);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_close_obj, socket_close);

STATIC const mp_rom_map_elem_t ussl_socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&socket_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_getpeercert), MP_ROM_PTR(&mod_ssl_getpeercert_obj) },
};

STATIC MP_DEFINE_CONST_DICT(ussl_socket_locals_dict, ussl_socket_locals_dict_table);

STATIC const mp_stream_p_t ussl_socket_stream_p = {
    .read = socket_read,
    .write = socket_write,
};

STATIC const mp_obj_type_t ussl_socket_type = {
    { &mp_type_type },
    // Save on qstr's, reuse same as for module
    .name = MP_QSTR_ussl,
    .print = socket_print,
    .getiter = NULL,
    .iternext = NULL,
    .protocol = &ussl_socket_stream_p,
    .locals_dict = (void*)&ussl_socket_locals_dict,
};

STATIC mp_obj_t mod_ssl_wrap_socket(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // TODO: Implement more args
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_server_side, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_server_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    // TODO: Check that sock implements stream protocol
    mp_obj_t sock = pos_args[0];

    struct ssl_args args;
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, (mp_arg_val_t*)&args);

    return MP_OBJ_FROM_PTR(socket_new(sock, &args));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(mod_ssl_wrap_socket_obj, 1, mod_ssl_wrap_socket);

STATIC const mp_rom_map_elem_t mp_module_ssl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ussl) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket), MP_ROM_PTR(&mod_ssl_wrap_socket_obj) },
};

STATIC MP_DEFINE_CONST_DICT(mp_module_ssl_globals, mp_module_ssl_globals_table);

const mp_obj_module_t mp_module_ussl = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_ssl_globals,
};

#endif // MICROPY_PY_USSL
