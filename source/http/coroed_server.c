// HTTP-сервер на корутинах Coroed: одно соединение -- один файбер.
//
// Запуск: ./build/bin/coroed_server [port]   (по умолчанию 8080)

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coroed/api/io.h"
#include "coroed/api/task.h"
#include "http/http.h"

enum {
  LISTEN_BACKLOG = 4096,
};

static int listen_fd = -1;

/**
 * Обслужить одно соединение: читать запросы, пока клиент
 * держит соединение открытым (keep-alive).
 */
TASK_DEFINE(http_connection, void, argument) {
  const int fd = (int)(intptr_t)argument;

  char request[HTTP_MAX_REQUEST_SIZE];
  char response[HTTP_MAX_RESPONSE_SIZE];
  size_t buffered = 0;

  for (;;) {
    // Дочитать заголовки очередного запроса. Буфер может уже
    // содержать следующий запрос, если клиент прислал
    // несколько подряд (pipelining).
    ssize_t request_len = http_request_length(request, buffered);
    while (request_len < 0) {
      if (buffered == sizeof(request)) {
        const size_t len = http_too_large(response);
        IO_WRITE(fd, response, len);
        close(fd);
        return;
      }
      const ssize_t n = IO_READ(fd, request + buffered, sizeof(request) - buffered);
      if (n <= 0) {
        close(fd);  // Клиент закрыл соединение или ошибка
        return;
      }
      buffered += (size_t)n;
      request_len = http_request_length(request, buffered);
    }

    bool keep_alive = false;
    const size_t response_len = http_handle(request, (size_t)request_len, response, &keep_alive);
    if (IO_WRITE(fd, response, response_len) < 0 || !keep_alive) {
      break;
    }

    // Сдвинуть остаток (начало следующего запроса) в начало буфера
    buffered -= (size_t)request_len;
    memmove(request, request + request_len, buffered);
  }

  close(fd);
}

/**
 * Принимать соединения и запускать на каждое свой файбер.
 */
TASK_DEFINE(http_listener, void, ignored) {
  for (;;) {
    const int fd = IO_ACCEPT(listen_fd);
    if (fd < 0) {
      // Например, EMFILE: закончились дескрипторы.
      // Дадим поработать остальным и попробуем снова.
      YIELD;
      continue;
    }

    task_t child = task_try_submit(__self, &http_connection, (void*)(intptr_t)fd);
    if (child.task == NULL) {
      // Все слоты задач заняты: соединение обслужить нечем.
      close(fd);
    }
  }
}

static int listen_on(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
  const uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8080;

  // Подстраховка: запись в закрытый сокет не должна убивать
  // процесс (io_write и так использует MSG_NOSIGNAL).
  signal(SIGPIPE, SIG_IGN);

  listen_fd = listen_on(port);
  printf("coroed_server: listening on port %u\n", port);
  fflush(stdout);

  tasks_init();
  tasks_submit(&http_listener, NULL);
  tasks_start();
  tasks_wait();  // Слушатель работает бесконечно
  tasks_destroy();
  return 0;
}
