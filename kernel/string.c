#include "types.h"

void* memset(void* dst, int c, uint n)
{
    char* cdst = (char*)dst;
    for (int i = 0; i < n; i++) {
        cdst[i] = c;
    }
    return dst;
}

int memcmp(const void* v1, const void* v2, uint n)
{
    const uchar* s1 = v1;
    const uchar* s2 = v2;
    
    while (n-- > 0) {
        if (*s1 != *s2)
            return *s1 - *s2;
        s1++, s2++;
    }

    return 0;
}

void* memmove(void* dst, const void* src, uint n)
{
    if (n == 0)
        return dst;

    char* d = dst;
    const char* s = src;

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

void* memcpy(void* dst, const void* src, uint n) { return memmove(dst, src, n); }

// 比较字符串 (最多n个字符)
int strncmp(const char* s1, const char* s2, uint n)
{
    while (n > 0 && *s1 && *s1 == *s2)
        n--, s1++, s2++;
    if (n == 0)
        return 0;
    return (uchar)*s1 - (uchar)*s2;
}

char* strncpy(char* dst, const char* src, int n)
{
    char* d = dst;
    while (n-- > 0 && (*dst++ = *src++) != 0)
        ;
    while (n-- > 0)
        *dst++ = 0;
    return d;
}

// 将t复制到s, 最多n个字符, 并保证s以null结尾
char* safestrcpy(char* dst, const char* src, int n)
{
    char* d = dst;
    if (n <= 0)
        return d;
    while (--n > 0 && (*dst++ = *src++) != 0)
        ;
    *dst = 0;
    return d;
}

// 返回字符串s的长度(不包括结尾null)
int strlen(const char* s)
{
    int n;
    for (n = 0; s[n]; n++)
        ;
    return n;
}
