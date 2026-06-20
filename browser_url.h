/* browser_url.h — URL handling utilities shared between browser and tests */
#ifndef LUMO_BROWSER_URL_H
#define LUMO_BROWSER_URL_H

#include <stdio.h>
#include <string.h>

static inline void lumo_url_encode(const char *src, char *dst, size_t dst_size) {
    /* O(1) lookup table instead of O(n) strchr per character */
    static const unsigned char safe[256] = {
        ['A']=1,['B']=1,['C']=1,['D']=1,['E']=1,['F']=1,['G']=1,
        ['H']=1,['I']=1,['J']=1,['K']=1,['L']=1,['M']=1,['N']=1,
        ['O']=1,['P']=1,['Q']=1,['R']=1,['S']=1,['T']=1,['U']=1,
        ['V']=1,['W']=1,['X']=1,['Y']=1,['Z']=1,
        ['a']=1,['b']=1,['c']=1,['d']=1,['e']=1,['f']=1,['g']=1,
        ['h']=1,['i']=1,['j']=1,['k']=1,['l']=1,['m']=1,['n']=1,
        ['o']=1,['p']=1,['q']=1,['r']=1,['s']=1,['t']=1,['u']=1,
        ['v']=1,['w']=1,['x']=1,['y']=1,['z']=1,
        ['0']=1,['1']=1,['2']=1,['3']=1,['4']=1,['5']=1,['6']=1,
        ['7']=1,['8']=1,['9']=1,['-']=1,['_']=1,['.']=1,['~']=1,
    };
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 3 < dst_size; i++) {
        unsigned char c = (unsigned char)src[i];
        if (safe[c]) {
            dst[j++] = (char)c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            snprintf(dst + j, dst_size - j, "%%%02X", c);
            j += 3;
        }
    }
    dst[j] = '\0';
}

static inline void lumo_resolve_url(const char *text, char *url, size_t url_size) {
    if (strstr(text, "://") != NULL) {
        snprintf(url, url_size, "%s", text);
    } else if (strstr(text, "localhost") != NULL) {
        snprintf(url, url_size, "http://%s", text);
    } else if (strchr(text, '.') != NULL && strchr(text, ' ') == NULL) {
        snprintf(url, url_size, "https://%s", text);
    } else {
        char encoded[2048];
        lumo_url_encode(text, encoded, sizeof(encoded));
        snprintf(url, url_size,
            "https://duckduckgo.com/?q=%s", encoded);
    }
}

#endif
