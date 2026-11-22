#include "schedy.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coroed/api/task.h"
#include "coroed/core/relax.h"
#include "coroed/core/spinlock.h"
#include "kthread.h"
#include "poller.h"
#include "uthread.h"

/**
 * Лимит задач можно переопределить при сборке:
 * `make CFLAGS_EXTRA=-DCOROED_MAX_TASKS=2048`.
 */
#ifndef COROED_MAX_TASKS
#define COROED_MAX_TASKS 512
#endif

enum {
  /**
   * Максимальное количество файберов, которое может
   * одновременно выполняться на планировщике.
   */
  SCHED_THREADS_LIMIT = COROED_MAX_TASKS,

  /**
   * Количество рабочих потоков для исполнения файберов.
   */
  SCHED_WORKERS_COUNT = (size_t)(8),

  /**
   * Обеспечивает костыль для ретрая операций
   * `submit` и `acquire_next` при высокой
   * конкуренции на спинлоках файберов.
   */
  SCHED_NEXT_MAX_ATTEMPTS = (size_t)(16),

  /**
   * Раз во сколько шагов рабочий поток неблокирующе
   * опрашивает поллер ввода-вывода.
   */
  SCHED_POLL_PERIOD = (size_t)(8),

  /**
   * Сколько миллисекунд простаивающий рабочий поток ждет
   * событий ввода-вывода, прежде чем снова поискать задачи.
   */
  SCHED_IDLE_POLL_TIMEOUT_MS = 10,
};

/**
 * Задача, выполняющаяся на планировщике.
 *
 * Hint: также тут можно хранить список задач, ожидающих
 *       завершения этой, чтобы уведомить их о данном
 *       событии.
 */
struct task {
  /**
   * Контекст исполнения.
   */
  struct uthread* thread;

  /**
   * Установлен при `UTHREAD_RUNNING`. Указывает на
   * рабочий поток, на котором исполняется задача.
   * Необходим для получения контекста локального
   * планировщика при операциях `yield`, `await`, `submit`.
   */
  struct worker* worker;

  enum {
    /** Готова к исполнению. */
    UTHREAD_RUNNABLE,

    /** Прямо сейчас выполняется. */
    UTHREAD_RUNNING,

    /**
     * Ждет внешнего события (ввод-вывод, `event`) и не
     * выбирается планировщиком, пока кто-то не вызовет
     * `task_wake`, переводящий ее обратно в `RUNNABLE`.
     */
    UTHREAD_BLOCKED,

    /** Завершена и скоро станет зомби. */
    UTHREAD_FINISHED,

    /** Отработала и может быть переиспользования. */
    UTHREAD_ZOMBIE,
  } state;  // Текущее состояние задачи

  /**
   * Защищает поля структуры от неупорядоченного доступа.
   */
  struct spinlock lock;
};

/**
 * Рабочий поток, исполняющий файберы.
 */
struct worker {
  /**
   * Идентификатор рабочего потока.
   */
  size_t index;

  /**
   * Поток операционной системы, на котором
   * исполняется рабочий.
   */
  struct kthread kthread;

  /**
   * Контекст планировщика. Нужно переключиться на него,
   * чтобы вернуться в планировщик.
   */
  struct uthread sched_thread;

  /**
   * В данный момент исполняемая на рабочем
   * задача. Может быть `NULL`.
   */
  struct task* running_task;

  struct {
    size_t steps;     // Сколько шагов было выполнено
    size_t finished;  // Сколько задач было завершено
  } statistics;       // Локальная статистика работяги
};

/**
 * Количество "живых" задач: отправленных в планировщик и еще
 * не завершенных (RUNNABLE, RUNNING или BLOCKED). Рабочие
 * потоки завершаются, только когда оно становится равным нулю,
 * иначе сервер, все задачи которого ждут клиентов, сразу
 * завершился бы.
 */
static atomic_size_t tasks_alive = 0;

static struct spinlock tasks_lock;              // Защищает список задач
static size_t next_task_index = 0;              // Для планирования round-robin
static struct task tasks[SCHED_THREADS_LIMIT];  // Список всех задач

// Hint: для реализации более сложных схем управления, вам
//       вам могут понадобиться связаные списки (`core/list.h`).

static kthread_id_t kthread_ids[SCHED_WORKERS_COUNT];
static struct worker workers[SCHED_WORKERS_COUNT];

/**
 * Установить задачу в пустое состояние.
 */
void sched_task_init(struct task* task) {
  task->thread = NULL;
  task->worker = NULL;
  task->state = UTHREAD_ZOMBIE;
  spinlock_init(&task->lock);
}

/**
 * Установить рабочего в пустое состояние.
 */
