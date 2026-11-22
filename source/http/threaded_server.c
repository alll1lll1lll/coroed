// HTTP-сервер на потоках ОС: одно соединение -- один поток.
//
// Эталон для сравнения с сервером на корутинах: тот же разбор
// запросов (http/http.c), но обычные блокирующие read/write.
// С keep-alive поток обслуживает все запросы своего соединения.
//
// Запуск: ./build/bin/threaded_server [port]   (по умолчанию 8081)

#define _GNU_SOURCE  // accept4

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http/http.h"

enum {
  LISTEN_BACKLOG = 4096,
};

/**
 * Записать все `len` байт. Возвращает false при ошибке.
 */
static bool write_all(int fd, const char* data, size_t len) {
  size_t written = 0;
  while (written < len) {
    const ssize_t n = send(fd, data + written, len - written, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    written += (size_t)n;
  }
  return true;
}

/**
 * Тело потока: обслужить одно соединение. Структура та же,
 * что у файбера `http_connection` в coroed_server.c.
 */
static void* http_connection(void* argument) {
  const int fd = (int)(intptr_t)argument;

  char request[HTTP_MAX_REQUEST_SIZE];
  char response[HTTP_MAX_RESPONSE_SIZE];
  size_t buffered = 0;

  for (;;) {
    ssize_t request_len = http_request_length(request, buffered);
    while (request_len < 0) {
      if (buffered == sizeof(request)) {
        const size_t len = http_too_large(response);
        write_all(fd, response, len);
        close(fd);
        return NULL;
      }
      // Блокирующее чтение: поток спит в ядре, пока нет данных
      const ssize_t n = read(fd, request + buffered, sizeof(request) - buffered);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        close(fd);
        return NULL;
      }
      buffered += (size_t)n;
      request_len = http_request_length(request, buffered);
    }

    bool keep_alive = false;
    const size_t response_len = http_handle(request, (size_t)request_len, response, &keep_alive);
    if (!write_all(fd, response, response_len) || !keep_alive) {
      break;
    }

    buffered -= (size_t)request_len;
    memmove(request, request + request_len, buffered);
  }

  close(fd);
  return NULL;
}

static int listen_on(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    exit(1);
  }

  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    perror("bind");
    exit(1);
  }
  if (listen(fd, LISTEN_BACKLOG) != 0) {
    perror("listen");
    exit(1);
  }
  return fd;
}

int main(int argc, char** argv) {
  const uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8081;

  signal(SIGPIPE, SIG_IGN);

  const int listen_fd = listen_on(port);
  printf("threaded_server: listening on port %u\n", port);
  fflush(stdout);

  // Потоки создаются отсоединенными: join не нужен, ресурсы
  // освобождаются при завершении потока.
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  for (;;) {
    const int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      perror("accept");
      usleep(1000);  // Например, EMFILE: подождать и попробовать снова
      continue;
    }

    pthread_t thread;
    if (pthread_create(&thread, &attr, http_connection, (void*)(intptr_t)fd) != 0) {
      close(fd);  // Не удалось создать поток (лимит потоков или памяти)
    }
  }
}
