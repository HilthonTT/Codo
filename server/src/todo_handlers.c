#define _GNU_SOURCE

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "http_protocol.h"
#include "http_types.h"
#include "json_util.h"
#include "lru.h"
#include "route.h"
#include "server.h"
#include "storage.h"
#include "todo_handlers.h"
#include "util.h"

// Todos are stored in the btree keyed by their decimal id (e.g. "42"); the
// value is the canonical JSON object
// {"id":..,"user_id":..,"title":..,"completed":..}. Ids are handed out from a
// process-wide counter that is seeded at startup from the highest id already
// on disk.
//
// Every todo belongs to the user who created it. The jwt_middleware runs
// before these handlers and publishes the verified identity in
// conn->auth_user_id; each handler filters or checks ownership against it,
// answering 404 for another user's todo so ids can't be probed.
static _Atomic uint64_t g_next_todo_id = 1;

// Read-through cache in front of the btree, keyed by the same todo id and
// holding the same canonical JSON. A single-todo GET is the hot path of this
// API and otherwise costs a transaction plus a btree descent through the buffer
// pool; a hit skips all of it. Writes keep the cache in step: create and update
// store the new JSON, delete drops the entry.
//
// Only single-todo reads are cached. The collection GET is a full scan whose
// result any write would invalidate, so caching it would trade a scan for a
// near-permanent miss.
static lru_cache_t *g_todo_cache = NULL;

static const char TODO_COLLECTION[] = "/api/todos";
static const char TODO_ITEM_PREFIX[] = "/api/todos/";

// Todo rows are keyed by a bare decimal id. Other record types share the same
// btree under prefixed keys (e.g. "user:<name>"), so any scan over todos must
// skip keys that are not purely numeric.
static bool key_is_todo(const char *key, size_t key_length)
{
  if (key_length == 0)
  {
    return false;
  }
  for (size_t i = 0; i < key_length; i++)
  {
    if (key[i] < '0' || key[i] > '9')
    {
      return false;
    }
  }
  return true;
}

// Render a complete todo object into dst. Returns length or -1 on overflow.
static int build_todo_json(char *dst, size_t dst_size, uint64_t id,
                           uint64_t user_id, const char *title,
                           size_t title_len, bool completed)
{
  char escaped[MAX_VALUE_SIZE];
  if (json_escape(escaped, sizeof(escaped), title, title_len) < 0)
  {
    return -1;
  }

  int n = snprintf(dst, dst_size,
                   "{\"id\":%llu,\"user_id\":%llu,\"title\":\"%s\",\"completed\":%s}",
                   (unsigned long long)id, (unsigned long long)user_id,
                   escaped, completed ? "true" : "false");
  if (n < 0 || (size_t)n >= dst_size)
  {
    return -1;
  }
  return n;
}

// True when the stored todo JSON belongs to the given user. A row without a
// user_id (created before accounts existed) belongs to nobody.
static bool todo_owned_by(const char *value, size_t value_len, uint64_t user_id)
{
  uint64_t owner = 0;
  return json_get_uint64(value, value_len, "user_id", &owner) &&
         owner == user_id;
}

