/*
 * zlite:
 *  light-weight lossless data compression utility.
 *
 * Copyright (C) 2012-2013 by Zhang Li <RichSelian at gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__MINGW32__) || defined(__MINGW64__)
#include <fcntl.h>
#include <io.h>

int ma_n(int argc, char** argv);
int main(int argc, char** argv) {
    setmode(fileno(stdin),  O_BINARY);
    setmode(fileno(stdout), O_BINARY);
    return ma_n(argc, argv);
}
#define main ma_n
#endif

#define __cache_aligned(name) __attribute__((aligned(64))) name

/*******************************************************************************
 * POLAR Coder
 ******************************************************************************/
#define POLAR_SYMBOLS   512 /* should be even */
#define POLAR_MAXLEN    15  /* should be less than 16, so we can pack two length values into a byte */

#define M_round_down(x)     while((x)&(-(x)^(x))) { (x) &= (-(x)^(x)); }
#define M_round_up(x)       while((x)&(-(x)^(x))) { (x) &= (-(x)^(x)); } (x) <<= 1;
#define M_int_swap(x, y)    {int (_)=(x); (x)=(y); (y)=(_);}

int polar_make_leng_table(const int* freq_table, int* leng_table) {
    int symbols[POLAR_SYMBOLS];
    int i;
    int s;
    int total;
    int shift = 0;

    memcpy(leng_table, freq_table, POLAR_SYMBOLS * sizeof(int));

MakeTablePass:
    /* sort symbols */
    for(i = 0; i < POLAR_SYMBOLS; i++) {
        symbols[i] = i;
    }
    for(i = 0; i < POLAR_SYMBOLS; i++) {
        if(i > 0 && leng_table[symbols[i - 1]] < leng_table[symbols[i]]) {
            M_int_swap(symbols[i - 1], symbols[i]);
            i -= 2;
        }
    }

    /* calculate total frequency */
    total = 0;
    for(i = 0; i < POLAR_SYMBOLS; i++) {
        total += leng_table[i];
    }

    /* run */
    M_round_up(total);
    s = 0;
    for(i = 0; i < POLAR_SYMBOLS; i++) {
        M_round_down(leng_table[i]);
        s += leng_table[i];
    }
    while(s < total) {
        for(i = 0; i < POLAR_SYMBOLS; i++) {
            if(s + leng_table[symbols[i]] <= total) {
                s += leng_table[symbols[i]];
                leng_table[symbols[i]] *= 2;
            }
        }
    }

    /* get code length */
    for(i = 0; i < POLAR_SYMBOLS; i++) {
        s = 2;
        if(leng_table[i] > 0) {
            while((total / leng_table[i]) >> s != 0) {
                s += 1;
            }
            leng_table[i] = s - 1;
        } else {
            leng_table[i] = 0;
        }

        /* code length too long -- scale and rebuild table */
        if(leng_table[i] > POLAR_MAXLEN) {
            shift += 1;
            for(i = 0; i < POLAR_SYMBOLS; i++) {
                if((leng_table[i] = freq_table[i] >> shift) == 0 && freq_table[i] > 0) {
                    leng_table[i] = 1;
                }
            }
            goto MakeTablePass;
        }
    }
    return 0;
}

int polar_make_code_table(const int* leng_table, int* code_table) {
    int i;
    int s;
    int t1;
    int t2;
    int code = 0;

    memset(code_table, 0, POLAR_SYMBOLS * sizeof(int));

    /* make code for each symbol */
    for(s = 1; s <= POLAR_MAXLEN; s++) {
        for(i = 0; i < POLAR_SYMBOLS; i++) {
            if(leng_table[i] == s) {
                code_table[i] = code;
                code += 1;
            }
        }
        code *= 2;
    }

    /* reverse each code */
    for(i = 0; i < POLAR_SYMBOLS; i++) {
        t1 = 0;
        t2 = leng_table[i] - 1;
        while(t1 < t2) {
            code_table[i] ^= (1 & (code_table[i] >> t1)) << t2;
            code_table[i] ^= (1 & (code_table[i] >> t2)) << t1;
            code_table[i] ^= (1 & (code_table[i] >> t1)) << t2;
            t1++;
            t2--;
        }
    }
    return 0;
}

