#pragma once

#include <aevos/types.h>

/* Memory operations */
void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* String length */
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);

/* String copy */
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);

/* String concatenation */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

/* String comparison */
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

/* String search */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

/* Number conversion */
int  atoi(const char *s);
long atol(const char *s);
char *itoa(int value, char *buf, int base);
char *ltoa(long value, char *buf, int base);

/* Formatted output */
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Character classification */
int toupper(int c);
int tolower(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isxdigit(int c);