// Pull a numeric id out of "/api/todos/{id}". Returns false for missing,
// empty, or non-numeric ids.
static bool extract_id(const char *uri, char *out, size_t out_size)
{
  size_t prefix_len = strlen(TODO_ITEM_PREFIX);
  if (strncmp(uri, TODO_ITEM_PREFIX, prefix_len) != 0)
  {
    return false;
  }

  const char *id = uri + prefix_len;
  size_t out_len = 0;
  while (id[out_len] && id[out_len] != '/' && id[out_len] != '?')
  {
    char c = id[out_len];
    if (c < '0' || c > '9')
    {
      return false; // ids are decimal only
    }
    if (out_len + 1 >= out_size)
    {
      return false;
    }
    out[out_len] = c;
    out_len++;
  }

  if (out_len == 0)
  {
    return false;
  }
  out[out_len] = '\0';
  return true;
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

// Parsed GET /api/todos query parameters. Any subset may be present; an absent
// field leaves filtering/pagination on that axis disabled.
typedef struct
{
  bool have_completed;
  bool completed_want;
  bool have_q;
  char q[256]; // case-insensitive substring matched against the title
  long offset; // items to skip (>= 0)
  long limit;  // max items to return; < 0 means unlimited
} todo_filter_t;

typedef struct
{
  char *buf;
  size_t len;
  size_t cap;
  int count;          // items appended to buf (after pagination)
  long matched;       // items passing the filter (before pagination)
  uint64_t user_id;   // only this user's todos are visible
  const todo_filter_t *filter;
  bool error;
} list_ctx_t;

static int list_append(list_ctx_t *ctx, const char *data, size_t n)
{
  if (ctx->len + n + 1 > ctx->cap)
  {
    size_t new_cap = ctx->cap ? ctx->cap * 2 : 4096;
    while (new_cap < ctx->len + n + 1)
    {
      new_cap *= 2;
    }
    char *grown = realloc(ctx->buf, new_cap);
    if (!grown)
    {
      ctx->error = true;
      return -1;
    }
    ctx->buf = grown;
    ctx->cap = new_cap;
  }
  memcpy(ctx->buf + ctx->len, data, n);
  ctx->len += n;
  ctx->buf[ctx->len] = '\0';
  return 0;
}

static int list_scan_cb(const char *key, size_t key_length,
                        const char *value, size_t value_length, void *ctx)
{
  list_ctx_t *list = (list_ctx_t *)ctx;
  const todo_filter_t *f = list->filter;

  // Skip rows that are not todos (user records share the btree) and todos
  // that belong to somebody else.
  if (!key_is_todo(key, key_length) ||
      !todo_owned_by(value, value_length, list->user_id))
  {
    return 0;
  }

  // Apply the filters against the stored JSON before this row counts as a match.
  if (f->have_completed)
  {
    bool completed = false;
    json_get_bool(value, value_length, "completed", &completed);
    if (completed != f->completed_want)
    {
      return 0;
    }
  }
  if (f->have_q)
  {
    char title[MAX_VALUE_SIZE];
    if (!json_get_string(value, value_length, "title", title, sizeof(title)) ||
        !strcasestr(title, f->q))
    {
      return 0;
    }
  }

  // Matched. Its 0-based position within the matched set drives pagination; the
  // running total keeps climbing past the page so X-Total-Count is exact.
  long index = list->matched;
  list->matched++;

  if (index < f->offset)
  {
    return 0; // before the requested page
  }
  if (f->limit >= 0 && list->count >= f->limit)
  {
    return 0; // page already full -- keep scanning only to finish the count
  }

  if (list->count > 0 && list_append(list, ",", 1) != 0)
  {
    return 1; // stop on allocation failure
  }
  if (list_append(list, value, value_length) != 0)
  {
    return 1;
  }
  list->count++;
  return 0;
}

// Parse the GET /api/todos query string into a filter. Unknown or malformed
// parameters are ignored, so a bad ?limit= just falls back to "no limit".
static void parse_todo_filter(const char *query, todo_filter_t *f)
{
  f->have_completed = false;
  f->completed_want = false;
  f->have_q = false;
  f->q[0] = '\0';
  f->offset = 0;
  f->limit = -1;

  if (!query || !*query)
  {
    return;
  }

  http_header_t params[MAX_HEADERS];
  int count = 0;
  parse_query_string(query, params, &count);

  for (int i = 0; i < count; i++)
  {
    const char *name = params[i].name;
    const char *val = params[i].value;

    if (strcmp(name, "completed") == 0)
    {
      if (strcmp(val, "true") == 0)
      {
        f->have_completed = true;
        f->completed_want = true;
      }
      else if (strcmp(val, "false") == 0)
      {
        f->have_completed = true;
        f->completed_want = false;
      }
    }
    else if (strcmp(name, "q") == 0)
    {
      char *decoded = url_decode(val);
      if (decoded)
      {
        strncpy(f->q, decoded, sizeof(f->q) - 1);
        f->q[sizeof(f->q) - 1] = '\0';
        f->have_q = f->q[0] != '\0';
        free(decoded);
      }
    }
    else if (strcmp(name, "limit") == 0)
    {
      char *end = NULL;
      long n = strtol(val, &end, 10);
      if (end != val && n >= 0)
      {
        f->limit = n;
      }
    }
    else if (strcmp(name, "offset") == 0)
    {
      char *end = NULL;
      long n = strtol(val, &end, 10);
      if (end != val && n >= 0)
      {
        f->offset = n;
      }
    }
  }
}

int todo_list_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  todo_filter_t filter;
  parse_todo_filter(request->query_string, &filter);

  list_ctx_t list = {0};
  list.filter = &filter;
  list.user_id = conn->auth_user_id;
  if (list_append(&list, "[", 1) != 0)
  {
    free(list.buf);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    free(list.buf);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Transaction failed");
  }

  db_scan(txn, list_scan_cb, &list);
  commit_transaction(txn);
  free(txn);

  if (list.error || list_append(&list, "]", 1) != 0)
  {
    free(list.buf);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Out of memory");
  }

  // The body stays a bare JSON array (paginated), so the demo UI and existing
  // clients keep working; the pre-pagination match count rides in a header.
  response->status = HTTP_OK;
  snprintf(response->version, sizeof(response->version), "HTTP/1.1");
  if (response->body)
  {
    free(response->body);
  }
  response->body = list.buf; // hand off ownership; send frees it via cleanup
  response->body_length = list.len;
  response->keep_alive = request->keep_alive;

  int h = 0;
  snprintf(response->headers[h].name, sizeof(response->headers[h].name), "Content-Type");
  snprintf(response->headers[h].value, sizeof(response->headers[h].value), "application/json");
  h++;
  snprintf(response->headers[h].name, sizeof(response->headers[h].name), "X-Total-Count");
  snprintf(response->headers[h].value, sizeof(response->headers[h].value), "%ld", list.matched);
  h++;
  response->header_count = h;

  return send_http_response(conn, response);
}

