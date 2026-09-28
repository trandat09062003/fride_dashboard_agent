#pragma once
#include <Arduino.h>

// Convert Vietnamese UTF-8 text to clean ASCII to prevent LVGL tofu rectangle boxes
inline void strip_vietnamese_diacritics(const char *src, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) return;
    size_t d = 0;
    const unsigned char *s = (const unsigned char *)src;

    while (*s && d + 1 < dst_len) {
        unsigned char b0 = s[0];
        if (b0 < 0x80) {
            dst[d++] = (char)b0;
            s++;
        } else if (b0 == 0xC3) {
            unsigned char b1 = s[1];
            if (!b1) break;
            char repl = '?';
            switch (b1) {
                case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: repl = 'a'; break;
                case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: repl = 'A'; break;
                case 0xA8: case 0xA9: case 0xAA: case 0xAB: repl = 'e'; break;
                case 0x88: case 0x89: case 0x8A: case 0x8B: repl = 'E'; break;
                case 0xAC: case 0xAD: case 0xAE: case 0xAF: repl = 'i'; break;
                case 0x8C: case 0x8D: case 0x8E: case 0x8F: repl = 'I'; break;
                case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB8: repl = 'o'; break;
                case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x98: repl = 'O'; break;
                case 0xB9: case 0xBA: case 0xBB: case 0xBC: repl = 'u'; break;
                case 0x99: case 0x9A: case 0x9B: case 0x9C: repl = 'U'; break;
                case 0xBD: repl = 'y'; break;
                case 0x9D: repl = 'Y'; break;
                case 0x90: case 0x91: repl = 'd'; break;
                case 0xB0: repl = ' '; break; // degree sign
                default: repl = ' '; break;
            }
            dst[d++] = repl;
            s += 2;
        } else if (b0 == 0xC4 || b0 == 0xC5) {
            unsigned char b1 = s[1];
            if (!b1) break;
            char repl = 'a';
            if (b0 == 0xC4 && (b1 == 0x90 || b1 == 0x91)) repl = (b1 == 0x90) ? 'D' : 'd';
            else if (b0 == 0xC4 && (b1 >= 0x82 && b1 <= 0x85)) repl = 'a';
            else if (b0 == 0xC4 && (b1 >= 0x92 && b1 <= 0x9B)) repl = 'e';
            else if (b0 == 0xC5 && (b1 >= 0xA8 && b1 <= 0xAF)) repl = 'u';
            else if (b0 == 0xC5 && (b1 >= 0xA0 && b1 <= 0xA5)) repl = 'o';
            dst[d++] = repl;
            s += 2;
        } else if (b0 == 0xE1) {
            unsigned char b1 = s[1];
            unsigned char b2 = s[2];
            if (!b1 || !b2) break;
            char repl = 'a';
            if (b1 == 0xBA || b1 == 0xBB) {
                if (b1 == 0xBA) {
                    if (b2 < 0x9A) repl = (b2 % 2 == 0) ? 'A' : 'a';
                    else if (b2 < 0xA6) repl = (b2 % 2 == 0) ? 'A' : 'a';
                    else if (b2 < 0xBC) repl = (b2 % 2 == 0) ? 'E' : 'e';
                    else repl = (b2 % 2 == 0) ? 'I' : 'i';
                } else {
                    if (b2 < 0x98) repl = (b2 % 2 == 0) ? 'O' : 'o';
                    else if (b2 < 0xB8) repl = (b2 % 2 == 0) ? 'U' : 'u';
                    else repl = (b2 % 2 == 0) ? 'Y' : 'y';
                }
            }
            dst[d++] = repl;
            s += 3;
        } else {
            // Skip other multi-byte characters cleanly
            s++;
        }
    }
    dst[d] = '\0';
}
