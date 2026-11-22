#define _GNU_SOURCE  // accept4

#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coroed/sched/poller.h"

// Обертки над системными вызовами.
//
// Каждая читает `errno` сразу после вызова и возвращает
// `-errno`. Они `noinline` намеренно: glibc объявляет
// `__errno_location` как `const`, поэтому компилятор вправе
// вычислить адрес `errno` один раз на функцию. Если бы цикл с
// `poller_wait` (а значит, с возможной сменой потока ОС) читал
// `errno` сам, после парковки он мог бы прочитать `errno`
// чужого потока. В отдельной функции адрес вычисляется заново
// при каждом вызове, уже на текущем потоке.

[[gnu::noinline]] static ssize_t sys_read(int fd, void* buf, size_t len) {
  const ssize_t n = read(fd, buf, len);
  return n >= 0 ? n : -errno;
}

[[gnu::noinline]] static ssize_t sys_write(int fd, const void* buf, size_t len) {
  // MSG_NOSIGNAL: запись в закрытый собеседником сокет вернет
  // EPIPE вместо сигнала SIGPIPE, который убил бы процесс.
  ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
  if (n < 0 && errno == ENOTSOCK) {
    n = write(fd, buf, len);  // Не сокет (например, pipe)
  }
  return n >= 0 ? n : -errno;
}

[[gnu::noinline]] static int sys_accept(int listen_fd) {
  const int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  return fd >= 0 ? fd : -errno;
}

[[gnu::noinline]] static int sys_connect(int fd, const struct sockaddr* addr, socklen_t len) {
  return connect(fd, addr, len) == 0 ? 0 : -errno;
}

[[gnu::noinline]] static int sys_socket_error(int fd) {
  int error = 0;
  socklen_t len = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0) {
    return -errno;
  }
  return -error;
}

static bool would_block(long code) {
  return code == -EAGAIN || code == -EWOULDBLOCK;
}

int io_set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -errno;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -errno;
  }
  return 0;
}

int io_accept(struct task* caller, int listen_fd) {
  for (;;) {
    const int fd = sys_accept(listen_fd);
    if (fd >= 0) {
      return fd;
    }
    if (fd == -EINTR || fd == -ECONNABORTED) {
      continue;  // Повторить сразу
    }
    if (!would_block(fd)) {
      return fd;
    }
    // Новых соединений нет: ждем, пока слушающий сокет
    // станет "готов к чтению", то есть придет клиент.
    const int code = poller_wait(caller, listen_fd, EPOLLIN);
    if (code != 0) {
      return code;
    }
  }
}

int io_connect(struct task* caller, int fd, const struct sockaddr* addr, socklen_t len) {
  const int code = sys_connect(fd, addr, len);
  if (code == 0) {
    return 0;
  }
  if (code != -EINPROGRESS && code != -EINTR) {
    return code;
  }
  // Соединение устанавливается в фоне. Оно готово, когда
  // сокет становится доступен для записи; результат -- SO_ERROR.
  const int wait_code = poller_wait(caller, fd, EPOLLOUT);
  if (wait_code != 0) {
    return wait_code;
  }
  return sys_socket_error(fd);
}

ssize_t io_read(struct task* caller, int fd, void* buf, size_t len) {
  for (;;) {
    const ssize_t n = sys_read(fd, buf, len);
    if (n >= 0) {
      return n;
    }
    if (n == -EINTR) {
      continue;
    }
    if (!would_block(n)) {
      return n;
    }
    const int code = poller_wait(caller, fd, EPOLLIN);
    if (code != 0) {
      return code;
    }
  }
}

ssize_t io_write(struct task* caller, int fd, const void* buf, size_t len) {
  const char* data = buf;
  size_t written = 0;
  while (written < len) {
    const ssize_t n = sys_write(fd, data + written, len - written);
    if (n >= 0) {
      written += (size_t)n;
      continue;
    }
    if (n == -EINTR) {
      continue;
    }
    if (!would_block(n)) {
      return n;
    }
    // Буфер отправки ядра заполнен: ждем, пока освободится.
    const int code = poller_wait(caller, fd, EPOLLOUT);
    if (code != 0) {
      return code;
    }
  }
  return (ssize_t)len;
}