int todo_create_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  if (!request->body || request->body_length == 0)
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Request body required");
  }

  char title[MAX_VALUE_SIZE];
  if (!json_get_string(request->body, request->body_length, "title", title, sizeof(title)))
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Missing or invalid 'title'");
  }

  bool completed = false;
  json_get_bool(request->body, request->body_length, "completed", &completed);

  uint64_t id = atomic_fetch_add(&g_next_todo_id, 1);

  char key[32];
  int key_len = snprintf(key, sizeof(key), "%llu", (unsigned long long)id);

  char value[MAX_VALUE_SIZE];
  int value_len = build_todo_json(value, sizeof(value), id, conn->auth_user_id,
                                  title, strlen(title), completed);
  if (value_len < 0)
  {
    return send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "Todo too large");
  }

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Transaction failed");
  }

  int rc = db_insert(txn, key, (size_t)key_len, value, (size_t)value_len);
  commit_transaction(txn);
  free(txn);

  if (rc != 0)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Insert failed");
  }

  // A just-created todo is very likely to be read back, so seed it rather than
  // waiting for the first GET to miss.
  lru_put(g_todo_cache, key, value, (size_t)value_len);

  return send_json(conn, request, response, HTTP_CREATED, value);
}

int todo_get_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char key[32];
  if (!extract_id(request->uri, key, sizeof(key)))
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Invalid todo id");
  }

  char value[MAX_VALUE_SIZE + 1];
  size_t value_len = MAX_VALUE_SIZE;

  if (lru_get(g_todo_cache, key, value, &value_len))
  {
    if (value_len > MAX_VALUE_SIZE)
    {
      value_len = MAX_VALUE_SIZE;
    }
    value[value_len] = '\0';
    // The cache is shared across users; a hit on someone else's todo is
    // answered exactly like a missing row so ids can't be probed.
    if (!todo_owned_by(value, value_len, conn->auth_user_id))
    {
      return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
    }
    return send_json(conn, request, response, HTTP_OK, value);
  }

  // Miss. Snapshot the cache generation *before* going to storage: if a write
  // for this key commits while we are reading, the fill below is dropped rather
  // than overwriting the writer's fresher value with what we are about to read.
  uint64_t generation = lru_generation(g_todo_cache);

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Transaction failed");
  }

  value_len = MAX_VALUE_SIZE;
  int rc = db_search(txn, key, strlen(key), value, &value_len);
  commit_transaction(txn);
  free(txn);

  if (rc != 0)
  {
    return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
  }

  if (value_len > MAX_VALUE_SIZE)
  {
    value_len = MAX_VALUE_SIZE;
  }
  value[value_len] = '\0';

  // Fill the cache regardless of who asked -- the row is valid -- but only
  // reveal it to its owner.
  lru_fill(g_todo_cache, key, value, value_len, generation);

  if (!todo_owned_by(value, value_len, conn->auth_user_id))
  {
    return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
  }
  return send_json(conn, request, response, HTTP_OK, value);
}

