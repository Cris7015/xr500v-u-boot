/* SPDX-License-Identifier: GPL-2.0-or-later */
typedef unsigned int size_t;

char *strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++) != '\0')
		;
	return r;
}

int strcmp(const char *a, const char *b)
{
	signed char r;
	for (;;) {
		r = *a - *b++;
		if (r || !*a++)
			break;
	}
	return r;
}

size_t strlen(const char *s)
{
	const char *p;
	for (p = s; *p; p++)
		;
	return p - s;
}

void *memset(void *d, int v, size_t c)
{
	unsigned char *p = d;
	while (c--)
		*p++ = v;
	return d;
}

void *memcpy(void *d, const void *s, size_t c)
{
	unsigned char *x = d;
	const unsigned char *y = s;
	while (c--)
		*x++ = *y++;
	return d;
}

void *memcpy4(void *d, const void *s, size_t c)
{
	unsigned int *x = d;
	const unsigned int *y = s;
	if (((unsigned long)x & 3) || ((unsigned long)y & 3))
		return memcpy(d, s, c);
	while (c >= 4) {
		*x++ = *y++;
		c -= 4;
	}
	if (c)
		memcpy(x, y, c);
	return d;
}
