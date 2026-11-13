/*
 * OPC UA varnost.
 *
 *  - prijava z uporabniskim imenom in geslom; gesla so v config.json shranjena
 *    kot crypt(3) hash (npr. "openssl passwd -6"), nikoli kot cisto besedilo
 *  - sifriran kanal (Basic256Sha256 in novejsi) s strezniskim certifikatom
 *  - STROGO zaupanje odjemalcem: sifrirani odjemalci morajo imeti certifikat
 *    (.der) v mapi trusted_dir; prazna mapa = streznik se ne zazene
 *    (odprta knjiznica bi sicer pri prazni listi sprejela VSE certifikate)
 *  - require_encryption=false doda se endpoint brez sifriranja za stare
 *    odjemalce (SCADA); uporabnik in geslo sta se vedno obvezna, anonimnega
 *    dostopa ni, razen ce je izrecno dovoljen z allow_anonymous
 *
 * Napisano po API-ju open62541 1.4. Prevajanja proti pravim glavam
 * (/usr/include/open62541) nisem mogel preveriti.
 */
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/accesscontrol_default.h>

#include <crypt.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "gateway.h"

#ifndef UA_ENABLE_ENCRYPTION
#error "open62541 mora biti prevedena z UA_ENABLE_ENCRYPTION (mbedTLS ali OpenSSL)"
#endif

#define OPCUA_PORT          4840
#define LOGIN_MAX_FAILURES  5     // toliko napacnih gesel zapored ...
#define LOGIN_LOCKOUT_SEC   60    // ... zaklene uporabnika za toliko sekund

// Geslo v UserNameIdentityToken se sifrira s to politiko tudi na kanalu brez
// sifriranja (odjemalec uporabi streznikov certifikat).
static UA_String g_token_policy_uri =
    {sizeof("http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256") - 1,
     (UA_Byte*)"http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"};

// Privzeta prijava iz knjiznice potrebuje seznam uporabnikov, da objavi
// token policy za uporabnisko ime; dejansko preverjanje naredi login_callback.
static UA_UsernamePasswordLogin g_login_placeholders[MAX_USERS];

/* ---------- datoteke in zaupani certifikati ---------- */

static int load_file(const char *path, UA_ByteString *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    UA_ByteString_init(out);
    if (UA_ByteString_allocBuffer(out, (size_t)n) != UA_STATUSCODE_GOOD) { fclose(f); return -1; }
    size_t got = fread(out->data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { UA_ByteString_clear(out); return -1; }
    return 0;
}

static int has_der_extension(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".der") == 0;
}

static void free_trust_list(UA_ByteString *list, size_t count) {
    for (size_t i = 0; i < count; i++) UA_ByteString_clear(&list[i]);
    free(list);
}

static int load_trust_list(const char *dir, UA_ByteString **list, size_t *count) {
    *list = NULL;
    *count = 0;
    DIR *d = opendir(dir);
    if (!d) return -1;

    size_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!has_der_extension(e->d_name)) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        UA_ByteString bs;
        if (load_file(path, &bs) != 0) {
            printf("[SECURITY] Ne morem prebrati %s - preskocen\n", path);
            continue;
        }
        if (*count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            UA_ByteString *nl = (UA_ByteString*)realloc(*list, ncap * sizeof(UA_ByteString));
            if (!nl) { UA_ByteString_clear(&bs); closedir(d); return -1; }
            *list = nl;
            cap = ncap;
        }
        (*list)[(*count)++] = bs;
    }
    closedir(d);
    return 0;
}

/* ---------- prijava ---------- */

typedef struct {
    unsigned failures;
    time_t locked_until;
} LoginState;

static LoginState g_login_state[MAX_USERS]; // callback tece samo v niti OPC UA streznika

