/* kstring.c - string and memory functions. */
#include "kstring.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    __asm__ volatile ("cld; rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return dst;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    __asm__ volatile ("cld; rep stosb"
                      : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] - y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (uint8_t)c)
            return (void *)(p + i);
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *r = dst;
    while ((*dst++ = *src++))
        ;
    return r;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);

    if (nl == 0)
        return (char *)h;
    for (; *h; h++)
        if (*h == *n && strncmp(h, n, nl) == 0)
            return (char *)h;
    return NULL;
}

bool isdigit(int c)  { return c >= '0' && c <= '9'; }
bool isupper(int c)  { return c >= 'A' && c <= 'Z'; }
bool islower(int c)  { return c >= 'a' && c <= 'z'; }
bool isalpha(int c)  { return isupper(c) || islower(c); }
bool isalnum(int c)  { return isalpha(c) || isdigit(c); }
bool isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
bool isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') ||
                              (c >= 'A' && c <= 'F'); }
int toupper(int c)   { return islower(c) ? c - 32 : c; }
int tolower(int c)   { return isupper(c) ? c + 32 : c; }

uint32_t kstrtoul(const char *s, char **end, int base)
{
    uint32_t v = 0;

    while (isspace(*s))
        s++;
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    for (;; s++) {
        int d;
        if (isdigit(*s))
            d = *s - '0';
        else if (isalpha(*s))
            d = tolower(*s) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * (uint32_t)base + (uint32_t)d;
    }
    if (end)
        *end = (char *)s;
    return v;
}

int32_t katoi(const char *s)
{
    bool neg = false;

    while (isspace(*s))
        s++;
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int32_t v = (int32_t)kstrtoul(s, NULL, 10);
    return neg ? -v : v;
}