int polar_make_decode_table(const int* leng_table, const int* code_table, int* decode_table) {
    int i;
    int c;

    for(c = 0; c < POLAR_SYMBOLS; c++) {
        if(leng_table[c] > 0) {
            for(i = 0; i + code_table[c] < 65536; i += (1 << leng_table[c])) {
                decode_table[i + code_table[c]] = c;
            }
        }
    }
    return 0;
}

/*******************************************************************************
 * ROLZ
 ******************************************************************************/
#define ROLZ_BUCKET_SIZE    65536
#define MATCH_IDX_SIZE      15  /* make element of rolz_table[] 64 Bytes */
#define MATCH_LEN_MIN       2
#define MATCH_LEN_MAX       17  /* MATCH_LEN_MAX < MATCH_LEN_MIN + (POLAR_SYMBOLS-256) / MATCH_IDX_SIZE */

static unsigned short lastword = 0;
static unsigned short context = 0;

static const unsigned char __cache_aligned(mod15_table)[] = { /* MATCH_IDX_SIZE=15 */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
};
#define M_rolz_item(x, n) rolz_table[(x)].m_item[mod15_table[rolz_table[(x)].m_head + (n)]]

static struct {
    unsigned int m_item[MATCH_IDX_SIZE];
    unsigned int m_head;
}__cache_aligned(rolz_table)[ROLZ_BUCKET_SIZE];

static inline void rolz_update_context(unsigned char* buf, int pos, int cache) {
    rolz_table[context].m_head = mod15_table[rolz_table[context].m_head + MATCH_IDX_SIZE - 1];

    rolz_table[context].m_item[rolz_table[context].m_head] = cache ?
        pos | (buf[pos] << 24) :
        pos;
    context = lastword * 13131 + buf[pos];
    lastword <<= 8;
    lastword |= buf[pos];
    return;
}

int rolz_encode(unsigned char* ibuf, unsigned short* obuf, int ilen) {
    int olen = 0;
    int pos = 0;
    int i;
    int j;
    int match_idx;
    int match_len;
    unsigned int item;
    unsigned int item_chr;
    unsigned int item_pos;

    memset(rolz_table, 0, sizeof(rolz_table));
    context = 0;
    lastword = 0;

    while(pos < ilen) {
        match_len = MATCH_LEN_MIN - 1;
        match_idx = -1;
        if(pos + MATCH_LEN_MAX < ilen) { /* find match */
            for(i = 0; i < MATCH_IDX_SIZE; i++) {
                item = M_rolz_item(context, i);
                item_chr = item >> 24;
                item_pos = item & 0xffffff;

                if(item_pos != 0 && item_chr == ibuf[pos]) {
                    for(j = 1; j < MATCH_LEN_MAX; j++) {
                        if(ibuf[pos + j] != ibuf[item_pos + j]) {
                            break;
                        }
                    }

                    if(j > match_len) {
                        match_len = j;
                        match_idx = i;
                        if(match_len == MATCH_LEN_MAX) { /* no need to find longer match */
                            break;
                        }
                    }
                }
            }
        }
        if(match_len < MATCH_LEN_MIN) {
            match_len = 1;
            match_idx = -1;
        }

        if(match_idx == -1) { /* encode */
            obuf[olen++] = ibuf[pos];
        } else {
            obuf[olen++] = 256 + (match_len - MATCH_LEN_MIN) * MATCH_IDX_SIZE + match_idx;
        }

        for(i = 0; i < match_len; i++) { /* update context */
            rolz_update_context(ibuf, pos++, 1);
        }
    }
    return olen;
}

int rolz_decode(unsigned short* ibuf, unsigned char* obuf, int ilen) {
    int olen = 0;
    int pos = 0;
    int match_idx;
    int match_len;
    int match_offset;

    context = 0;
    lastword = 0;

    for(pos = 0; pos < ilen; pos++) {
        if(ibuf[pos] < 256) { /* process a literal byte */
            obuf[olen] = ibuf[pos];
            rolz_update_context(obuf, olen++, 0);
            continue;
        }

        /* process a match */
        match_idx = (ibuf[pos] - 256) % MATCH_IDX_SIZE;
        match_len = (ibuf[pos] - 256) / MATCH_IDX_SIZE + MATCH_LEN_MIN;
        match_offset = olen - M_rolz_item(context, match_idx);

        while((match_len--) > 0) {
            obuf[olen] = obuf[olen - match_offset];
            rolz_update_context(obuf, olen++, 0);
        }
    }
    return olen;
}

/*******************************************************************************
 * MAIN
 ******************************************************************************/
