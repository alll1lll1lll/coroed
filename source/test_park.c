#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "coroed/api/event.h"
#include "coroed/api/sleep.h"
#include "coroed/api/task.h"

enum {
  WAITERS_COUNT = 256,
  ROUNDS = 8,
};

static struct event events[ROUNDS];
static atomic_size_t woken = 0;

TASK_DEFINE(park_waiter, void, ignored) {
  // Каждый ожидающий проходит через все раунды: паркуется,
  // просыпается и тут же снова ждет следующего события.
  for (size_t round = 0; round < ROUNDS; ++round) {
    EVENT_WAIT(&events[round]);
    atomic_fetch_add(&woken, 1);
  }
}

TASK_DEFINE(park_firer, void, ignored) {
  for (size_t round = 0; round < ROUNDS; ++round) {
    YIELD;
    event_fire(&events[round]);
  }
}

TASK_DEFINE(park_test, void, ignored) {
  for (size_t i = 0; i < WAITERS_COUNT; ++i) {
    GO(park_waiter, NULL);
  }
  GO(park_firer, NULL);
}

void test_park() {
  for (size_t i = 0; i < ROUNDS; ++i) {
    event_init(&events[i]);
  }
  atomic_store(&woken, 0);

  tasks_init();
  tasks_submit(&park_test, NULL);
  tasks_start();
  tasks_wait();
  tasks_destroy();

  assert(atomic_load(&woken) == (size_t)WAITERS_COUNT * ROUNDS);
}
