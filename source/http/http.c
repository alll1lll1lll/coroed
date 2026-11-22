#define _GNU_SOURCE  // memmem, strncasecmp

#include "http.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char HELLO_PREFIX[] = "/hello/";

ssize_t http_request_length(const char* buf, size_t len) {
  const char* end = memmem(buf, len, "\r\n\r\n", 4);
  if (end == NULL) {
    return -1;
  }
  return (end - buf) + 4;
}

/**
 * Значение шестнадцатеричной цифры или -1.
 */
static int hex_value(char c) {
  if ('0' <= c && c <= '9') {
    return c - '0';
  }
  if ('a' <= c && c <= 'f') {
    return c - 'a' + 10;
  }
  if ('A' <= c && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/**
 * Раскодировать percent-encoding (`%20` -> пробел) из
 * `src[0..len)` в `dst`. Возвращает длину результата
 * или -1, если кодировка некорректна.
 */
static ssize_t url_decode(const char* src, size_t len, char* dst) {
  size_t out = 0;
  for (size_t i = 0; i < len; ++i) {
    if (src[i] != '%') {
      dst[out++] = src[i];
      continue;
    }
    if (i + 2 >= len) {
      return -1;  // После '%' должно быть две цифры
    }
    const int high = hex_value(src[i + 1]);
    const int low = hex_value(src[i + 2]);
    if (high < 0 || low < 0) {
      return -1;
    }
    dst[out++] = (char)(high * 16 + low);
    i += 2;
  }
  return (ssize_t)out;
}

/**
 * Записать `src[0..len)` в `dst` как содержимое строки JSON:
 * экранировать кавычки, обратную косую черту и управляющие
 * символы. Возвращает длину результата (не более 6 * len).
 */
static size_t json_escape(const char* src, size_t len, char* dst) {
  static const char HEX[] = "0123456789abcdef";
  size_t out = 0;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = (unsigned char)src[i];
    if (c == '"' || c == '\\') {
      dst[out++] = '\\';
      dst[out++] = (char)c;
    } else if (c < 0x20) {
      dst[out++] = '\\';
      dst[out++] = 'u';
      dst[out++] = '0';
      dst[out++] = '0';
      dst[out++] = HEX[c >> 4];
      dst[out++] = HEX[c & 0xF];
    } else {
      dst[out++] = (char)c;
    }
  }
  return out;
}

/**
 * Есть ли среди заголовков `Connection: <value>`.
 * `headers` -- текст после первой строки запроса.
 */
static bool has_connection(const char* headers, const char* end, const char* value) {
  static const char NAME[] = "Connection:";
  const size_t name_len = sizeof(NAME) - 1;
  const size_t value_len = strlen(value);

  const char* line = headers;
  while (line < end) {
    const char* eol = memmem(line, end - line, "\r\n", 2);
    if (eol == NULL) {
      eol = end;
    }
    if ((size_t)(eol - line) >= name_len && strncasecmp(line, NAME, name_len) == 0) {
      const char* v = line + name_len;
      while (v < eol && (*v == ' ' || *v == '\t')) {
        ++v;
      }
      if ((size_t)(eol - v) >= value_len && strncasecmp(v, value, value_len) == 0) {
        return true;
      }
    }
    line = eol + 2;
  }
  return false;
}

static size_t respond(
    char* response, int status, const char* reason, const char* body, size_t body_len,
    bool keep_alive
) {
  const int head = snprintf(
      response, HTTP_MAX_RESPONSE_SIZE,
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %zu\r\n"
      "Connection: %s\r\n"
      "\r\n",
      status, reason, body_len, keep_alive ? "keep-alive" : "close"
  );
  memcpy(response + head, body, body_len);
  return (size_t)head + body_len;
}

static size_t respond_error(char* response, int status, const char* reason, bool keep_alive) {
  char body[64];
  const int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", reason);
  return respond(response, status, reason, body, (size_t)len, keep_alive);
}

size_t http_too_large(char* response) {
  return respond_error(response, 431, "Request Header Fields Too Large", false);
}

size_t http_handle(const char* request, size_t request_len, char* response, bool* keep_alive) {
  const char* end = request + request_len;

  // Первая строка: METHOD SP TARGET SP VERSION CRLF
  const char* line_end = memmem(request, request_len, "\r\n", 2);
  const char* method_end = line_end ? memchr(request, ' ', line_end - request) : NULL;
  const char* target = method_end ? method_end + 1 : NULL;
  const char* target_end = target ? memchr(target, ' ', line_end - target) : NULL;
  const char* version = target_end ? target_end + 1 : NULL;

  if (version == NULL || target == target_end || line_end - version != 8 ||
      strncmp(version, "HTTP/1.", 7) != 0) {
    *keep_alive = false;
    return respond_error(response, 400, "Bad Request", false);
  }

  // Keep-alive: в HTTP/1.1 по умолчанию включен, в HTTP/1.0 -- нет.
  const char* headers = line_end + 2;
  if (version[7] == '1') {
    *keep_alive = !has_connection(headers, end, "close");
  } else {
    *keep_alive = has_connection(headers, end, "keep-alive");
  }

  const size_t method_len = method_end - request;
  if (method_len != 3 || memcmp(request, "GET", 3) != 0) {
    return respond_error(response, 405, "Method Not Allowed", *keep_alive);
  }

  // Путь без query-строки
  const char* path_end = memchr(target, '?', target_end - target);
  if (path_end == NULL) {
    path_end = target_end;
  }

  const size_t prefix_len = sizeof(HELLO_PREFIX) - 1;
  const char* name = target + prefix_len;
  if ((size_t)(path_end - target) <= prefix_len ||
      memcmp(target, HELLO_PREFIX, prefix_len) != 0 ||
      memchr(name, '/', path_end - name) != NULL) {
    return respond_error(response, 404, "Not Found", *keep_alive);
  }

  char decoded[HTTP_MAX_REQUEST_SIZE];
  const ssize_t decoded_len = url_decode(name, path_end - name, decoded);
  if (decoded_len < 0) {
    return respond_error(response, 400, "Bad Request", *keep_alive);
  }

  // Тело: {"message":"<name>"}
  static const char BODY_PREFIX[] = "{\"message\":\"";
  static const char BODY_SUFFIX[] = "\"}";
  char body[sizeof(BODY_PREFIX) + 6 * HTTP_MAX_REQUEST_SIZE + sizeof(BODY_SUFFIX)];
  size_t body_len = 0;
  memcpy(body, BODY_PREFIX, sizeof(BODY_PREFIX) - 1);
  body_len += sizeof(BODY_PREFIX) - 1;
  body_len += json_escape(decoded, (size_t)decoded_len, body + body_len);
  memcpy(body + body_len, BODY_SUFFIX, sizeof(BODY_SUFFIX) - 1);
  body_len += sizeof(BODY_SUFFIX) - 1;

  return respond(response, 200, "OK", body, body_len, *keep_alive);
}
