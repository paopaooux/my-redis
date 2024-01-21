#ifndef COMMON_H
#define CONMON_H

#include <stddef.h>
#include <stdint.h>

/*
  ptr：一个指向结构体中某个成员的指针。
  type：结构体的类型。
  member：结构体中的成员名称。
  根据给定的成员指针 ptr，反向计算出包含该成员的整个结构体的指针*/
#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)0)->member) *__mptr = (ptr);                     \
        (type *)((char *)__mptr - offsetof(type, member));                     \
    })

uint64_t str_hash(const uint8_t *data, size_t len);

enum {
    SER_NIL = 0, // NULL
    SER_ERR = 1, // error
    SER_STR = 2, // string
    SER_INT = 3, // int64
    SER_DBL = 4, // 浮点数
    SER_ARR = 5, // Array
};

#endif // COMMON_H