int todo_update_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char key[32];
  if (!extract_id(request->uri, key, sizeof(key)))
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Invalid todo id");
  }

  if (!request->body || request->body_length == 0)
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Request body required");
  }

  char title[MAX_VALUE_SIZE];
  if (!json_get_string(request->body, request->body_length, "title", title, sizeof(title)))
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Missing or invalid 'title'");
  }

  bool completed = false;
  json_get_bool(request->body, request->body_length, "completed", &completed);

  size_t key_len = strlen(key);
  errno = 0;
  char *endptr = NULL;
  uint64_t id = strtoull(key, &endptr, 10);
  if (errno != 0 || !endptr || *endptr != '\0' || endptr == key)
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Invalid todo id");
  }

  char value[MAX_VALUE_SIZE];
  int value_len = build_todo_json(value, sizeof(value), id, conn->auth_user_id,
                                  title, strlen(title), completed);
  if (value_len < 0)
  {
    return send_error_response(conn, HTTP_PAYLOAD_TOO_LARGE, "Todo too large");
  }

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Transaction failed");
  }

  // db_update only handles same-length values, so replace via delete+insert.
  char existing[MAX_VALUE_SIZE + 1];
  size_t existing_len = MAX_VALUE_SIZE;
  if (db_search(txn, key, key_len, existing, &existing_len) != 0)
  {
    commit_transaction(txn);
    free(txn);
    return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
  }
  if (existing_len > MAX_VALUE_SIZE)
  {
    existing_len = MAX_VALUE_SIZE;
  }
  if (!todo_owned_by(existing, existing_len, conn->auth_user_id))
  {
    commit_transaction(txn);
    free(txn);
    return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
  }

  int rc = db_delete(txn, key, key_len);
  if (rc == 0)
  {
    rc = db_insert(txn, key, key_len, value, (size_t)value_len);
  }

  if (rc != 0)
  {
    // Either the delete or the insert failed; roll back so a committed delete
    // can never leave the todo lost while we return an error.
    abort_transaction(txn);
    free(txn);
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Update failed");
  }

  commit_transaction(txn);
  free(txn);

  // The write is durable, so the cache can now be moved to the new value. (The
  // abort path above leaves storage untouched, and with it the cached entry.)
  lru_put(g_todo_cache, key, value, (size_t)value_len);

  return send_json(conn, request, response, HTTP_OK, value);
}

int todo_delete_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  char key[32];
  if (!extract_id(request->uri, key, sizeof(key)))
  {
    return send_error_response(conn, HTTP_BAD_REQUEST, "Invalid todo id");
  }

  size_t key_len = strlen(key);

  transaction_t *txn = begin_transaction();
  if (!txn)
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Transaction failed");
  }

  char existing[MAX_VALUE_SIZE + 1];
  size_t existing_len = MAX_VALUE_SIZE;
  if (db_search(txn, key, key_len, existing, &existing_len) != 0)
  {
    commit_transaction(txn);
    free(txn);
    return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
  }
  if (existing_len > MAX_VALUE_SIZE)
  {
    existing_len = MAX_VALUE_SIZE;
  }
  if (!todo_owned_by(existing, existing_len, conn->auth_user_id))
  {
    commit_transaction(txn);
    free(txn);
    return send_error_response(conn, HTTP_NOT_FOUND, "Todo not found");
  }

  db_delete(txn, key, key_len);
  commit_transaction(txn);
  free(txn);

  lru_invalidate(g_todo_cache, key);

  return send_json(conn, request, response, HTTP_NO_CONTENT, "");
}

