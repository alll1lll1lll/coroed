#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "http/http.h"

static char response[HTTP_MAX_RESPONSE_SIZE + 1];

/**
 * Обработать запрос и вернуть ответ строкой.
 */
static const char* handle(const char* request, bool* keep_alive) {
  const ssize_t len = http_request_length(request, strlen(request));
  assert(len == (ssize_t)strlen(request));
  const size_t n = http_handle(request, (size_t)len, response, keep_alive);
  response[n] = '\0';
  return response;
}

static bool starts_with(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool ends_with(const char* s, const char* suffix) {
  const size_t s_len = strlen(s);
  const size_t suffix_len = strlen(suffix);
  return s_len >= suffix_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

void test_http() {
  bool keep_alive = false;
  const char* r = NULL;

  // Неполный запрос
  assert(http_request_length("GET / HTTP/1.1\r\nHost: x\r\n", 25) == -1);

  // Основной маршрут
  r = handle("GET /hello/vasya HTTP/1.1\r\nHost: localhost\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 200 OK\r\n"));
  assert(strstr(r, "Content-Type: application/json\r\n") != NULL);
  assert(strstr(r, "Content-Length: 19\r\n") != NULL);
  assert(ends_with(r, "\r\n\r\n{\"message\":\"vasya\"}"));
  assert(keep_alive);

  // Query-строка отбрасывается, percent-encoding раскодируется
  r = handle("GET /hello/John%20Doe?x=1 HTTP/1.1\r\n\r\n", &keep_alive);
  assert(ends_with(r, "{\"message\":\"John Doe\"}"));

  // Экранирование для JSON
  r = handle("GET /hello/a%22b%5Cc HTTP/1.1\r\n\r\n", &keep_alive);
  assert(ends_with(r, "{\"message\":\"a\\\"b\\\\c\"}"));

  // Connection: close и HTTP/1.0
  handle("GET /hello/x HTTP/1.1\r\nconnection: Close\r\n\r\n", &keep_alive);
  assert(!keep_alive);
  handle("GET /hello/x HTTP/1.0\r\n\r\n", &keep_alive);
  assert(!keep_alive);
  handle("GET /hello/x HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &keep_alive);
  assert(keep_alive);

  // Ошибки
  r = handle("GET /hello/ HTTP/1.1\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 404 "));
  r = handle("GET /hello/a/b HTTP/1.1\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 404 "));
  r = handle("GET /bye/x HTTP/1.1\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 404 "));
  r = handle("POST /hello/x HTTP/1.1\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 405 "));
  r = handle("GET /hello/%zz HTTP/1.1\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 400 "));
  r = handle("garbage\r\n\r\n", &keep_alive);
  assert(starts_with(r, "HTTP/1.1 400 "));
  assert(!keep_alive);
}
