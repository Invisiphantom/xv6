#include "types.h"

void* memset(void* dst, int c, uint n) {
    char* cdst = (char*)dst;
    int i;
    for (i = 0; i < n; i++) {
        cdst[i] = c;
    }
    return dst;
}

int memcmp(const void* v1, const void* v2, uint n) {
    const uchar *s1, *s2;

    s1 = v1;
    s2 = v2;
    while (n-- > 0) {
        if (*s1 != *s2)
            return *s1 - *s2;
        s1++, s2++;
    }

    return 0;
}

void* memmove(void* dst, const void* src, uint n) {
    const char* s;
    char* d;

    if (n == 0)
        return dst;

    s = src;
    d = dst;
    if (s < d && s + n > d) {
        s += n;
        d += n;
        while (n-- > 0)
            *--d = *--s;
    } else
        while (n-- > 0)
            *d++ = *s++;

    return dst;
}

// memcpy exists to placate GCC.  Use memmove.
void* memcpy(void* dst, const void* src, uint n) {
    return memmove(dst, src, n);
}

// 比较字符串s和t的前n个字符
int strncmp(const char* p, const char* q, uint n) {
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (uchar)*p - (uchar)*q;
}

char* strncpy(char* s, const char* t, int n) {
    char* os;

    os = s;
    while (n-- > 0 && (*s++ = *t++) != 0)
        ;
    while (n-- > 0)
        *s++ = 0;
    return os;
}

// 将t复制到s, 最多n个字符, 并保证s以null结尾
char* safestrcpy(char* s, const char* t, int n) {
    char* os;

    os = s;
    if (n <= 0)
        return os;
    while (--n > 0 && (*s++ = *t++) != 0)
        ;
    *s = 0;
    return os;
}

// 返回字符串s的长度(不包括结尾null)
int strlen(const char* s) {
    int n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}
