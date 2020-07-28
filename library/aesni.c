/*
 *  AES-NI support functions
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */

/*
 * [AES-WP] http://software.intel.com/en-us/articles/intel-advanced-encryption-standard-aes-instructions-set
 * [CLMUL-WP] http://software.intel.com/en-us/articles/intel-carry-less-multiplication-instruction-and-its-usage-for-computing-the-gcm-mode/
 */

#include "common.h"

#if defined(MBEDTLS_AESNI_C)

#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#warning "MBEDTLS_AESNI_C is known to cause spurious error reports with some memory sanitizers as they do not understand the assembly code."
#endif
#endif

#include "mbedtls/aesni.h"

#include <string.h>

#ifndef asm
#define asm __asm
#endif

#if defined(MBEDTLS_HAVE_X86_64)

#if defined(_MSC_VER) && defined(_M_X64)
#define USE_MSVC_X64_INTRINSICS
#include <intrin.h>
#define mm_shuffle_epi64(a, b, ctrl) _mm_castpd_si128( _mm_shuffle_pd( _mm_castsi128_pd( a ), _mm_castsi128_pd( b ), ctrl ) )
#endif

/*
 * AES-NI support detection routine
 */
int mbedtls_aesni_has_support( unsigned int what )
{
    static int done = 0;
    static unsigned int c = 0;

    if( ! done )
    {
#if defined(USE_MSVC_X64_INTRINSICS)
        int regs[4]; // eax, ebx, ecx, edx
        __cpuid( regs, 1 );
        c = regs[2];
#else
        asm( "movl  $1, %%eax   \n\t"
             "cpuid             \n\t"
             : "=c" (c)
             :
             : "eax", "ebx", "edx" );
#endif
        done = 1;
    }

    return( ( c & what ) != 0 );
}

/*
 * Binutils needs to be at least 2.19 to support AES-NI instructions.
 * Unfortunately, a lot of users have a lower version now (2014-04).
 * Emit bytecode directly in order to support "old" version of gas.
 *
 * Opcodes from the Intel architecture reference manual, vol. 3.
 * We always use registers, so we don't need prefixes for memory operands.
 * Operand macros are in gas order (src, dst) as opposed to Intel order
 * (dst, src) in order to blend better into the surrounding assembly code.
 */
#define AESDEC      ".byte 0x66,0x0F,0x38,0xDE,"
#define AESDECLAST  ".byte 0x66,0x0F,0x38,0xDF,"
#define AESENC      ".byte 0x66,0x0F,0x38,0xDC,"
#define AESENCLAST  ".byte 0x66,0x0F,0x38,0xDD,"
#define AESIMC      ".byte 0x66,0x0F,0x38,0xDB,"
#define AESKEYGENA  ".byte 0x66,0x0F,0x3A,0xDF,"
#define PCLMULQDQ   ".byte 0x66,0x0F,0x3A,0x44,"

#define xmm0_xmm0   "0xC0"
#define xmm0_xmm1   "0xC8"
#define xmm0_xmm2   "0xD0"
#define xmm0_xmm3   "0xD8"
#define xmm0_xmm4   "0xE0"
#define xmm1_xmm0   "0xC1"
#define xmm1_xmm2   "0xD1"

/*
 * AES-NI AES-ECB block en(de)cryption
 */
