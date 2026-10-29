/*
 * log.h - kernel log messages: "[   1.234] tag: message".
 *
 * Every message goes to the serial port. Messages at LOG_INFO or higher
 * also go to the screen.
 */
#ifndef LOG_H
#define LOG_H

#include "types.h"

enum log_level {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3,
};

void klog(int level, const char *tag, const char *fmt, ...) PRINTF_LIKE(3, 4);
void log_set_console_level(int level);

#define log_debug(tag, ...) klog(LOG_DEBUG, tag, __VA_ARGS__)
#define log_info(tag, ...)  klog(LOG_INFO,  tag, __VA_ARGS__)
#define log_warn(tag, ...)  klog(LOG_WARN,  tag, __VA_ARGS__)
#define log_error(tag, ...) klog(LOG_ERROR, tag, __VA_ARGS__)

#endif
