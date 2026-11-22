#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coroed/api/io.h"
#include "coroed/api/task.h"

// ---------------------------------------------------------------
// Тест 1: передача 8 МБ через пару сокетов. Объем намного больше
// буфера сокета, поэтому писатель упирается в EAGAIN и ждет
// EPOLLOUT, а читатель -- в EAGAIN и ждет EPOLLIN.
// ---------------------------------------------------------------

enum {
  TRANSFER_SIZE = 8 * 1024 * 1024,
  TRANSFER_CHUNK = 64 * 1024,
};

static int pair[2];
static atomic_size_t transfer_received = 0;
static atomic_bool transfer_corrupted = false;

static uint8_t transfer_byte(size_t offset) {
  return (uint8_t)(offset * 31 + 7);
}

TASK_DEFINE(transfer_writer, void, ignored) {
  static uint8_t chunk[TRANSFER_CHUNK];
  for (size_t offset = 0; offset < TRANSFER_SIZE; offset += TRANSFER_CHUNK) {
    for (size_t i = 0; i < TRANSFER_CHUNK; ++i) {
      chunk[i] = transfer_byte(offset + i);
    }
    const ssize_t n = IO_WRITE(pair[0], chunk, TRANSFER_CHUNK);
    assert(n == TRANSFER_CHUNK);
  }
  close(pair[0]);
}

TASK_DEFINE(transfer_reader, void, ignored) {
  static uint8_t buf[4096];
  size_t offset = 0;
  for (;;) {
    const ssize_t n = IO_READ(pair[1], buf, sizeof(buf));
    assert(n >= 0);
    if (n == 0) {
      break;  // Писатель закрыл сокет
    }
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] != transfer_byte(offset + (size_t)i)) {
        atomic_store(&transfer_corrupted, true);
      }
    }
    offset += (size_t)n;
  }
  atomic_store(&transfer_received, offset);
  close(pair[1]);
}

TASK_DEFINE(transfer_test, void, ignored) {
  GO(transfer_reader, NULL);
  GO(transfer_writer, NULL);
}

// ---------------------------------------------------------------
// Тест 2: эхо-сервер по TCP. Сервер принимает ECHO_CLIENTS
// соединений и на каждое запускает файбер; клиенты (тоже
// файберы) подключаются, отправляют сообщение и сверяют ответ.
// ---------------------------------------------------------------

enum {
  ECHO_CLIENTS = 200,
  ECHO_MESSAGE_SIZE = 32,
};

static uint16_t echo_port = 0;
static int echo_listen_fd = -1;
static atomic_size_t echo_ok = 0;

static ssize_t read_exactly(struct task* self, int fd, char* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    const ssize_t n = io_read(self, fd, buf + got, len - got);
    if (n <= 0) {
      return n;
    }
    got += (size_t)n;
  }
  return (ssize_t)got;
}

TASK_DEFINE(echo_handler, void, argument) {
  const int fd = (int)(intptr_t)argument;
  char buf[ECHO_MESSAGE_SIZE];
  if (read_exactly(__self, fd, buf, sizeof(buf)) == sizeof(buf)) {
    IO_WRITE(fd, buf, sizeof(buf));
  }
  close(fd);
}

TASK_DEFINE(echo_server, void, ignored) {
  for (size_t i = 0; i < ECHO_CLIENTS; ++i) {
    const int fd = IO_ACCEPT(echo_listen_fd);
    assert(fd >= 0);
    GO(echo_handler, (void*)(intptr_t)fd);
  }
  close(echo_listen_fd);
}

TASK_DEFINE(echo_client, void, argument) {
  const size_t index = (size_t)(intptr_t)argument;

  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  assert(fd >= 0);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(echo_port),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  const int code = IO_CONNECT(fd, (struct sockaddr*)&addr, sizeof(addr));
  assert(code == 0);

  char message[ECHO_MESSAGE_SIZE];
  memset(message, 0, sizeof(message));
  snprintf(message, sizeof(message), "hello from client %zu", index);
  IO_WRITE(fd, message, sizeof(message));

  char reply[ECHO_MESSAGE_SIZE];
  if (read_exactly(__self, fd, reply, sizeof(reply)) == sizeof(reply) &&
      memcmp(message, reply, sizeof(message)) == 0) {
    atomic_fetch_add(&echo_ok, 1);
  }
  close(fd);
}

TASK_DEFINE(echo_test, void, ignored) {
  GO(echo_server, NULL);
  for (size_t i = 0; i < ECHO_CLIENTS; ++i) {
    GO(echo_client, (void*)(intptr_t)i);
  }
}

static void echo_listen() {
  echo_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  assert(echo_listen_fd >= 0);

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = 0,  // Любой свободный порт
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
  };
  int code = bind(echo_listen_fd, (struct sockaddr*)&addr, sizeof(addr));
  assert(code == 0);
  code = listen(echo_listen_fd, 1024);
  assert(code == 0);

  socklen_t len = sizeof(addr);
  code = getsockname(echo_listen_fd, (struct sockaddr*)&addr, &len);
  assert(code == 0);
  (void)code;
  echo_port = ntohs(addr.sin_port);
}

void test_io() {
  // Тест 1
  const int code = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair);
  assert(code == 0);
  (void)code;
  atomic_store(&transfer_received, 0);
  atomic_store(&transfer_corrupted, false);

  tasks_init();
  tasks_submit(&transfer_test, NULL);
  tasks_start();
  tasks_wait();
  tasks_destroy();

  assert(atomic_load(&transfer_received) == TRANSFER_SIZE);
  assert(!atomic_load(&transfer_corrupted));

  // Тест 2
  echo_listen();
  atomic_store(&echo_ok, 0);

  tasks_init();
  tasks_submit(&echo_test, NULL);
  tasks_start();
  tasks_wait();
  tasks_destroy();

  assert(atomic_load(&echo_ok) == ECHO_CLIENTS);
}