clock_t clock_start;

void print_result(size_t size_src, size_t size_dst, int encode) {
    fprintf(stderr, (encode ?
                "%u => %u, time=%.2f sec\n" :
                "%u <= %u, time=%.2f sec\n"),
            size_src,
            size_dst,
            (clock() - clock_start) / (double)CLOCKS_PER_SEC);
    return;
}

#define BLOCK_SIZE_IN  16777216
#define BLOCK_SIZE_OUT 18000000

int main(int argc, char** argv) {
    static unsigned char  ibuf[BLOCK_SIZE_IN];
    static unsigned short rbuf[BLOCK_SIZE_IN];
    static unsigned char  obuf[BLOCK_SIZE_OUT];
    size_t size_src = 0;
    size_t size_dst = 0;
    int ilen;
    int rlen;
    int olen;
    int rpos;
    int opos;
    int i;
    int freq_table[POLAR_SYMBOLS];
    int leng_table[POLAR_SYMBOLS];
    int code_table[POLAR_SYMBOLS];
    int decode_table[1 << (POLAR_MAXLEN + 1)];
    int code_buf;
    int code_len;

    clock_start = clock();

    if(argc == 2 && strcmp(argv[1], "e") == 0) {
        while((ilen = fread(ibuf, 1, sizeof(ibuf), stdin)) > 0) {
            rlen = rolz_encode(ibuf, rbuf, ilen);
            olen = 0;

            memset(freq_table, 0, sizeof(freq_table));
            code_buf = 0;
            code_len = 0;

            for(i = 0; i < rlen; i++) {
                freq_table[rbuf[i]] += 1;
            }
            polar_make_leng_table(freq_table, leng_table);
            polar_make_code_table(leng_table, code_table);

            /* write length table */
            for(i = 0; i < POLAR_SYMBOLS; i += 2) {
                obuf[olen++] = leng_table[i] * 16 + leng_table[i + 1];
            }

            /* encode */
            for(i = 0; i < rlen; i++) {
                code_buf += code_table[rbuf[i]] << code_len;
                code_len += leng_table[rbuf[i]];
                while(code_len > 8) {
                    obuf[olen++] = code_buf % 256;
                    code_buf /= 256;
                    code_len -= 8;
                }
            }
            if(code_len > 0) {
                obuf[olen++] = code_buf;
                code_buf = 0;
                code_len = 0;
            }
            fwrite(&rlen, sizeof(rlen), 1, stdout);
            fwrite(&olen, sizeof(olen), 1, stdout);
            fwrite(obuf, 1, olen, stdout);

            size_src += ilen;
            size_dst += olen + sizeof(rlen) + sizeof(olen);
        }
        print_result(size_src, size_dst, 1);
        return 0;
    }

    if(argc == 2 && strcmp(argv[1], "d") == 0) {
        while(fread(&rlen, sizeof(rlen), 1, stdin) == 1 && fread(&olen, sizeof(olen), 1, stdin) == 1) {
            olen = fread(obuf, 1, olen, stdin);
            rpos = 0;
            opos = 0;
            code_buf = 0;
            code_len = 0;

            /* read length table */
            for(i = 0; i < POLAR_SYMBOLS; i += 2) {
                leng_table[i] =     obuf[opos] / 16;
                leng_table[i + 1] = obuf[opos] % 16;
                opos++;
            }

            /* decode */
            polar_make_code_table(leng_table, code_table);
            polar_make_decode_table(leng_table, code_table, decode_table);

            while(rpos < rlen) {
                while(opos < olen && code_len < POLAR_MAXLEN) {
                    code_buf += obuf[opos++] << code_len;
                    code_len += 8;
                }
                i = decode_table[code_buf % 65536];

                rbuf[rpos++] = i;
                code_buf >>= leng_table[i];
                code_len -=  leng_table[i];
            }

            ilen = rolz_decode(rbuf, ibuf, rlen);
            fwrite(ibuf, 1, ilen, stdout);

            size_src += ilen;
            size_dst += olen + sizeof(rlen) + sizeof(olen);
        }
        print_result(size_src, size_dst, 0);
        return 0;
    }

    fprintf(stderr, "zlite:\n");
    fprintf(stderr, "   light-weight lossless data compression utility.\n");
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "   zlite e (from-stdin) (to-stdout)\n");
    fprintf(stderr, "   zlite d (from-stdin) (to-stdout)\n");
    return -1;
}