int mbedtls_aesni_crypt_ecb( mbedtls_aes_context *ctx,
                     int mode,
                     const unsigned char input[16],
                     unsigned char output[16] )
{
#if defined(USE_MSVC_X64_INTRINSICS)
    __m128i* rk, a;
    int i;

    rk = (__m128i*)ctx->rk;
    a = _mm_xor_si128( _mm_loadu_si128( (__m128i*)input ), _mm_loadu_si128( rk++ ) );

    if (mode == MBEDTLS_AES_ENCRYPT)
    {
        for (i = ctx->nr - 1; i; --i)
            a = _mm_aesenc_si128( a, _mm_loadu_si128( rk++ ) );
        a = _mm_aesenclast_si128( a, _mm_loadu_si128( rk ) );
    }
    else
    {
        for (i = ctx->nr - 1; i; --i)
            a = _mm_aesdec_si128( a, _mm_loadu_si128( rk++ ) );
        a = _mm_aesdeclast_si128( a, _mm_loadu_si128( rk ) );
    }

    _mm_storeu_si128( (__m128i*)output, a );
#else
    asm( "movdqu    (%3), %%xmm0    \n\t" // load input
         "movdqu    (%1), %%xmm1    \n\t" // load round key 0
         "pxor      %%xmm1, %%xmm0  \n\t" // round 0
         "add       $16, %1         \n\t" // point to next round key
         "subl      $1, %0          \n\t" // normal rounds = nr - 1
         "test      %2, %2          \n\t" // mode?
         "jz        2f              \n\t" // 0 = decrypt

         "1:                        \n\t" // encryption loop
         "movdqu    (%1), %%xmm1    \n\t" // load round key
         AESENC     xmm1_xmm0      "\n\t" // do round
         "add       $16, %1         \n\t" // point to next round key
         "subl      $1, %0          \n\t" // loop
         "jnz       1b              \n\t"
         "movdqu    (%1), %%xmm1    \n\t" // load round key
         AESENCLAST xmm1_xmm0      "\n\t" // last round
         "jmp       3f              \n\t"

         "2:                        \n\t" // decryption loop
         "movdqu    (%1), %%xmm1    \n\t"
         AESDEC     xmm1_xmm0      "\n\t" // do round
         "add       $16, %1         \n\t"
         "subl      $1, %0          \n\t"
         "jnz       2b              \n\t"
         "movdqu    (%1), %%xmm1    \n\t" // load round key
         AESDECLAST xmm1_xmm0      "\n\t" // last round

         "3:                        \n\t"
         "movdqu    %%xmm0, (%4)    \n\t" // export output
         :
         : "r" (ctx->nr), "r" (ctx->rk), "r" (mode), "r" (input), "r" (output)
         : "memory", "cc", "xmm0", "xmm1" );

#endif

    return( 0 );
}

#if defined(USE_MSVC_X64_INTRINSICS)

static inline void clmul256( __m128i a, __m128i b, __m128i* r0, __m128i* r1 )
{
    __m128i c, d, e, f, ef;
    c = _mm_clmulepi64_si128( a, b, 0x00 );
    d = _mm_clmulepi64_si128( a, b, 0x11 );
    e = _mm_clmulepi64_si128( a, b, 0x10 );
    f = _mm_clmulepi64_si128( a, b, 0x01 );

    // r0 = f0^e0^c1:c0 = c1:c0 ^ f0^e0:0
    // r1 = d1:f1^e1^d0 = d1:d0 ^ 0:f1^e1

    ef = _mm_xor_si128( e, f );
    *r0 = _mm_xor_si128( c, _mm_slli_si128( ef, 8 ) );
    *r1 = _mm_xor_si128( d, _mm_srli_si128( ef, 8 ) );
}

static inline void sll256( __m128i a0, __m128i a1, __m128i* s0, __m128i* s1 )
{
    __m128i l0, l1, r0, r1;

    l0 = _mm_slli_epi64( a0, 1 );
    l1 = _mm_slli_epi64( a1, 1 );

    r0 = _mm_srli_epi64( a0, 63 );
    r1 = _mm_srli_epi64( a1, 63 );

    *s0 = _mm_or_si128( l0, _mm_slli_si128( r0, 8 ) );
    *s1 = _mm_or_si128( _mm_or_si128( l1, _mm_srli_si128( r0, 8 ) ), _mm_slli_si128( r1, 8 ) );
}

static inline __m128i reducemod128( __m128i x10, __m128i x32 )
{
    __m128i a, b, c, dx0, e, f, g, h;

    // (1) left shift x0 by 63, 62 and 57
    a = _mm_slli_epi64( x10, 63 );
    b = _mm_slli_epi64( x10, 62 );
    c = _mm_slli_epi64( x10, 57 );

    // (2) compute D xor'ing a, b, c and x1
    // d:x0 = [x1:x0] ^ [a^b^c:0]
    dx0 = _mm_xor_si128( x10, _mm_slli_si128( _mm_xor_si128( _mm_xor_si128( a, b ), c ), 8 ) );

    // (3) right shift [d:x0] by 1, 2, 7
    e = _mm_or_si128( _mm_srli_epi64( dx0, 1 ), _mm_srli_si128( _mm_slli_epi64( dx0, 63 ), 8 ) );
    f = _mm_or_si128( _mm_srli_epi64( dx0, 2 ), _mm_srli_si128( _mm_slli_epi64( dx0, 62 ), 8 ) );
    g = _mm_or_si128( _mm_srli_epi64( dx0, 7 ), _mm_srli_si128( _mm_slli_epi64( dx0, 57 ), 8 ) );

    // (4) compute h = d^e1^f1^g1:x0^e0^f0^g0
    h = _mm_xor_si128( dx0, _mm_xor_si128( e, _mm_xor_si128( f, g ) ) );

    // result is x3^h1:x2^h0
    return _mm_xor_si128( x32, h );
}

