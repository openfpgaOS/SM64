/*
 * String functions for VexRiscv
 */

#include "libc.h"

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) {
        p++;
    }
    return p - s;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
        /* copy */
    }
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;

    /* Copy up to n characters */
    while (n > 0 && *src != '\0') {
        *d++ = *src++;
        n--;
    }

    /* Pad with nulls */
    while (n > 0) {
        *d++ = '\0';
        n--;
    }

    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;

    /* Find end of dest */
    while (*d) {
        d++;
    }

    /* Append src */
    while ((*d++ = *src++) != '\0') {
        /* copy */
    }

    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;

    /* Find end of dest */
    while (*d) {
        d++;
    }

    /* Append up to n characters from src */
    while (n > 0 && *src != '\0') {
        *d++ = *src++;
        n--;
    }

    *d = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }

    if (n == 0) {
        return 0;
    }

    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;

    while (*s) {
        if (*s == ch) {
            return (char *)s;
        }
        s++;
    }

    /* Check for null terminator match */
    if (ch == '\0') {
        return (char *)s;
    }

    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    char ch = (char)c;

    while (*s) {
        if (*s == ch) {
            last = s;
        }
        s++;
    }

    /* Check for null terminator match */
    if (ch == '\0') {
        return (char *)s;
    }

    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    size_t nlen;
    if (!*needle) return (char *)haystack;
    nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strtok(char *str, const char *delim) {
    static char *last = NULL;
    char *start;

    if (str) last = str;
    if (!last) return NULL;

    /* Skip leading delimiters */
    while (*last && strchr(delim, *last)) last++;
    if (!*last) { last = NULL; return NULL; }

    start = last;
    /* Find end of token */
    while (*last && !strchr(delim, *last)) last++;
    if (*last) {
        *last = '\0';
        last++;
    } else {
        last = NULL;
    }
    return start;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n > 0 && *s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}
