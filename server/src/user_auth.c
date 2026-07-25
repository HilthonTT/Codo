#define _GNU_SOURCE

// crypto.h sets _POSIX_C_SOURCE itself, so it must come before any system
// header has locked feature macros in. It also defines a MAX_KEY_SIZE that
// clashes with storage.h's (max crypto key bytes vs max btree key bytes);
// only the btree limit is used in this file, so let storage.h's win.
#include "crypto.h"
#undef MAX_KEY_SIZE

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "env.h"
#include "http_protocol.h"
#include "json_util.h"
#include "jwt.h"
#include "route.h"
#include "server.h"
#include "storage.h"
#include "user_auth.h"

// PBKDF2-HMAC-SHA256 parameters. The iteration count is stored per user
// record ("iter"), so it can be raised later without invalidating existing
// accounts.
#define PBKDF2_ITERATIONS 100000
#define PW_SALT_SIZE 16
#define PW_HASH_SIZE 32

#define USER_KEY_PREFIX "user:"

#define MIN_USERNAME_LEN 3
#define MAX_USERNAME_LEN 64 // including the NUL; must fit conn->auth_username
#define MIN_PASSWORD_LEN 8
#define MAX_PASSWORD_LEN 128

// User ids are handed out from a process-wide counter seeded at startup from
// the highest id already on disk, mirroring the todo id scheme.
static _Atomic uint64_t g_next_user_id = 1;
static long g_token_ttl = 3600;

typedef struct
{
  uint64_t id;
  char username[MAX_USERNAME_LEN];
  uint8_t salt[PW_SALT_SIZE];
  uint8_t hash[PW_HASH_SIZE];
  uint64_t iterations;
} user_record_t;

static const char *request_header(const http_request_t *request, const char *name)
{
  for (int i = 0; i < request->header_count; i++)
  {
    if (strcasecmp(request->headers[i].name, name) == 0)
    {
      return request->headers[i].value;
    }
  }
  return NULL;
}

static int send_json(connection_t *conn, http_request_t *request,
                     http_response_t *response, http_status_t status,
                     const char *json)
{
  response->status = status;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");

  if (response->body)
  {
    free(response->body);
    response->body = NULL;
  }
  response->body = strdup(json ? json : "");
  if (!response->body)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }
  response->body_length = strlen(response->body);
  response->keep_alive = request->keep_alive;

  snprintf(response->headers[0].name, sizeof(response->headers[0].name), "Content-Type");
  snprintf(response->headers[0].value, sizeof(response->headers[0].value), "application/json");
  response->header_count = 1;

  return send_http_response(conn, response);
}