void sched_worker_init(struct worker* worker, size_t index) {
  worker->index = index;
  worker->sched_thread.context = NULL;
  worker->running_task = NULL;
  worker->statistics.steps = 0;
  worker->statistics.finished = 0;
}

void sched_init() {
  poller_init();
  spinlock_init(&tasks_lock);
  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    sched_task_init(&tasks[i]);
  }
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    kthread_ids[i] = 0;
    sched_worker_init(&workers[i], i);
  }
}

/**
 * Находясь в контексте задачи `task`, переключиться
 * в контекст планировщика.
 */
void sched_switch_to_scheduler(struct task* task) {
  struct uthread* sched = &task->worker->sched_thread;
  task->worker = NULL;
  uthread_switch(task->thread, sched);
}

/**
 * Находясь в контексте планировщика, переключиться
 * в контекст задачи `task`. Выполнять ее до следующего
 * невынужденного возвращения в планировщик.
 */
void sched_switch_to(struct worker* worker, struct task* task) {
  assert(task->thread != &worker->sched_thread);

  task->state = UTHREAD_RUNNING;

  task->worker = worker;
  worker->running_task = task;

  struct uthread* sched = &worker->sched_thread;
  uthread_switch(sched, task->thread);
}

/**
 * Получить следующую задачу на исполнение в
 * соответствии с текущим алгоритмом планирования.
 *
 * Вызывающему передается владение задачей, а также
 * захваченный лок `task->lock`.
 */
struct task* sched_acquire_next();

/**
 * Вернуть задачу в очередь планирования.
 *
 * Очереди передается владение задачей,
 * а также она отпустит лок `task->lock`.
 */
void sched_release(struct task* task);

/**
 * Цикл планировщика. Выполняется, пока есть задачи.
 */
int sched_loop(void* argument) {
  struct worker* worker = argument;
  kthread_ids[worker->index] = kthread_id();

  for (;;) {
    struct task* task = sched_acquire_next();
    if (task == NULL) {
      if (atomic_load(&tasks_alive) == 0) {
        break;  // Все задачи завершены
      }
      // Готовых задач нет, но есть ожидающие (BLOCKED).
      // Ждем событий ввода-вывода вместо пустого вращения:
      // epoll_wait вернется сразу, как только какой-то
      // дескриптор станет готов, или по таймауту.
      poller_poll(SCHED_IDLE_POLL_TIMEOUT_MS);
      continue;
    }

    sched_switch_to(worker, task);

    worker->statistics.steps += 1;
    if (task->state == UTHREAD_FINISHED) {
      worker->statistics.finished += 1;
    }

    sched_release(task);

    // Задача уже ушла со стека и ее лок отпущен, поэтому
    // здесь безопасно будить задачи, в том числе только что
    // припаркованную. Неблокирующий опрос делаем раз в
    // несколько шагов, чтобы при большом числе готовых задач
    // ожидающие ввода-вывода не голодали.
    if (worker->statistics.steps % SCHED_POLL_PERIOD == 0) {
      poller_poll(/* timeout_ms = */ 0);
    }
  }

  return 0;
}

struct task* sched_acquire_next() {
  // На всякий случай пытаемся найти задачу несколько раз,
  // так как какие-то `task->lock` могли быть отпущены.

  for (size_t attempt = 0; attempt < SCHED_NEXT_MAX_ATTEMPTS; ++attempt) {
    spinlock_lock(&tasks_lock);  // Защитим `next_task_index`

    for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
      struct task* task = &tasks[next_task_index];

      // Планирование round-robin. Индекс сдвигаем всегда: в
      // исходной версии он сдвигался только после успешного
      // `try_lock`, и занятый слот (например, исполняющаяся
      // задача) проверялся повторно до конца прохода.
      next_task_index = (next_task_index + 1) % SCHED_THREADS_LIMIT;

      // Дешевая проверка без захвата лока: большинство слотов
      // не RUNNABLE, и атомарная запись в их лок лишь гоняла бы
      // кэш-линии между ядрами. Под локом состояние
      // перепроверяется.
      if (__atomic_load_n(&task->state, __ATOMIC_RELAXED) != UTHREAD_RUNNABLE) {
        continue;
      }
      if (!spinlock_try_lock(&task->lock)) {
        continue;
      }

      if (task->thread != NULL && task->state == UTHREAD_RUNNABLE) {
        spinlock_unlock(&tasks_lock);
        return task;
      }

      spinlock_unlock(&task->lock);
    }

    spinlock_unlock(&tasks_lock);
    SPINLOOP(2 * attempt);
  }

  return NULL;
}

