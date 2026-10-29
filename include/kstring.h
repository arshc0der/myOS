/*
 * kstring.h - string and memory functions (the kernel has no libc, so
 * we provide our own versions).
 */
#ifndef KSTRING_H
#define KSTRING_H

#include "types.h"

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
/* Safe copy: always NUL-terminates; returns strlen(src). */
size_t strlcpy(char *dst, const char *src, size_t size);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

/* Convert text to a number. base 0 = auto (0x.. hex, 0.. octal, else 10). */
uint32_t kstrtoul(const char *s, char **end, int base);
int32_t  katoi(const char *s);

bool isdigit(int c);
bool isalpha(int c);
bool isalnum(int c);
bool isspace(int c);
bool isupper(int c);
bool islower(int c);
bool isxdigit(int c);
int  toupper(int c);
int  tolower(int c);

#endif