static int send_error_json(connection_t *conn, http_request_t *request,
                           http_response_t *response, http_status_t status,
                           const char *msg)
{
  char body[192];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);

  int rc = 0;
  response->status = status;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  if (response->body)
  {
    free(response->body);
    response->body = NULL;
  }
  response->body = strdup(body);
  response->body_length = response->body ? strlen(response->body) : 0;
  response->keep_alive = request->keep_alive;

  int h = 0;
  snprintf(response->headers[h].name, sizeof(response->headers[h].name), "Content-Type");
  snprintf(response->headers[h].value, sizeof(response->headers[h].value), "application/json");
  h++;
  if (status == HTTP_UNAUTHORIZED)
  {
    snprintf(response->headers[h].name, sizeof(response->headers[h].name), "WWW-Authenticate");
    snprintf(response->headers[h].value, sizeof(response->headers[h].value),
             "Bearer realm=\"codo\", charset=\"UTF-8\"");
    h++;
  }
  response->header_count = h;

  rc = send_http_response(conn, response);
  return rc;
}

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++)
  {
    out[i * 2] = digits[in[i] >> 4];
    out[i * 2 + 1] = digits[in[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

static int hex_nibble(char c)
{
  if (c >= '0' && c <= '9')
  {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f')
  {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F')
  {
    return c - 'A' + 10;
  }
  return -1;
}

// Decode exactly n bytes (2n hex chars, then NUL) into out.
static bool hex_decode(const char *in, uint8_t *out, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    int hi = hex_nibble(in[i * 2]);
    int lo = hex_nibble(in[i * 2 + 1]);
    if (hi < 0 || lo < 0)
    {
      return false;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return in[n * 2] == '\0';
}

// Usernames are restricted to [A-Za-z0-9_.-] so they can be embedded verbatim
// in btree keys, stored JSON, and JWT payloads without any escaping.
static bool username_valid(const char *username)
{
  size_t len = strlen(username);
  if (len < MIN_USERNAME_LEN || len >= MAX_USERNAME_LEN)
  {
    return false;
  }
  for (size_t i = 0; i < len; i++)
  {
    char c = username[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok)
    {
      return false;
    }
  }
  return true;
}

static int hash_password(const char *password, const uint8_t *salt,
                         size_t salt_len, int iterations,
                         uint8_t out[PW_HASH_SIZE])
{
  uint8_t *derived = NULL;
  if (pbkdf2_derive_key(password, strlen(password), salt, salt_len,
                        iterations, PW_HASH_SIZE, &derived) != 0)
  {
    return -1;
  }
  memcpy(out, derived, PW_HASH_SIZE);
  secure_free(derived, PW_HASH_SIZE);
  return 0;
}

// Render a user record into its stored JSON form. The username needs no
// escaping (see username_valid). Returns length or -1 on overflow.
static int build_user_json(char *dst, size_t dst_size, const user_record_t *user)
{
  char salt_hex[PW_SALT_SIZE * 2 + 1];
  char hash_hex[PW_HASH_SIZE * 2 + 1];
  hex_encode(user->salt, PW_SALT_SIZE, salt_hex);
  hex_encode(user->hash, PW_HASH_SIZE, hash_hex);

  int n = snprintf(dst, dst_size,
                   "{\"id\":%llu,\"username\":\"%s\",\"salt\":\"%s\","
                   "\"hash\":\"%s\",\"iter\":%llu}",
                   (unsigned long long)user->id, user->username, salt_hex,
                   hash_hex, (unsigned long long)user->iterations);
  if (n < 0 || (size_t)n >= dst_size)
  {
    return -1;
  }
  return n;
}

// Fetch and decode "user:<username>". Returns false when the user does not
// exist or the stored record is unreadable.
static bool load_user(const char *username, user_record_t *user)
{
  char key[MAX_KEY_SIZE];
  int key_len = snprintf(key, sizeof(key), USER_KEY_PREFIX "%s", username);
  if (key_len < 0 || (size_t)key_len >= sizeof(key))
  {
    return false;
  }

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return false;
  }

  char value[MAX_VALUE_SIZE + 1];
  size_t value_len = MAX_VALUE_SIZE;
  int rc = db_search(txn, key, (size_t)key_len, value, &value_len);
  commit_transaction(txn);
  free(txn);

  if (rc != 0)
  {
    return false;
  }
  if (value_len > MAX_VALUE_SIZE)
  {
    value_len = MAX_VALUE_SIZE;
  }

  char salt_hex[PW_SALT_SIZE * 2 + 1];
  char hash_hex[PW_HASH_SIZE * 2 + 1];
  if (!json_get_uint64(value, value_len, "id", &user->id) ||
      !json_get_string(value, value_len, "username",
                       user->username, sizeof(user->username)) ||
      !json_get_string(value, value_len, "salt", salt_hex, sizeof(salt_hex)) ||
      !json_get_string(value, value_len, "hash", hash_hex, sizeof(hash_hex)) ||
      !json_get_uint64(value, value_len, "iter", &user->iterations))
  {
    return false;
  }
  return hex_decode(salt_hex, user->salt, PW_SALT_SIZE) &&
         hex_decode(hash_hex, user->hash, PW_HASH_SIZE) &&
         user->iterations > 0;
}

// Pull username/password out of a request body. Writes an error response and
// returns false when either is missing/invalid; password must be wiped by the
// caller once used.
static bool parse_credentials(connection_t *conn, http_request_t *request,
                              http_response_t *response,
                              char username[MAX_USERNAME_LEN],
                              char password[MAX_PASSWORD_LEN + 1])
{
  if (!request->body || request->body_length == 0)
  {
    send_error_json(conn, request, response, HTTP_BAD_REQUEST,
                    "request body required");
    return false;
  }
  if (!json_get_string(request->body, request->body_length, "username",
                       username, MAX_USERNAME_LEN))
  {
    send_error_json(conn, request, response, HTTP_BAD_REQUEST,
                    "missing or invalid 'username'");
    return false;
  }
  if (!json_get_string(request->body, request->body_length, "password",
                       password, MAX_PASSWORD_LEN + 1))
  {
    send_error_json(conn, request, response, HTTP_BAD_REQUEST,
                    "missing or invalid 'password'");
    return false;
  }
  return true;
}

int user_register_handler(connection_t *conn, http_request_t *request,
                          http_response_t *response)
{
  char username[MAX_USERNAME_LEN];
  char password[MAX_PASSWORD_LEN + 1];
  if (!parse_credentials(conn, request, response, username, password))
  {
    return 0;
  }

  if (!username_valid(username))
  {
    explicit_bzero(password, sizeof(password));
    return send_error_json(conn, request, response, HTTP_BAD_REQUEST,
                           "username must be 3-63 characters of [A-Za-z0-9_.-]");
  }
  if (strlen(password) < MIN_PASSWORD_LEN)
  {
    explicit_bzero(password, sizeof(password));
    return send_error_json(conn, request, response, HTTP_BAD_REQUEST,
                           "password must be at least 8 characters");
  }

  user_record_t user = {0};
  snprintf(user.username, sizeof(user.username), "%s", username);
  user.iterations = PBKDF2_ITERATIONS;

  if (secure_random_bytes(user.salt, PW_SALT_SIZE) != 0 ||
      hash_password(password, user.salt, PW_SALT_SIZE,
                    PBKDF2_ITERATIONS, user.hash) != 0)
  {
    explicit_bzero(password, sizeof(password));
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Password hashing failed");
  }
  explicit_bzero(password, sizeof(password));

  user.id = atomic_fetch_add(&g_next_user_id, 1);

  char key[MAX_KEY_SIZE];
  int key_len = snprintf(key, sizeof(key), USER_KEY_PREFIX "%s", username);
  char value[MAX_VALUE_SIZE];
  int value_len = build_user_json(value, sizeof(value), &user);
  if (key_len < 0 || (size_t)key_len >= sizeof(key) || value_len < 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "User record too large");
  }

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Transaction failed");
  }

  // Uniqueness rides on the btree's duplicate-key rejection, so two
  // concurrent registrations of the same name can't both win.
  int rc = db_insert(txn, key, (size_t)key_len, value, (size_t)value_len);
  commit_transaction(txn);
  free(txn);

  if (rc != 0)
  {
    return send_error_json(conn, request, response, HTTP_CONFLICT,
                           "username already exists");
  }

  char token[512];
  if (jwt_create(user.id, user.username, g_token_ttl, token, sizeof(token)) != 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Token creation failed");
  }

  char body[768];
  int n = snprintf(body, sizeof(body),
                   "{\"id\":%llu,\"username\":\"%s\",\"token\":\"%s\","
                   "\"token_type\":\"Bearer\",\"expires_in\":%ld}",
                   (unsigned long long)user.id, user.username, token,
                   g_token_ttl);
  if (n < 0 || (size_t)n >= sizeof(body))
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Response too large");
  }
  return send_json(conn, request, response, HTTP_CREATED, body);
}

int user_login_handler(connection_t *conn, http_request_t *request,
                       http_response_t *response)
{
  char username[MAX_USERNAME_LEN];
  char password[MAX_PASSWORD_LEN + 1];
  if (!parse_credentials(conn, request, response, username, password))
  {
    return 0;
  }

  user_record_t user = {0};
  bool found = load_user(username, &user);

  // Always run the KDF, deriving against a dummy salt when the user does not
  // exist, so response timing can't be used to probe for valid usernames.
  static const uint8_t dummy_salt[PW_SALT_SIZE] = {0};
  const uint8_t *salt = found ? user.salt : dummy_salt;
  int iterations = found ? (int)user.iterations : PBKDF2_ITERATIONS;

  uint8_t computed[PW_HASH_SIZE];
  int rc = hash_password(password, salt, PW_SALT_SIZE, iterations, computed);
  explicit_bzero(password, sizeof(password));
  if (rc != 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Password hashing failed");
  }

  unsigned char diff = found ? 0 : 1;
  for (size_t i = 0; i < PW_HASH_SIZE; i++)
  {
    diff |= (unsigned char)(computed[i] ^ (found ? user.hash[i] : 0));
  }
  if (diff != 0)
  {
    // Same answer for unknown user and wrong password.
    return send_error_json(conn, request, response, HTTP_UNAUTHORIZED,
                           "invalid username or password");
  }

  char token[512];
  if (jwt_create(user.id, user.username, g_token_ttl, token, sizeof(token)) != 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Token creation failed");
  }

  char body[768];
  int n = snprintf(body, sizeof(body),
                   "{\"token\":\"%s\",\"token_type\":\"Bearer\","
                   "\"expires_in\":%ld,"
                   "\"user\":{\"id\":%llu,\"username\":\"%s\"}}",
                   token, g_token_ttl,
                   (unsigned long long)user.id, user.username);
  if (n < 0 || (size_t)n >= sizeof(body))
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR,
                               "Response too large");
  }
  return send_json(conn, request, response, HTTP_OK, body);
}

