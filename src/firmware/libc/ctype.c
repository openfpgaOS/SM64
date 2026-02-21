/*
 * Character classification functions for VexRiscv
 */

#include "libc.h"

int isprint(int c) {
    return (c >= 0x20 && c <= 0x7E);
}

int isspace(int c) {
    return (c == ' ' || c == '\t' || c == '\n' ||
            c == '\r' || c == '\f' || c == '\v');
}

int isdigit(int c) {
    return (c >= '0' && c <= '9');
}

int isalpha(int c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isupper(int c) {
    return (c >= 'A' && c <= 'Z');
}

int islower(int c) {
    return (c >= 'a' && c <= 'z');
}

int tolower(int c) {
    if (isupper(c)) {
        return c + ('a' - 'A');
    }
    return c;
}

int toupper(int c) {
    if (islower(c)) {
        return c - ('a' - 'A');
    }
    return c;
}