// GET /api/cache -- hit/miss counters for the todo cache. Runs inline on the
// event loop: it reads atomics and never touches storage.
int todo_cache_stats_handler(connection_t *conn, http_request_t *request, http_response_t *response)
{
  uint64_t hits = 0, misses = 0, evictions = 0, size = 0;
  lru_stats(g_todo_cache, &hits, &misses, &evictions, &size);

  uint64_t lookups = hits + misses;
  double hit_rate = lookups ? (double)hits / (double)lookups : 0.0;

  char json[256];
  int n = snprintf(json, sizeof(json),
                   "{\"entries\":%llu,\"capacity\":%d,\"hits\":%llu,\"misses\":%llu,"
                   "\"evictions\":%llu,\"hit_rate\":%.4f}",
                   (unsigned long long)size,
                   g_todo_cache ? g_todo_cache->list->capacity : 0,
                   (unsigned long long)hits, (unsigned long long)misses,
                   (unsigned long long)evictions, hit_rate);
  if (n < 0 || (size_t)n >= sizeof(json))
  {
    return send_error_response(conn, HTTP_INTERNAL_SERVER_ERROR, "Stats too large");
  }

  return send_json(conn, request, response, HTTP_OK, json);
}

static int seed_id_scan_cb(const char *key, size_t key_length,
                           const char *value, size_t value_length, void *ctx)
{
  (void)value;
  (void)value_length;
  uint64_t *max_id = (uint64_t *)ctx;

  if (!key_is_todo(key, key_length))
  {
    return 0; // user records and other namespaces don't consume todo ids
  }

  char buf[32];
  size_t n = key_length < sizeof(buf) - 1 ? key_length : sizeof(buf) - 1;
  memcpy(buf, key, n);
  buf[n] = '\0';

  uint64_t id = strtoull(buf, NULL, 10);
  if (id > *max_id)
  {
    *max_id = id;
  }
  return 0;
}

void todo_api_init(void)
{
  uint64_t max_id = 0;

  transaction_t *txn = begin_transaction();
  if (txn)
  {
    db_scan(txn, seed_id_scan_cb, &max_id);
    commit_transaction(txn);
    free(txn);
  }

  atomic_store(&g_next_todo_id, max_id + 1);

  // A NULL cache is not fatal: every lru_* call tolerates it and the handlers
  // fall back to going straight to storage.
  g_todo_cache = lru_cache_create(env_int("TODO_CACHE_CAPACITY", 1024));
  if (!g_todo_cache)
  {
    fprintf(stderr, "todo cache disabled (allocation failed)\n");
  }
}

void todo_api_shutdown(void)
{
  lru_cache_destroy(g_todo_cache);
  g_todo_cache = NULL;
}

void todo_api_register_routes(http_server_t *server)
{
  // Every todo handler goes through the btree storage engine (transactions,
  // WAL fsync, page reads/writes), so mark them for thread-pool offload to
  // keep the epoll workers responsive under storage load. A cached GET returns
  // without any of that, but the route still has to be able to miss.
  add_route_offloaded(server, TODO_COLLECTION, HTTP_GET, todo_list_handler);
  add_route_offloaded(server, TODO_COLLECTION, HTTP_POST, todo_create_handler);
  add_route_offloaded(server, "/api/todos/*", HTTP_GET, todo_get_handler);
  add_route_offloaded(server, "/api/todos/*", HTTP_PUT, todo_update_handler);
  add_route_offloaded(server, "/api/todos/*", HTTP_DELETE, todo_delete_handler);

  add_route(server, "/api/cache", HTTP_GET, todo_cache_stats_handler);
}
