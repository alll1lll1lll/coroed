#include "event.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "coroed/core/spinlock.h"
#include "task.h"

void event_init(struct event* event) {
  atomic_store(&event->is_fired, false);
  spinlock_init(&event->lock);
  event->waiters = NULL;
}

void event_wait(struct task* caller, struct event* event) {
  // Быстрый путь: событие уже произошло.
  if (atomic_load(&event->is_fired)) {
    return;
  }

  spinlock_lock(&event->lock);
  if (atomic_load(&event->is_fired)) {
    spinlock_unlock(&event->lock);
    return;
  }

  // Встаем в очередь и паркуемся. Даже если `event_fire`
  // разбудит нас между `unlock` и `task_park`, пробуждение
  // не потеряется: `task_wake` дождется окончания парковки.
  struct event_waiter self = {.task = caller, .next = event->waiters};
  event->waiters = &self;
  spinlock_unlock(&event->lock);

  task_park(caller);

  assert(atomic_load(&event->is_fired));
}

void event_fire(struct event* event) {
  spinlock_lock(&event->lock);
  atomic_store(&event->is_fired, true);
  struct event_waiter* waiter = event->waiters;
  event->waiters = NULL;
  spinlock_unlock(&event->lock);

  while (waiter != NULL) {
    // Узел лежит на стеке ожидающего: после `task_wake` файбер
    // может продолжиться и узел исчезнет, поэтому читаем все
    // поля до пробуждения.
    struct event_waiter* next = waiter->next;
    struct task* task = waiter->task;
    task_wake(task);
    waiter = next;
  }
}
