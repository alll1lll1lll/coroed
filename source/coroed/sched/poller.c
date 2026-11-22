#include "poller.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "coroed/api/task.h"

enum {
  /**
   * Сколько событий забирать за один вызов `epoll_wait`.
   */
  POLLER_BATCH_SIZE = 64,
};

static int epoll_fd = -1;

void poller_init() {
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  assert(epoll_fd >= 0);
}

void poller_destroy() {
  if (epoll_fd >= 0) {
    close(epoll_fd);
    epoll_fd = -1;
  }
}

/**
 * Взвести (или перевзвести) ожидание событий на `fd`.
 *
 * Вынесено в отдельную `noinline` функцию, чтобы `errno`
 * читался сразу после системного вызова и в том же потоке
 * ОС: адрес thread-local `errno` компилятор может вычислить
 * один раз на функцию, а задача после парковки может
 * продолжиться уже на другом рабочем потоке.
 */
[[gnu::noinline]] static int poller_arm(struct task* caller, int fd, uint32_t events) {
  struct epoll_event event = {
      .events = events | EPOLLONESHOT,
      .data.ptr = caller,
  };

  // После срабатывания EPOLLONESHOT дескриптор остается
  // в epoll, но выключен: его достаточно перевзвести через MOD.
  // Новый дескриптор (или переиспользованный номер после close)
  // в epoll еще не зарегистрирован: MOD вернет ENOENT, тогда ADD.
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == 0) {
    return 0;
  }
  if (errno != ENOENT) {
    return -errno;
  }
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == 0) {
    return 0;
  }
  return -errno;
}

int poller_wait(struct task* caller, int fd, uint32_t events) {
  const int code = poller_arm(caller, fd, events);
  if (code != 0) {
    return code;
  }

  // Событие может прийти еще до того, как задача уйдет со
  // стека. Тогда `task_wake` в `poller_poll` дождется на локе
  // задачи окончания парковки, и пробуждение не потеряется.
  task_park(caller);
  return 0;
}

int poller_poll(int timeout_ms) {
  struct epoll_event events[POLLER_BATCH_SIZE];

  const int count = epoll_wait(epoll_fd, events, POLLER_BATCH_SIZE, timeout_ms);
  if (count <= 0) {
    return 0;  // Нет событий, таймаут или EINTR
  }

  for (int i = 0; i < count; ++i) {
    struct task* task = events[i].data.ptr;
    task_wake(task);
  }
  return count;
}