int user_me_handler(connection_t *conn, http_request_t *request,
                    http_response_t *response)
{
  // jwt_middleware has already verified the token and populated these.
  char body[128];
  snprintf(body, sizeof(body), "{\"id\":%llu,\"username\":\"%s\"}",
           (unsigned long long)conn->auth_user_id, conn->auth_username);
  return send_json(conn, request, response, HTTP_OK, body);
}

// The routes that serve per-user data. /api/auth/register and /api/auth/login
// stay open (they are how a token is obtained); /api/cache stays open as a
// public stats endpoint.
static bool path_requires_jwt(const char *uri)
{
  return strncmp(uri, "/api/todos", 10) == 0 ||
         strcmp(uri, "/api/auth/me") == 0;
}

int jwt_middleware(connection_t *conn, http_request_t *request,
                   http_response_t *response, middleware_ctx_t *next)
{
  // Never let identity leak from a previous request on this connection.
  conn->auth_user_id = 0;
  conn->auth_username[0] = '\0';

  if (!path_requires_jwt(request->uri))
  {
    return middleware_next(conn, request, response, next);
  }

  const char *token = NULL;
  const char *authz = request_header(request, "Authorization");
  if (authz && strncasecmp(authz, "Bearer ", 7) == 0)
  {
    token = authz + 7;
    while (*token == ' ')
    {
      token++;
    }
  }
  if (!token || !*token)
  {
    return send_error_json(conn, request, response, HTTP_UNAUTHORIZED,
                           "missing bearer token");
  }

  uint64_t user_id = 0;
  char username[sizeof(conn->auth_username)];
  jwt_result_t vr = jwt_verify(token, &user_id, username, sizeof(username));
  if (vr == JWT_EXPIRED)
  {
    return send_error_json(conn, request, response, HTTP_UNAUTHORIZED,
                           "token expired");
  }
  if (vr != JWT_OK || user_id == 0)
  {
    return send_error_json(conn, request, response, HTTP_UNAUTHORIZED,
                           "invalid token");
  }

  conn->auth_user_id = user_id;
  snprintf(conn->auth_username, sizeof(conn->auth_username), "%s", username);
  return middleware_next(conn, request, response, next);
}