void sched_release(struct task* task) {
  task->worker = NULL;
  if (task->state == UTHREAD_FINISHED) {
    // Отправляем задачу на кладбище, а могли бы
    // еще, например, разблокировать зависимые задачи.
    uthread_reset(task->thread);
    task->state = UTHREAD_ZOMBIE;
    atomic_fetch_sub(&tasks_alive, 1);
  } else if (task->state == UTHREAD_RUNNING) {
    task->state = UTHREAD_RUNNABLE;
  } else {
    // Задача сама ушла в ожидание через `task_park`.
    // Оставляем BLOCKED: в RUNNABLE ее переведет `task_wake`.
    assert(task->state == UTHREAD_BLOCKED);
  }
  // Только после этого `task_wake` сможет захватить лок,
  // то есть задача гарантированно уже ушла со своего стека.
  spinlock_unlock(&task->lock);
}

/**
 * Отметить задачу завершенной.
 */
void sched_finish(struct task* task) {
  task->state = UTHREAD_FINISHED;
}

void task_yield(struct task* caller) {
  sched_switch_to_scheduler(caller);
}

void task_exit(struct task* caller) {
  sched_finish(caller);
  task_yield(caller);
}

void task_park(struct task* caller) {
  // `caller->lock` сейчас удерживает рабочий поток (задача
  // исполняется), поэтому `task_wake` из другого потока будет
  // ждать на нем до `sched_release`, то есть до момента, когда
  // задача уже ушла со стека. Так пробуждение, пришедшее раньше
  // парковки, не теряется и задача не запускается дважды.
  caller->state = UTHREAD_BLOCKED;
  sched_switch_to_scheduler(caller);
}

void task_wake(struct task* task) {
  spinlock_lock(&task->lock);
  if (task->state == UTHREAD_BLOCKED) {
    task->state = UTHREAD_RUNNABLE;
  }
  spinlock_unlock(&task->lock);
}

task_t task_submit(struct task* caller, uthread_routine entry, void* argument) {
  (void)caller;  // Может быть полезно.
  task_t child = sched_submit(*entry, argument);
  return child;
}

task_t task_try_submit(struct task* caller, uthread_routine entry, void* argument) {
  (void)caller;
  return sched_try_submit(entry, argument);
}

task_t sched_try_submit(void (*entry)(), void* argument) {
  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    struct task* task = &tasks[i];

    if (!spinlock_try_lock(&task->lock)) {
      continue;
    }

    if (task->thread == NULL) {
      task->thread = uthread_allocate();
      assert(task->thread != NULL);
      task->state = UTHREAD_ZOMBIE;
    }

    const bool is_submitted = task->state == UTHREAD_ZOMBIE;

    if (task->state == UTHREAD_ZOMBIE) {
      uthread_reset(task->thread);
      uthread_set_entry(task->thread, entry);
      uthread_set_arg_0(task->thread, task);
      uthread_set_arg_1(task->thread, argument);
      task->state = UTHREAD_RUNNABLE;
      atomic_fetch_add(&tasks_alive, 1);
    }

    spinlock_unlock(&task->lock);
    if (is_submitted) {
      return (task_t){.task = task};
    }
  }

  return (task_t){.task = NULL};
}

task_t sched_submit(void (*entry)(), void* argument) {
  for (size_t attempt = 0; attempt < SCHED_NEXT_MAX_ATTEMPTS; ++attempt) {
    task_t handle = sched_try_submit(entry, argument);
    if (handle.task != NULL) {
      return handle;
    }
    SPINLOOP(2 * attempt);
  }

  assert(false && "Can't create a task");
}

void sched_start() {
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    enum kthread_status status = kthread_create(&worker->kthread, sched_loop, worker);
    assert(status == KTHREAD_SUCCESS);
  }
}

void sched_wait() {
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    enum kthread_status status = kthread_join(&worker->kthread);
    assert(status == KTHREAD_SUCCESS);
  }
}

void sched_print_statistics() {
  printf("\nsched statistics\n");

  size_t tasks_count = 0;
  size_t steps_count = 0;
  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    tasks_count += worker->statistics.finished;
    steps_count += worker->statistics.steps;
  }

  printf("|- tasks executed %zu\n", tasks_count);
  printf("|- steps done     %zu\n", steps_count);

  for (size_t i = 0; i < SCHED_WORKERS_COUNT; ++i) {
    struct worker* worker = &workers[i];
    printf("|- worker %zu %zu\n", i, kthread_ids[i]);
    printf("   |- steps     %zu\n", worker->statistics.steps);
    printf("   |- finished  %zu\n", worker->statistics.finished);
  }
}

void sched_destroy() {
  for (size_t i = 0; i < SCHED_THREADS_LIMIT; ++i) {
    struct task* task = &tasks[i];
    spinlock_lock(&task->lock);
    if (task->thread != NULL) {
      uthread_free(task->thread);
    }
    spinlock_unlock(&task->lock);
  }
  poller_destroy();
}