#endif

/*
 * GCM multiplication: c = a times b in GF(2^128)
 * Based on [CLMUL-WP] algorithms 1 (with equation 27) and 5.
 */
void mbedtls_aesni_gcm_mult( unsigned char c[16],
                     const unsigned char a[16],
                     const unsigned char b[16] )
{

#if defined(USE_MSVC_X64_INTRINSICS)
    __m128i xa, xb, m0, m1, x10, x32, r;

    xa.m128i_u64[1] = _byteswap_uint64( *((unsigned __int64*)a + 0) );
    xa.m128i_u64[0] = _byteswap_uint64( *((unsigned __int64*)a + 1) );
    xb.m128i_u64[1] = _byteswap_uint64( *((unsigned __int64*)b + 0) );
    xb.m128i_u64[0] = _byteswap_uint64( *((unsigned __int64*)b + 1) );

    clmul256( xa, xb, &m0, &m1 );
    sll256( m0, m1, &x10, &x32 );
    r = reducemod128( x10, x32 );

    *((unsigned __int64*)c + 0) = _byteswap_uint64( r.m128i_u64[1] );
    *((unsigned __int64*)c + 1) = _byteswap_uint64( r.m128i_u64[0] );
#else
    unsigned char aa[16], bb[16], cc[16];
    size_t i;

    /* The inputs are in big-endian order, so byte-reverse them */
    for( i = 0; i < 16; i++ )
    {
        aa[i] = a[15 - i];
        bb[i] = b[15 - i];
    }

    asm( "movdqu (%0), %%xmm0               \n\t" // a1:a0
         "movdqu (%1), %%xmm1               \n\t" // b1:b0

         /*
          * Caryless multiplication xmm2:xmm1 = xmm0 * xmm1
          * using [CLMUL-WP] algorithm 1 (p. 13).
          */
         "movdqa %%xmm1, %%xmm2             \n\t" // copy of b1:b0
         "movdqa %%xmm1, %%xmm3             \n\t" // same
         "movdqa %%xmm1, %%xmm4             \n\t" // same
         PCLMULQDQ xmm0_xmm1 ",0x00         \n\t" // a0*b0 = c1:c0
         PCLMULQDQ xmm0_xmm2 ",0x11         \n\t" // a1*b1 = d1:d0
         PCLMULQDQ xmm0_xmm3 ",0x10         \n\t" // a0*b1 = e1:e0
         PCLMULQDQ xmm0_xmm4 ",0x01         \n\t" // a1*b0 = f1:f0
         "pxor %%xmm3, %%xmm4               \n\t" // e1+f1:e0+f0
         "movdqa %%xmm4, %%xmm3             \n\t" // same
         "psrldq $8, %%xmm4                 \n\t" // 0:e1+f1
         "pslldq $8, %%xmm3                 \n\t" // e0+f0:0
         "pxor %%xmm4, %%xmm2               \n\t" // d1:d0+e1+f1
         "pxor %%xmm3, %%xmm1               \n\t" // c1+e0+f1:c0

         /*
          * Now shift the result one bit to the left,
          * taking advantage of [CLMUL-WP] eq 27 (p. 20)
          */
         "movdqa %%xmm1, %%xmm3             \n\t" // r1:r0
         "movdqa %%xmm2, %%xmm4             \n\t" // r3:r2
         "psllq $1, %%xmm1                  \n\t" // r1<<1:r0<<1
         "psllq $1, %%xmm2                  \n\t" // r3<<1:r2<<1
         "psrlq $63, %%xmm3                 \n\t" // r1>>63:r0>>63
         "psrlq $63, %%xmm4                 \n\t" // r3>>63:r2>>63
         "movdqa %%xmm3, %%xmm5             \n\t" // r1>>63:r0>>63
         "pslldq $8, %%xmm3                 \n\t" // r0>>63:0
         "pslldq $8, %%xmm4                 \n\t" // r2>>63:0
         "psrldq $8, %%xmm5                 \n\t" // 0:r1>>63
         "por %%xmm3, %%xmm1                \n\t" // r1<<1|r0>>63:r0<<1
         "por %%xmm4, %%xmm2                \n\t" // r3<<1|r2>>62:r2<<1
         "por %%xmm5, %%xmm2                \n\t" // r3<<1|r2>>62:r2<<1|r1>>63

         /*
          * Now reduce modulo the GCM polynomial x^128 + x^7 + x^2 + x + 1
          * using [CLMUL-WP] algorithm 5 (p. 20).
          * Currently xmm2:xmm1 holds x3:x2:x1:x0 (already shifted).
          */
         /* Step 2 (1) */
         "movdqa %%xmm1, %%xmm3             \n\t" // x1:x0
         "movdqa %%xmm1, %%xmm4             \n\t" // same
         "movdqa %%xmm1, %%xmm5             \n\t" // same
         "psllq $63, %%xmm3                 \n\t" // x1<<63:x0<<63 = stuff:a
         "psllq $62, %%xmm4                 \n\t" // x1<<62:x0<<62 = stuff:b
         "psllq $57, %%xmm5                 \n\t" // x1<<57:x0<<57 = stuff:c

         /* Step 2 (2) */
         "pxor %%xmm4, %%xmm3               \n\t" // stuff:a+b
         "pxor %%xmm5, %%xmm3               \n\t" // stuff:a+b+c
         "pslldq $8, %%xmm3                 \n\t" // a+b+c:0
         "pxor %%xmm3, %%xmm1               \n\t" // x1+a+b+c:x0 = d:x0

         /* Steps 3 and 4 */
         "movdqa %%xmm1,%%xmm0              \n\t" // d:x0
         "movdqa %%xmm1,%%xmm4              \n\t" // same
         "movdqa %%xmm1,%%xmm5              \n\t" // same
         "psrlq $1, %%xmm0                  \n\t" // e1:x0>>1 = e1:e0'
         "psrlq $2, %%xmm4                  \n\t" // f1:x0>>2 = f1:f0'
         "psrlq $7, %%xmm5                  \n\t" // g1:x0>>7 = g1:g0'
         "pxor %%xmm4, %%xmm0               \n\t" // e1+f1:e0'+f0'
         "pxor %%xmm5, %%xmm0               \n\t" // e1+f1+g1:e0'+f0'+g0'
         // e0'+f0'+g0' is almost e0+f0+g0, ex\tcept for some missing
         // bits carried from d. Now get those\t bits back in.
         "movdqa %%xmm1,%%xmm3              \n\t" // d:x0
         "movdqa %%xmm1,%%xmm4              \n\t" // same
         "movdqa %%xmm1,%%xmm5              \n\t" // same
         "psllq $63, %%xmm3                 \n\t" // d<<63:stuff
         "psllq $62, %%xmm4                 \n\t" // d<<62:stuff
         "psllq $57, %%xmm5                 \n\t" // d<<57:stuff
         "pxor %%xmm4, %%xmm3               \n\t" // d<<63+d<<62:stuff
         "pxor %%xmm5, %%xmm3               \n\t" // missing bits of d:stuff
         "psrldq $8, %%xmm3                 \n\t" // 0:missing bits of d
         "pxor %%xmm3, %%xmm0               \n\t" // e1+f1+g1:e0+f0+g0
         "pxor %%xmm1, %%xmm0               \n\t" // h1:h0
         "pxor %%xmm2, %%xmm0               \n\t" // x3+h1:x2+h0

         "movdqu %%xmm0, (%2)               \n\t" // done
         :
         : "r" (aa), "r" (bb), "r" (cc)
         : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5" );

    /* Now byte-reverse the outputs */
    for( i = 0; i < 16; i++ )
        c[i] = cc[15 - i];
#endif

    return;
}

/*
 * Compute decryption round keys from encryption round keys
 */
void mbedtls_aesni_inverse_key( unsigned char *invkey,
                        const unsigned char *fwdkey, int nr )
{
    unsigned char *ik = invkey;
    const unsigned char *fk = fwdkey + 16 * nr;

    memcpy( ik, fk, 16 );

    for( fk -= 16, ik += 16; fk > fwdkey; fk -= 16, ik += 16 )
#if defined(USE_MSVC_X64_INTRINSICS)
        _mm_storeu_si128( (__m128i*)ik, _mm_aesimc_si128( _mm_loadu_si128( (__m128i*)fk) ) );
#else
        asm( "movdqu (%0), %%xmm0       \n\t"
             AESIMC  xmm0_xmm0         "\n\t"
             "movdqu %%xmm0, (%1)       \n\t"
             :
             : "r" (fk), "r" (ik)
             : "memory", "xmm0" );
#endif

    memcpy( ik, fk, 16 );
}

#if defined(USE_MSVC_X64_INTRINSICS)

// [AES-WP] Part of Fig. 24 (p. 25)
inline static __m128i aes_key_128_assist( __m128i temp1, __m128i kg )
{
    __m128i temp3;
    temp3 = _mm_slli_si128( temp1, 0x4 );
    temp1 = _mm_xor_si128( temp1, temp3 );
    temp3 = _mm_slli_si128( temp3, 0x4 );
    temp1 = _mm_xor_si128( temp1, temp3 );
    temp3 = _mm_slli_si128( temp3, 0x4 );
    temp1 = _mm_xor_si128( temp1, temp3 );
    temp1 = _mm_xor_si128( temp1, _mm_shuffle_epi32( kg, 0xff ) );
    return temp1;
}

// [AES-WP] Part of Fig. 25 (p. 26)
inline void aes_key_192_assist( __m128i* temp1, __m128i * temp3, __m128i kg )
{
    __m128i temp4;
    temp4 = _mm_slli_si128( *temp1, 0x4 );
    *temp1 = _mm_xor_si128( *temp1, temp4 );
    temp4 = _mm_slli_si128( temp4, 0x4 );
    *temp1 = _mm_xor_si128( *temp1, temp4 );
    temp4 = _mm_slli_si128( temp4, 0x4 );
    *temp1 = _mm_xor_si128( *temp1, temp4 );
    *temp1 = _mm_xor_si128( *temp1, _mm_shuffle_epi32( kg, 0x55 ) );
    temp4 = _mm_slli_si128( *temp3, 0x4 );
    *temp3 = _mm_xor_si128( *temp3, temp4 );
    *temp3 = _mm_xor_si128( *temp3, _mm_shuffle_epi32( *temp1, 0xff ) );
}

// [AES-WP] Part of Fig. 26 (p. 27)
inline static void aes_key_256_assist_1( __m128i* temp1, __m128i kg )
{
    __m128i temp4;
    temp4 = _mm_slli_si128( *temp1, 0x4 );
    *temp1 = _mm_xor_si128( *temp1, temp4 );
    temp4 = _mm_slli_si128( temp4, 0x4 );
    *temp1 = _mm_xor_si128( *temp1, temp4 );
    temp4 = _mm_slli_si128( temp4, 0x4 );
    *temp1 = _mm_xor_si128( *temp1, temp4 );
    *temp1 = _mm_xor_si128( *temp1, _mm_shuffle_epi32( kg, 0xff ) );
}

inline static void aes_key_256_assist_2( __m128i* temp1, __m128i* temp3 )
{
    __m128i temp2, temp4;
    temp4 = _mm_aeskeygenassist_si128( *temp1, 0x0 );
    temp2 = _mm_shuffle_epi32( temp4, 0xaa );
    temp4 = _mm_slli_si128( *temp3, 0x4 );
    *temp3 = _mm_xor_si128( *temp3, temp4 );
    temp4 = _mm_slli_si128( temp4, 0x4 );
    *temp3 = _mm_xor_si128( *temp3, temp4 );
    temp4 = _mm_slli_si128( temp4, 0x4 );
    *temp3 = _mm_xor_si128( *temp3, temp4 );
    *temp3 = _mm_xor_si128( *temp3, temp2 );
}
#endif /* USE_MSVC_X64_INTRINSICS */

/*
 * Key expansion, 128-bit case
 */
static void aesni_setkey_enc_128( unsigned char *rk,
                                  const unsigned char *key )
{
#if defined(USE_MSVC_X64_INTRINSICS)

    __m128i temp;

#define EXPAND_ROUND(index, rcon) \
        _mm_storeu_si128( (__m128i*)rk + index, temp ); \
        temp = aes_key_128_assist( temp, _mm_aeskeygenassist_si128( temp, rcon ) )

    temp = _mm_loadu_si128( (__m128i*)key );
    EXPAND_ROUND( 0, 0x01 );
    EXPAND_ROUND( 1, 0x02 );
    EXPAND_ROUND( 2, 0x04 );
    EXPAND_ROUND( 3, 0x08 );
    EXPAND_ROUND( 4, 0x10 );
    EXPAND_ROUND( 5, 0x20 );
    EXPAND_ROUND( 6, 0x40 );
    EXPAND_ROUND( 7, 0x80 );
    EXPAND_ROUND( 8, 0x1b );
    EXPAND_ROUND( 9, 0x36 );
    _mm_storeu_si128( (__m128i*)rk + 10, temp );

#undef EXPAND_ROUND

#else

    asm( "movdqu (%1), %%xmm0               \n\t" // copy the original key
         "movdqu %%xmm0, (%0)               \n\t" // as round key 0
         "jmp 2f                            \n\t" // skip auxiliary routine

         /*
          * Finish generating the next round key.
          *
          * On entry xmm0 is r3:r2:r1:r0 and xmm1 is X:stuff:stuff:stuff
          * with X = rot( sub( r3 ) ) ^ RCON.
          *
          * On exit, xmm0 is r7:r6:r5:r4
          * with r4 = X + r0, r5 = r4 + r1, r6 = r5 + r2, r7 = r6 + r3
          * and those are written to the round key buffer.
          */
         "1:                                \n\t"
         "pshufd $0xff, %%xmm1, %%xmm1      \n\t" // X:X:X:X
         "pxor %%xmm0, %%xmm1               \n\t" // X+r3:X+r2:X+r1:r4
         "pslldq $4, %%xmm0                 \n\t" // r2:r1:r0:0
         "pxor %%xmm0, %%xmm1               \n\t" // X+r3+r2:X+r2+r1:r5:r4
         "pslldq $4, %%xmm0                 \n\t" // etc
         "pxor %%xmm0, %%xmm1               \n\t"
         "pslldq $4, %%xmm0                 \n\t"
         "pxor %%xmm1, %%xmm0               \n\t" // update xmm0 for next time!
         "add $16, %0                       \n\t" // point to next round key
         "movdqu %%xmm0, (%0)               \n\t" // write it
         "ret                               \n\t"

         /* Main "loop" */
         "2:                                \n\t"
         AESKEYGENA xmm0_xmm1 ",0x01        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x02        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x04        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x08        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x10        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x20        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x40        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x80        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x1B        \n\tcall 1b \n\t"
         AESKEYGENA xmm0_xmm1 ",0x36        \n\tcall 1b \n\t"
         :
         : "r" (rk), "r" (key)
         : "memory", "cc", "0" );
#endif /* USE_MSVC_X64_INTRINSICS */
}

/*
 * Key expansion, 192-bit case
 */
static void aesni_setkey_enc_192( unsigned char *rk,
                                  const unsigned char *key )
{
#if defined(USE_MSVC_X64_INTRINSICS)
    __m128i temp1, temp2, temp3;
    __m128i *key_schedule = (__m128i*)rk;

#define EXPAND_ROUND(index, rcon1, rcon2) \
    _mm_storeu_si128( key_schedule + index + 0, temp1 ); \
    temp2 = temp3; \
    aes_key_192_assist( &temp1, &temp3, _mm_aeskeygenassist_si128( temp3, rcon1 ) ); \
    _mm_storeu_si128( key_schedule + index + 1, mm_shuffle_epi64( temp2, temp1, 0 ) ); \
    _mm_storeu_si128( key_schedule + index + 2, mm_shuffle_epi64( temp1, temp3, 1 ) ); \
    aes_key_192_assist( &temp1, &temp3, _mm_aeskeygenassist_si128( temp3, rcon2 ) )

    temp1 = _mm_loadu_si128( (__m128i*)key );
    temp3 = _mm_loadu_si128( (__m128i*)(key + 16) );

    EXPAND_ROUND( 0, 0x01, 0x02 );
    EXPAND_ROUND( 3, 0x04, 0x08 );
    EXPAND_ROUND( 6, 0x10, 0x20 );
    EXPAND_ROUND( 9, 0x40, 0x80 );

    _mm_storeu_si128( key_schedule + 12, temp1 );

#undef EXPAND_ROUND

#else
    asm( "movdqu (%1), %%xmm0   \n\t" // copy original round key
         "movdqu %%xmm0, (%0)   \n\t"
         "add $16, %0           \n\t"
         "movq 16(%1), %%xmm1   \n\t"
         "movq %%xmm1, (%0)     \n\t"
         "add $8, %0            \n\t"
         "jmp 2f                \n\t" // skip auxiliary routine

         /*
          * Finish generating the next 6 quarter-keys.
          *
          * On entry xmm0 is r3:r2:r1:r0, xmm1 is stuff:stuff:r5:r4
          * and xmm2 is stuff:stuff:X:stuff with X = rot( sub( r3 ) ) ^ RCON.
          *
          * On exit, xmm0 is r9:r8:r7:r6 and xmm1 is stuff:stuff:r11:r10
          * and those are written to the round key buffer.
          */
         "1:                            \n\t"
         "pshufd $0x55, %%xmm2, %%xmm2  \n\t" // X:X:X:X
         "pxor %%xmm0, %%xmm2           \n\t" // X+r3:X+r2:X+r1:r4
         "pslldq $4, %%xmm0             \n\t" // etc
         "pxor %%xmm0, %%xmm2           \n\t"
         "pslldq $4, %%xmm0             \n\t"
         "pxor %%xmm0, %%xmm2           \n\t"
         "pslldq $4, %%xmm0             \n\t"
         "pxor %%xmm2, %%xmm0           \n\t" // update xmm0 = r9:r8:r7:r6
         "movdqu %%xmm0, (%0)           \n\t"
         "add $16, %0                   \n\t"
         "pshufd $0xff, %%xmm0, %%xmm2  \n\t" // r9:r9:r9:r9
         "pxor %%xmm1, %%xmm2           \n\t" // stuff:stuff:r9+r5:r10
         "pslldq $4, %%xmm1             \n\t" // r2:r1:r0:0
         "pxor %%xmm2, %%xmm1           \n\t" // xmm1 = stuff:stuff:r11:r10
         "movq %%xmm1, (%0)             \n\t"
         "add $8, %0                    \n\t"
         "ret                           \n\t"

         "2:                            \n\t"
         AESKEYGENA xmm1_xmm2 ",0x01    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x02    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x04    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x08    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x10    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x20    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x40    \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x80    \n\tcall 1b \n\t"

         :
         : "r" (rk), "r" (key)
         : "memory", "cc", "0" );
#endif /* USE_MSVC_X64_INTRINSICS */
}

/*
 * Key expansion, 256-bit case
 */
static void aesni_setkey_enc_256( unsigned char *rk,
                                  const unsigned char *key )
{
#if defined(USE_MSVC_X64_INTRINSICS)
    __m128i temp1, temp3;
    __m128i *key_schedule = (__m128i*)rk;

#define EXPAND_ROUND1(index, rcon) \
    aes_key_256_assist_1( &temp1, _mm_aeskeygenassist_si128( temp3, rcon ) ); \
    _mm_storeu_si128( key_schedule + index, temp1 )

#define EXPAND_ROUND2(index) \
    aes_key_256_assist_2( &temp1, &temp3 ); \
    _mm_storeu_si128( key_schedule + index, temp3 )

    temp1 = _mm_loadu_si128( (__m128i*)key );
    temp3 = _mm_loadu_si128( (__m128i*)(key + 16) );

    _mm_storeu_si128( key_schedule + 0, temp1 );
    _mm_storeu_si128( key_schedule + 1, temp3 );
    EXPAND_ROUND1( 2, 0x01 );
    EXPAND_ROUND2( 3 );
    EXPAND_ROUND1( 4, 0x02 );
    EXPAND_ROUND2( 5 );
    EXPAND_ROUND1( 6, 0x04 );
    EXPAND_ROUND2( 7 );
    EXPAND_ROUND1( 8, 0x08 );
    EXPAND_ROUND2( 9 );
    EXPAND_ROUND1( 10, 0x10 );
    EXPAND_ROUND2( 11 );
    EXPAND_ROUND1( 12, 0x20 );
    EXPAND_ROUND2( 13 );
    EXPAND_ROUND1( 14, 0x40 );

#undef EXPAND_ROUND1
#undef EXPAND_ROUND2

#else
    asm( "movdqu (%1), %%xmm0           \n\t"
         "movdqu %%xmm0, (%0)           \n\t"
         "add $16, %0                   \n\t"
         "movdqu 16(%1), %%xmm1         \n\t"
         "movdqu %%xmm1, (%0)           \n\t"
         "jmp 2f                        \n\t" // skip auxiliary routine

         /*
          * Finish generating the next two round keys.
          *
          * On entry xmm0 is r3:r2:r1:r0, xmm1 is r7:r6:r5:r4 and
          * xmm2 is X:stuff:stuff:stuff with X = rot( sub( r7 )) ^ RCON
          *
          * On exit, xmm0 is r11:r10:r9:r8 and xmm1 is r15:r14:r13:r12
          * and those have been written to the output buffer.
          */
         "1:                                \n\t"
         "pshufd $0xff, %%xmm2, %%xmm2      \n\t"
         "pxor %%xmm0, %%xmm2               \n\t"
         "pslldq $4, %%xmm0                 \n\t"
         "pxor %%xmm0, %%xmm2               \n\t"
         "pslldq $4, %%xmm0                 \n\t"
         "pxor %%xmm0, %%xmm2               \n\t"
         "pslldq $4, %%xmm0                 \n\t"
         "pxor %%xmm2, %%xmm0               \n\t"
         "add $16, %0                       \n\t"
         "movdqu %%xmm0, (%0)               \n\t"

         /* Set xmm2 to stuff:Y:stuff:stuff with Y = subword( r11 )
          * and proceed to generate next round key from there */
         AESKEYGENA xmm0_xmm2 ",0x00        \n\t"
         "pshufd $0xaa, %%xmm2, %%xmm2      \n\t"
         "pxor %%xmm1, %%xmm2               \n\t"
         "pslldq $4, %%xmm1                 \n\t"
         "pxor %%xmm1, %%xmm2               \n\t"
         "pslldq $4, %%xmm1                 \n\t"
         "pxor %%xmm1, %%xmm2               \n\t"
         "pslldq $4, %%xmm1                 \n\t"
         "pxor %%xmm2, %%xmm1               \n\t"
         "add $16, %0                       \n\t"
         "movdqu %%xmm1, (%0)               \n\t"
         "ret                               \n\t"

         /*
          * Main "loop" - Generating one more key than necessary,
          * see definition of mbedtls_aes_context.buf
          */
         "2:                                \n\t"
         AESKEYGENA xmm1_xmm2 ",0x01        \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x02        \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x04        \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x08        \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x10        \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x20        \n\tcall 1b \n\t"
         AESKEYGENA xmm1_xmm2 ",0x40        \n\tcall 1b \n\t"
         :
         : "r" (rk), "r" (key)
         : "memory", "cc", "0" );
#endif /* USE_MSVC_X64_INTRINSICS */
}

/*
 * Key expansion, wrapper
 */
int mbedtls_aesni_setkey_enc( unsigned char *rk,
                      const unsigned char *key,
                      size_t bits )
{
    switch( bits )
    {
        case 128: aesni_setkey_enc_128( rk, key ); break;
        case 192: aesni_setkey_enc_192( rk, key ); break;
        case 256: aesni_setkey_enc_256( rk, key ); break;
        default : return( MBEDTLS_ERR_AES_INVALID_KEY_LENGTH );
    }

    return( 0 );
}

#endif /* MBEDTLS_HAVE_X86_64 */

#endif /* MBEDTLS_AESNI_C */