static int seed_user_id_scan_cb(const char *key, size_t key_length,
                                const char *value, size_t value_length,
                                void *ctx)
{
  uint64_t *max_id = (uint64_t *)ctx;
  size_t prefix_len = sizeof(USER_KEY_PREFIX) - 1;
  if (key_length < prefix_len ||
      memcmp(key, USER_KEY_PREFIX, prefix_len) != 0)
  {
    return 0; // not a user row
  }

  uint64_t id = 0;
  if (json_get_uint64(value, value_length, "id", &id) && id > *max_id)
  {
    *max_id = id;
  }
  return 0;
}

int user_api_init(void)
{
  // The crypto framework backs both PBKDF2 (secure_malloc pool) and the JWT
  // secret (RNG), so it is owned by this module rather than main().
  if (init_crypto_framework() != 0)
  {
    fprintf(stderr, "crypto framework init failed\n");
    return -1;
  }
  if (jwt_init() != 0)
  {
    fprintf(stderr, "JWT secret init failed\n");
    cleanup_crypto_framework();
    return -1;
  }

  long ttl = env_int("JWT_TTL_SECONDS", 3600);
  g_token_ttl = ttl > 0 ? ttl : 3600;

  uint64_t max_id = 0;
  transaction_t *txn = begin_transaction();
  if (txn)
  {
    db_scan(txn, seed_user_id_scan_cb, &max_id);
    commit_transaction(txn);
    free(txn);
  }
  atomic_store(&g_next_user_id, max_id + 1);

  return 0;
}

void user_api_shutdown(void)
{
  cleanup_crypto_framework();
}

void user_api_register_routes(http_server_t *server)
{
  // register/login run the deliberately slow KDF and touch storage, so they
  // are offloaded to the thread pool like the todo routes.
  add_route_offloaded(server, "/api/auth/register", HTTP_POST, user_register_handler);
  add_route_offloaded(server, "/api/auth/login", HTTP_POST, user_login_handler);
  // me only echoes claims already verified by the middleware -- no storage,
  // so it runs inline on the event loop.
  add_route(server, "/api/auth/me", HTTP_GET, user_me_handler);
}
