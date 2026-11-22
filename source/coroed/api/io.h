#pragma once

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "coroed/api/task.h"

/**
 * Асинхронный ввод-вывод для файберов.
 *
 * Для вызывающего файбера функции выглядят блокирующими:
 * возврат происходит, когда операция выполнена. Внутри же
 * дескриптор работает в неблокирующем режиме, и если операцию
 * нельзя выполнить сразу (EAGAIN), файбер паркуется до
 * готовности дескриптора, а его рабочий поток занимают другие.
 *
 * Все дескрипторы должны быть неблокирующими (O_NONBLOCK).
 *
 * Ошибки возвращаются как `-errno`, а не через `errno`:
 * `errno` хранится в thread-local памяти, а файбер после
 * парковки может продолжиться на другом потоке ОС.
 */

/**
 * Синтаксический сахар для использования в теле файбера.
 */
#define IO_ACCEPT(fd) io_accept(__self, (fd))
#define IO_CONNECT(fd, addr, len) io_connect(__self, (fd), (addr), (len))
#define IO_READ(fd, buf, len) io_read(__self, (fd), (buf), (len))
#define IO_WRITE(fd, buf, len) io_write(__self, (fd), (buf), (len))

/**
 * Перевести дескриптор в неблокирующий режим.
 * Возвращает 0 или `-errno`.
 */
int io_set_nonblocking(int fd);

/**
 * Принять соединение на слушающем сокете `listen_fd`.
 * Возвращает дескриптор клиента (уже неблокирующий)
 * или `-errno`.
 */
int io_accept(struct task* caller, int listen_fd);

/**
 * Установить соединение на сокете `fd`.
 * Возвращает 0 или `-errno`.
 */
int io_connect(struct task* caller, int fd, const struct sockaddr* addr, socklen_t len);

/**
 * Прочитать до `len` байт. Возвращает число прочитанных
 * байт (0 -- собеседник закрыл соединение) или `-errno`.
 */
ssize_t io_read(struct task* caller, int fd, void* buf, size_t len);

/**
 * Записать ровно `len` байт (с повторами при частичной
 * записи). Возвращает `len` или `-errno`.
 */
ssize_t io_write(struct task* caller, int fd, const void* buf, size_t len);
