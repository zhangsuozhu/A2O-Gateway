#ifndef LOG_H
#define LOG_H

#include <stdbool.h>
#include <stdarg.h>
#include "types.h"

typedef enum { RT_OFF, RT_ALL, RT_TXT } rt_mode_t;

void log_open(const char *path);
void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_set_level(const char *level);
const char *log_get_level(void);
void rt_set_mode(rt_mode_t mode);
rt_mode_t rt_get_mode(void);
const char *rt_mode_name(void);
void rt_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_print_json(const char *tag, const char *body);

#endif