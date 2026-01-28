//
// Group5
// A 1-bpp image decoder
//
// Written by Larry Bank (bitbank@pobox.com)
//
// SPDX-FileCopyrightText: 2024 BitBank Software, Inc.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "Group5.h"

/*
 The code tree that follows has: bit_length, decode routine
 These codes are for Group 4 (MMR) decoding

 01 = vertneg1, 11h = vert1, 20h = horiz, 30h = pass, 12h = vert2
 02 = vertneg2, 13h = vert3, 03 = vertneg3, 90h = trash
*/

static const uint8_t code_table[128]  =
        {0x90, 0, 0x40, 0,       /* trash, uncompr mode - codes 0 and 1 */
         3, 7,                   /* V(-3) pos = 2 */
         0x13, 7,                /* V(3)  pos = 3 */
         2, 6, 2, 6,             /* V(-2) pos = 4,5 */
         0x12, 6, 0x12, 6,       /* V(2)  pos = 6,7 */
         0x30, 4, 0x30, 4, 0x30, 4, 0x30, 4,    /* pass  pos = 8->F */
         0x30, 4, 0x30, 4, 0x30, 4, 0x30, 4,
         0x20, 3, 0x20, 3, 0x20, 3, 0x20, 3,    /* horiz pos = 10->1F */
         0x20, 3, 0x20, 3, 0x20, 3, 0x20, 3,
         0x20, 3, 0x20, 3, 0x20, 3, 0x20, 3,
         0x20, 3, 0x20, 3, 0x20, 3, 0x20, 3,
/* V(-1) pos = 20->2F */
         1, 3, 1, 3, 1, 3, 1, 3,
         1, 3, 1, 3, 1, 3, 1, 3,
         1, 3, 1, 3, 1, 3, 1, 3,
         1, 3, 1, 3, 1, 3, 1, 3,
         0x11, 3, 0x11, 3, 0x11, 3, 0x11, 3,   /* V(1)   pos = 30->3F */
         0x11, 3, 0x11, 3, 0x11, 3, 0x11, 3,
         0x11, 3, 0x11, 3, 0x11, 3, 0x11, 3,
         0x11, 3, 0x11, 3, 0x11, 3, 0x11, 3};

static int g5_decode_init(G5DECIMAGE *pImage, int iWidth, int iHeight, uint8_t *pData, int iDataSize)
{
    if (pImage == NULL || iWidth < 1 || iHeight < 1 || pData == NULL || iDataSize < 1)
        return G5_INVALID_PARAMETER;
    
    pImage->iVLCSize = iDataSize;
    pImage->pSrc = pData;
    pImage->ulBitOff = 0;
    pImage->y = 0;
    pImage->ulBits = TIFFMOTOLONG(pData); // preload the first 32 bits of data
    pImage->iWidth = iWidth;
    pImage->iHeight = iHeight;
    return G5_SUCCESS;

} /* g5_decode_init() */