static int ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static UA_StatusCode login_callback(const UA_String *userName, const UA_ByteString *password,
                                    size_t loginSize, const UA_UsernamePasswordLogin *logins,
                                    void **sessionContext, void *loginContext) {
    (void)loginSize; (void)logins; (void)sessionContext; (void)loginContext;

    char user[sizeof(global_config.users[0].username)];
    char pass[128];

    if (userName->length == 0 || userName->length >= sizeof(user) ||
        password->length >= sizeof(pass)) {
        printf("[SECURITY] Prijava zavrnjena: neveljavna dolzina uporabnika ali gesla\n");
        return UA_STATUSCODE_BADUSERACCESSDENIED;
    }
    memcpy(user, userName->data, userName->length);
    user[userName->length] = '\0';
    if (password->length) memcpy(pass, password->data, password->length);
    pass[password->length] = '\0';

    // ime prihaja od odjemalca - ne spustimo kontrolnih znakov v log
    for (char *p = user; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = '?';
    }

    int idx = -1;
    for (size_t i = 0; i < global_config.user_count; i++) {
        if (strcmp(global_config.users[i].username, user) == 0) { idx = (int)i; break; }
    }

    time_t now = time(NULL);
    UA_StatusCode result = UA_STATUSCODE_BADUSERACCESSDENIED;

    if (idx < 0) {
        printf("[SECURITY] Prijava zavrnjena: neznan uporabnik '%s'\n", user);
    } else if (now < g_login_state[idx].locked_until) {
        printf("[SECURITY] Prijava zavrnjena: uporabnik '%s' je zacasno zaklenjen (se %ld s)\n",
               user, (long)(g_login_state[idx].locked_until - now));
    } else {
        static struct crypt_data cd;
        memset(&cd, 0, sizeof(cd));
        const char *stored = global_config.users[idx].password_hash;
        char *calc = crypt_r(pass, stored, &cd);

        if (calc && ct_equal(calc, stored)) {
            g_login_state[idx].failures = 0;
            printf("[SECURITY] Prijava uspesna: uporabnik '%s'\n", user);
            result = UA_STATUSCODE_GOOD;
        } else if (++g_login_state[idx].failures >= LOGIN_MAX_FAILURES) {
            g_login_state[idx].failures = 0;
            g_login_state[idx].locked_until = now + LOGIN_LOCKOUT_SEC;
            printf("[SECURITY] Prijava zavrnjena: napacno geslo za '%s' - uporabnik zaklenjen za %d s\n",
                   user, LOGIN_LOCKOUT_SEC);
        } else {
            printf("[SECURITY] Prijava zavrnjena: napacno geslo za '%s' (%u/%d)\n",
                   user, g_login_state[idx].failures, LOGIN_MAX_FAILURES);
        }
        explicit_bzero(&cd, sizeof(cd));
    }

    explicit_bzero(pass, sizeof(pass));
    return result;
}

/* ---------- endpointi ---------- */

static int uri_has_suffix(const UA_String *uri, const char *suffix) {
    size_t n = strlen(suffix);
    return uri->length >= n && memcmp(uri->data + uri->length - n, suffix, n) == 0;
}

// Odstrani endpointe, ki jih ne zelimo: brez sifriranja (ce je sifriranje
// obvezno) ter zastarele politike Basic128Rsa15 in Basic256 (SHA-1).
static int prune_endpoints(UA_ServerConfig *config, int require_encryption) {
    size_t w = 0;
    for (size_t r = 0; r < config->endpointsSize; r++) {
        UA_EndpointDescription *e = &config->endpoints[r];
        int drop = (require_encryption && e->securityMode == UA_MESSAGESECURITYMODE_NONE) ||
                   uri_has_suffix(&e->securityPolicyUri, "#Basic128Rsa15") ||
                   uri_has_suffix(&e->securityPolicyUri, "#Basic256");
        if (drop) {
            UA_EndpointDescription_clear(e);
        } else {
            if (w != r) config->endpoints[w] = *e;
            w++;
        }
    }
    config->endpointsSize = w;
    return w > 0 ? 0 : -1;
}

// URI aplikacije mora biti enak URI-ju v SAN strezniskega certifikata,
// sicer ga odjemalci (UaExpert) zavrnejo ali opozarjajo.
static void set_application_uri(UA_ServerConfig *config, const char *uri) {
    UA_String_clear(&config->applicationDescription.applicationUri);
    config->applicationDescription.applicationUri = UA_STRING_ALLOC(uri);
    for (size_t i = 0; i < config->endpointsSize; i++) {
        UA_String_clear(&config->endpoints[i].server.applicationUri);
        UA_String_copy(&config->applicationDescription.applicationUri,
                       &config->endpoints[i].server.applicationUri);
    }
}

/* ---------- glavna nastavitev ---------- */

int configure_opcua_security(UA_ServerConfig *config) {
    const int allow_anon = global_config.allow_anonymous ? 1 : 0;
    const int require_enc = global_config.require_encryption ? 1 : 0;

    if (!allow_anon && global_config.user_count == 0) {
        fprintf(stderr, "[SECURITY] V config.json ni nobenega uporabnika (\"users\"), anonimni dostop pa je "
                        "izklopljen - nihce se ne bi mogel prijaviti.\n");
        return -1;
    }
    for (size_t i = 0; i < global_config.user_count; i++) {
        const UserConfig *u = &global_config.users[i];
        if (u->username[0] == '\0' || u->password_hash[0] != '$') {
            fprintf(stderr, "[SECURITY] Uporabnik #%zu ('%s'): \"password_hash\" mora biti crypt(3) hash "
                            "(ustvari ga z: openssl passwd -6), ne geslo v cistem besedilu.\n",
                    i + 1, u->username);
            return -1;
        }
    }

    // 1. streznikov certifikat in kljuc (DER)
    UA_ByteString cert = UA_BYTESTRING_NULL, key = UA_BYTESTRING_NULL;
    if (load_file(global_config.pki_certificate, &cert) != 0 ||
        load_file(global_config.pki_private_key, &key) != 0) {
        fprintf(stderr, "[SECURITY] Ne morem prebrati strezniskega certifikata (%s) ali kljuca (%s). "
                        "Ustvari ju s: tools/make_pki.sh\n",
                global_config.pki_certificate, global_config.pki_private_key);
        UA_ByteString_clear(&cert);
        UA_ByteString_clear(&key);
        return -1;
    }

    // 2. zaupani certifikati odjemalcev - STROGO (fail closed)
    UA_ByteString *trust = NULL;
    size_t trust_n = 0;
    if (load_trust_list(global_config.pki_trusted_dir, &trust, &trust_n) != 0 || trust_n == 0) {
        fprintf(stderr, "[SECURITY] Mapa zaupanih certifikatov (%s) ne obstaja ali nima nobenega .der "
                        "certifikata odjemalca. Dodaj ga s: tools/trust_client.sh <certifikat>. "
                        "Brez tega bi knjiznica sprejela vse certifikate, zato streznik ne zazene.\n",
                global_config.pki_trusted_dir);
        free_trust_list(trust, trust_n);
        UA_ByteString_clear(&cert);
        UA_ByteString_clear(&key);
        return -1;
    }

    // 3. politike in endpointi
    UA_StatusCode rv = UA_ServerConfig_setDefaultWithSecurityPolicies(
        config, OPCUA_PORT, &cert, &key, trust, trust_n, NULL, 0, NULL, 0);
    UA_ByteString_clear(&cert);
    UA_ByteString_clear(&key);
    free_trust_list(trust, trust_n);
    if (rv != UA_STATUSCODE_GOOD) {
        fprintf(stderr, "[SECURITY] UA_ServerConfig_setDefaultWithSecurityPolicies ni uspel (koda 0x%08X)\n",
                (unsigned)rv);
        return -1;
    }

    if (prune_endpoints(config, require_enc) != 0) {
        fprintf(stderr, "[SECURITY] Po odstranitvi nezelenih endpointov ni ostal noben endpoint.\n");
        return -1;
    }
    set_application_uri(config, global_config.pki_application_uri);

    // 4. nadzor dostopa: tokeni za uporabnika/geslo (+ anonimni, ce je dovoljen).
    //    Endpointi morajo biti ze dokoncni - knjiznica jim doda token policy.
    for (size_t i = 0; i < global_config.user_count; i++) {
        g_login_placeholders[i].username = UA_STRING(global_config.users[i].username);
        g_login_placeholders[i].password = UA_STRING("-"); // ni v uporabi, preverja login_callback
    }
    rv = UA_AccessControl_defaultWithLoginCallback(config, allow_anon ? true : false,
                                                   &g_token_policy_uri,
                                                   global_config.user_count,
                                                   global_config.user_count ? g_login_placeholders : NULL,
                                                   login_callback, NULL);
    if (rv != UA_STATUSCODE_GOOD) {
        fprintf(stderr, "[SECURITY] Nastavitev nadzora dostopa ni uspela (koda 0x%08X)\n", (unsigned)rv);
        return -1;
    }

    printf("[SECURITY] OPC UA: uporabnikov=%zu, zaupanih certifikatov odjemalcev=%zu, anonimni dostop=%s, "
           "sifriranje=%s, URI aplikacije=%s\n",
           global_config.user_count, trust_n, allow_anon ? "DA" : "ne",
           require_enc ? "obvezno" : "NI obvezno", global_config.pki_application_uri);
    if (allow_anon) {
        printf("[SECURITY] OPOZORILO: anonimni dostop je dovoljen (allow_anonymous=true) - vsak v omrezju lahko pise v naprave!\n");
    }
    if (!require_enc) {
        printf("[SECURITY] OPOZORILO: obstaja tudi endpoint brez sifriranja (za stare odjemalce). "
               "Omeji dostop do porta %d z ognjenim zidom.\n", OPCUA_PORT);
    }
    return 0;
}