static void G5DrawLine(G5DECIMAGE *pPage, int16_t *pCurFlips, uint8_t *pOut)
{
    int x, len, run;
    uint8_t lBit, rBit, *p;
    int iStart = 0, xright = pPage->iWidth;
    uint8_t *pDest;

    iStart = 0;
    
    pDest = pOut;
    len = (xright+7)>>3; // number of bytes to generate
    for (x=0; x<len; x++) {
        pOut[x] = 0xff; // start with white and only draw the black runs
    }
    x = 0;
    while (x < xright) { // while the scaled x is within the window bounds
        x = *pCurFlips++; // black starting point
        run = *pCurFlips++ - x; // get the black run
        x -= iStart;
        if (x >= xright || run == 0)
             break;
        if ((x + run) > 0) { /* If the run is visible, draw it */
             if (x < 0) {
                run += x; /* draw only visible part of run */
                x = 0;
             }
            if ((x + run) > xright) { /* Don't let it go off right edge */
                run = xright - x;
            }
            /* Draw this run */
            lBit = 0xff << (8 - (x & 7));
            rBit = 0xff >> ((x + run) & 7);
            len = ((x+run)>>3) - (x >> 3);
            p = &pDest[x >> 3];
            if (len == 0) {
                lBit |= rBit;
                *p &= lBit;
            } else {
                *p++ &= lBit;
                while (len > 1) {
                   *p++ = 0;
                   len--;
                }
                *p = rBit;
            }
        } // visible run
    } /* while drawing line */
} /* G5DrawLine() */
//
// Initialize internal structures to decode the image
//
static void Decode_Begin(G5DECIMAGE *pPage)
{
    int i, xsize;
    int16_t *CurFlips, *RefFlips;
    
    xsize = pPage->iWidth;
    
    RefFlips = pPage->RefFlips;
    CurFlips = pPage->CurFlips;
    
    /* Seed the current and reference line with XSIZE for V(0) codes */
     for (i=0; i<MAX_IMAGE_FLIPS-2; i++) {
         RefFlips[i] = xsize;
         CurFlips[i] = xsize;
     }
    /* Prefill both current and reference lines with 7fff to prevent it from
       walking off the end if the data gets bunged and the current X is > XSIZE
       3-16-94 */
     CurFlips[i] = RefFlips[i] = 0x7fff;
     CurFlips[i+1] = RefFlips[i+1] = 0x7fff;
    
    pPage->pCur = CurFlips;
    pPage->pRef = RefFlips;
    pPage->pBuf = pPage->pSrc;
    pPage->ulBits = TIFFMOTOLONG(pPage->pSrc); // load 32 bits to start
    pPage->ulBitOff = 0;
    // Calculate the number of bits needed for a long horizontal code
#ifdef __AVR__
    pPage->iHLen = 16 - __builtin_clz(pPage->iWidth);
#else
    pPage->iHLen = 32 - __builtin_clz(pPage->iWidth);
#endif
} /* Decode_Begin() */
//
// Decode a single line of G5 data (private function)
//
static int DecodeLine(G5DECIMAGE *pPage)
{
    signed int a0, a0_p, b1;
    int16_t *pCur, *pRef, *RefFlips, *CurFlips;
    int xsize, tot_run=0, tot_run1 = 0;
    int32_t sCode;
    uint32_t lBits;
    uint32_t ulBits, ulBitOff;
    uint8_t *pBuf/*, *pBufEnd*/;
    uint32_t u32HMask, u32HLen; // horizontal code mask and length

    pCur = CurFlips = pPage->pCur;
    pRef = RefFlips = pPage->pRef;
    ulBits = pPage->ulBits;
    ulBitOff = pPage->ulBitOff;
    pBuf = pPage->pBuf;
    // pBufEnd = &pPage->pSrc[pPage->iVLCSize];
    u32HLen = pPage->iHLen;
    u32HMask = (1 << u32HLen) - 1;
    a0 = -1;
    xsize = pPage->iWidth;
    
    while (a0 < xsize) {  /* Decode this line */
        if (ulBitOff > (REGISTER_WIDTH-8)) { // need at least 7 unused bits
            pBuf += (ulBitOff >> 3);
            ulBitOff &= 7;
            ulBits = TIFFMOTOLONG(pBuf);
        }
        if ((int32_t)(ulBits << ulBitOff) < 0) { /* V(0) code is the most frequent case (1 bit) */
            a0 = *pRef++;
            ulBitOff++; // length = 1 bit
            *pCur++ = a0;
        } else { /* Slower method for the less frequence codes */
            lBits = (ulBits >> ((REGISTER_WIDTH - 8) - ulBitOff)) & 0xfe; /* Only the first 7 bits are useful */
            sCode = code_table[lBits]; /* Get the code type as an 8-bit value */
            ulBitOff += code_table[lBits+1]; /* Get the code length */
            switch (sCode) {
                case 1: /* V(-1) */
                case 2: /* V(-2) */
                case 3: /* V(-3) */
                    a0 = *pRef - sCode;  /* A0 = B1 - x */
                    *pCur++ = a0;
                    if (pRef == RefFlips) {
                        pRef += 2;
                    }
                    pRef--;
                    while (a0 >= *pRef) {
                        pRef += 2;
                    }
                    break;

                case 0x11: /* V(1) */
                case 0x12: /* V(2) */
                case 0x13: /* V(3) */
                    a0 = *pRef++;   /* A0 = B1 */
                    b1 = a0;
                    a0 += sCode & 7;      /* A0 = B1 + x */
                    if (b1 != xsize && a0 < xsize) {
                        while (a0 >= *pRef) {
                            pRef += 2;
                        }
                    }
                    if (a0 > xsize) {
                        a0 = xsize;
                    }
                    *pCur++ = a0;
                    break;

                case 0x20: /* Horizontal codes */
                    if (ulBitOff > (REGISTER_WIDTH-16)) { // need at least 16 unused bits
                        pBuf += (ulBitOff >> 3);
                        ulBitOff &= 7;
                        ulBits = TIFFMOTOLONG(pBuf);
                    }
                    a0_p = a0;
                    if (a0 < 0) {
                        a0_p = 0;
                    }
                    lBits = (ulBits >> ((REGISTER_WIDTH - 2) - ulBitOff)) & 0x3; // get 2-bit prefix for code type
                    // There are 4 possible horizontal cases: short/short, short/long, long/short, long/long
                    // These are encoded in a 2-bit prefix code, followed by 3 bits for short or N bits for long code
                    // N is the log base 2 of the image width (e.g. 320 pixels requires 9 bits)
                    ulBitOff += 2;
                    switch (lBits) {
                        case HORIZ_SHORT_SHORT:
                            tot_run = (ulBits >> ((REGISTER_WIDTH - 3) - ulBitOff)) & 0x7; // get 3-bit short length
                            ulBitOff += 3;
                            tot_run1 = (ulBits >> ((REGISTER_WIDTH - 3) - ulBitOff)) & 0x7; // get 3-bit short length
                            ulBitOff += 3;
                            break;
                        case HORIZ_SHORT_LONG:
                            tot_run = (ulBits >> ((REGISTER_WIDTH - 3) - ulBitOff)) & 0x7; // get 3-bit short length
                            ulBitOff += 3;
                            tot_run1 = (ulBits >> ((REGISTER_WIDTH - u32HLen) - ulBitOff)) & u32HMask; // get long length
                            ulBitOff += u32HLen;
                            break;
                        case HORIZ_LONG_SHORT:
                            tot_run = (ulBits >> ((REGISTER_WIDTH - u32HLen) - ulBitOff)) & u32HMask; // get long length
                            ulBitOff += u32HLen;
                            tot_run1 = (ulBits >> ((REGISTER_WIDTH - 3) - ulBitOff)) & 0x7; // get 3-bit short length
                            ulBitOff += 3;
                            break;
                        case HORIZ_LONG_LONG:
                            tot_run = (ulBits >> ((REGISTER_WIDTH - u32HLen) - ulBitOff)) & u32HMask; // get long length
                            ulBitOff += u32HLen;
                            if (ulBitOff > (REGISTER_WIDTH-16)) { // need at least 16 unused bits
                                pBuf += (ulBitOff >> 3);
                                ulBitOff &= 7;
                                ulBits = TIFFMOTOLONG(pBuf);
                            }
                            tot_run1 = (ulBits >> ((REGISTER_WIDTH - u32HLen) - ulBitOff)) & u32HMask; // get long length
                            ulBitOff += u32HLen;
                            break;
                    } // switch on lBits
                    a0 = a0_p + tot_run;
                    *pCur++ = a0;
                    a0 += tot_run1;
                    if (a0 < xsize) {
                        while (a0 >= *pRef) {
                            pRef += 2;
                        }
                    }
                    *pCur++ = a0;
                    break;

                case 0x30: /* Pass code */
                    pRef++;         /* A0 = B2, iRef+=2 */
                    a0 = *pRef++;
                break;
                     
                default: /* ERROR */
                   pPage->iError = G5_DECODE_ERROR;
                   goto pilreadg5z;
             } /* switch */
          } /* Slow climb */
       }
    /*--- Convert flips data into run lengths ---*/
    *pCur++ = xsize;  /* Terminate the line properly */
    *pCur++ = xsize;
pilreadg5z:
    // Save the current VLC decoder state
    pPage->ulBits = ulBits;
    pPage->ulBitOff = ulBitOff;
    pPage->pBuf = pBuf;
    return pPage->iError;
} /* DecodeLine() */
//
// Decompress the VLC data
//
static int g5_decode_line(G5DECIMAGE *pPage, uint8_t *pOut)
{
    int rc;
    uint8_t *pBufEnd;
    int16_t *t1;
    
    if (pPage == NULL || pOut == NULL)
        return G5_INVALID_PARAMETER;
    if (pPage->y >= pPage->iHeight)
        return G5_DECODE_COMPLETE;
    
    pPage->iError = G5_SUCCESS;
    
    if (pPage->y == 0) { // first time through
        Decode_Begin(pPage);
    }
    pBufEnd = &pPage->pSrc[pPage->iVLCSize];
    
   if (pPage->pBuf >= pBufEnd) { // read past the end, error
       pPage->iError = G5_DECODE_ERROR;
       return G5_DECODE_ERROR;
   }
   rc = DecodeLine(pPage);
   if (rc == G5_SUCCESS) {
       // Draw the current line
       G5DrawLine(pPage, pPage->pCur, pOut);
       /*--- Swap current and reference lines ---*/
       t1 = pPage->pRef;
       pPage->pRef = pPage->pCur;
       pPage->pCur = t1;
       pPage->y++;
       if (pPage->y >= pPage->iHeight) {
           pPage->iError = G5_DECODE_COMPLETE;
       }
   } else {
       pPage->iError = rc;
   }
    return pPage->iError;
} /* Decode() */
