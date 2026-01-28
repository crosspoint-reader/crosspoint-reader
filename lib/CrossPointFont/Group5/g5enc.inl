//
// G5 Encoder
// A 1-bpp image encoding library
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

/* Number of consecutive 1 bits in a byte from MSB to LSB */
static uint8_t bitcount[256] =
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 0-15 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 16-31 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 32-47 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 48-63 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 64-79 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 80-95 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 96-111 */
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  /* 112-127 */
         1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  /* 128-143 */
         1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  /* 144-159 */
         1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  /* 160-175 */
         1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  /* 176-191 */
         2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  /* 192-207 */
         2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  /* 208-223 */
         3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,  /* 224-239 */
         4,4,4,4,4,4,4,4,5,5,5,5,6,6,7,8}; /* 240-255 */

/* Table of vertical codes for G5 encoding */
/* code followed by length, starting with v(-3) */
static const uint8_t vtable[14] =
        {3,7,     /* V(-3) = 0000011 */
         3,6,     /* V(-2) = 000011  */
         3,3,     /* V(-1) = 011     */
         1,1,     /* V(0)  = 1       */
         2,3,     /* V(1)  = 010     */
         2,6,     /* V(2)  = 000010  */
         2,7};    /* V(3)  = 0000010 */


static void G5ENCInsertCode(G5_BUFFERED_BITS *bb, BIGUINT ulCode, int iLen)
{
    if ((bb->ulBitOff + iLen) > REGISTER_WIDTH) { // need to write data
        bb->ulBits |= (ulCode >> (bb->ulBitOff + iLen - REGISTER_WIDTH)); // partial bits on first word
        *(BIGUINT *)bb->pBuf = __builtin_bswap32(bb->ulBits);
        bb->pBuf += sizeof(BIGUINT);
        bb->ulBits = ulCode << ((REGISTER_WIDTH*2) - (bb->ulBitOff + iLen));
        bb->ulBitOff += iLen - REGISTER_WIDTH;
    } else {
        bb->ulBits |= (ulCode << (REGISTER_WIDTH - bb->ulBitOff - iLen));
        bb->ulBitOff += iLen;
    }
} /* G5ENCInsertCode() */
//
// Flush any buffered bits to the output
//
static void G5ENCFlushBits(G5_BUFFERED_BITS *bb)
{
    while (bb->ulBitOff >= 8)
    {
        *bb->pBuf++ = (unsigned char) (bb->ulBits >> (REGISTER_WIDTH - 8));
        bb->ulBits <<= 8;
        bb->ulBitOff -= 8;
    }
    if (bb->ulBitOff) { // partial byte?
        *bb->pBuf++ = (unsigned char) (bb->ulBits >> (REGISTER_WIDTH - 8));
    }
    bb->ulBitOff = 0;
    bb->ulBits = 0;
} /* G5ENCFlushBits() */
//
// Initialize the compressor
// This must be called before adding data to the output
//
static int g5_encode_init(G5ENCIMAGE *pImage, int iWidth, int iHeight, uint8_t *pOut, int iOutSize)
{
    int iError = G5_SUCCESS;
    
    if (pImage == NULL || iHeight <= 0)
        return G5_INVALID_PARAMETER;
    pImage->iWidth = iWidth; // image size
    pImage->iHeight = iHeight;
    pImage->pCur = pImage->CurFlips;
    pImage->pRef = pImage->RefFlips;
    pImage->pOutBuf = pOut; // optional output buffer
    pImage->iOutSize = iOutSize; // output buffer pre-allocated size
    pImage->iDataSize = 0; // no data yet
    pImage->y = 0;
    for (int i=0; i<MAX_IMAGE_FLIPS; i++) {
        pImage->RefFlips[i] = iWidth;
        pImage->CurFlips[i] = iWidth;
    }
    pImage->bb.pBuf = pImage->pOutBuf;
    pImage->bb.ulBits = 0;
    pImage->bb.ulBitOff = 0;
    pImage->iError = iError;
    return iError;
} /* g5_encode_init() */
//
// Internal function to convert uncompressed 1-bit per pixel data
// into the run-end data needed to feed the G5 encoder
//
static int G5ENCEncodeLine(unsigned char *buf, int xsize, int16_t *pDest)
{
int iCount, xborder;
uint8_t i, c;
int8_t cBits;
int iLen;
int16_t x;
int16_t *pLimit = pDest + (MAX_IMAGE_FLIPS-4);

   xborder = xsize;
   iCount = (xsize + 7) >> 3; /* Number of bytes per line */
   cBits = 8;
   iLen = 0; /* Current run length */
   x = 0;

   c = *buf++;  /* Get the first byte to start */
   iCount--;
   while (iCount >=0) {
      if (pDest >= pLimit) return G5_MAX_FLIPS_EXCEEDED;
      i = bitcount[c]; /* Get the number of consecutive bits */
      iLen += i; /* Add this length to total run length */
      c <<= i;
      cBits -= i; /* Minus the number in a byte */
      if (cBits <= 0)
         {
         iLen += cBits; /* Adjust length */
         cBits = 8;
         c = *buf++;  /* Get another data byte */
         iCount--;
         continue; /* Keep doing white until color change */
         }
      c = ~c; /* flip color to count black pixels */
   /* Store the white run length */
      xborder -= iLen;
      if (xborder < 0)
         {
         iLen += xborder; /* Make sure run length is not past end */
         break;
         }
      x += iLen;
      *pDest++ = x;
      iLen = 0;
doblack:
      i = bitcount[c]; /* Get consecutive bits */
      iLen += i; /* Add to total run length */
      c <<= i;
      cBits -= i;
      if (cBits <= 0)
         {
         iLen += cBits; /* Adjust length */
         cBits = 8;
         c = *buf++;  /* Get another data byte */
         c = ~c;   /* Flip color to find black */
         iCount--;
         if (iCount < 0)
            break;
         goto doblack;
         }
   /* Store the black run length */
      c = ~c;       /* Flip color again to find white pixels */
      xborder -= iLen;
      if (xborder < 0)
         {
         iLen += xborder; /* Make sure run length is not past end */
         break;
         }
      x += iLen;
      *pDest++ = x;
      iLen = 0;
      } /* while */

   if (pDest >= pLimit) return G5_MAX_FLIPS_EXCEEDED;
   *pDest++ = xsize;
   *pDest++ = xsize; // Store a few more XSIZE to end the line
   *pDest++ = xsize; // so that the compressor doesn't go past
   *pDest++ = xsize; // the end of the line
   return G5_SUCCESS;
} /* G5ENCEncodeLine() */
//
// Compress a line of pixels and add it to the output
// the input format is expected to be MSB (most significant bit) first
// for example, pixel 0 is in byte 0 at bit 7 (0x80)
// Returns G5ENC_SUCCESS for each line if all is well and G5ENC_IMAGE_COMPLETE
// for the last line
//
static int g5_encode_encodeLine(G5ENCIMAGE *pImage, uint8_t *pPixels)
{
int16_t a0, a0_c, b2, a1;
int dx, run1, run2;
int xsize, iErr, iHighWater;
int iCur, iRef, iLen;
int iHLen; // number of bits for long horizontal codes
int16_t *CurFlips, *RefFlips;
G5_BUFFERED_BITS bb;

    if (pImage == NULL || pPixels == NULL)
        return G5_INVALID_PARAMETER;
    iHighWater = pImage->iOutSize - 32;
    iHLen = 32 - __builtin_clz(pImage->iWidth);
    memcpy(&bb, &pImage->bb, sizeof(G5_BUFFERED_BITS)); // keep local copy
    CurFlips = pImage->pCur;
    RefFlips = pImage->pRef;
    xsize = pImage->iWidth; /* For performance reasons */

    // Convert the incoming line of pixels into run-end data
    iErr = G5ENCEncodeLine(pPixels, pImage->iWidth, CurFlips);
    if (iErr != G5_SUCCESS) return iErr; // exceeded the maximum number of color changes
    /* Encode this line as G5 */
    a0 = a0_c = 0;
    iCur = iRef = 0;
    while (a0 < xsize) {
        b2 = RefFlips[iRef+1];
        a1 = CurFlips[iCur];
        if (b2 < a1) { /* Is b2 to the left of a1? */
            /* yes, do pass mode */
            a0 = b2;
            iRef += 2;
            G5ENCInsertCode(&bb, 1, 4); /* Pass code = 0001 */
        } else { /* Try vertical and horizontal mode */
            dx = RefFlips[iRef] - a1;  /* b1 - a1 */
            if (dx > 3 || dx < -3) { /* Horizontal mode */
                G5ENCInsertCode(&bb, 1, 3); /* Horizontal code = 001 */
                run1 = CurFlips[iCur] - a0;
                run2 = CurFlips[iCur+1] - CurFlips[iCur];
                if (run1 < 8) {
                    if (run2 < 8) { // short, short
                        G5ENCInsertCode(&bb, HORIZ_SHORT_SHORT, 2); /* short, short = 00 */
                        G5ENCInsertCode(&bb, run1, 3);
                        G5ENCInsertCode(&bb, run2, 3);
                    } else { // short, long
                        G5ENCInsertCode(&bb, HORIZ_SHORT_LONG, 2); /* short, long = 01 */
                        G5ENCInsertCode(&bb, run1, 3);
                        G5ENCInsertCode(&bb, run2, iHLen);
                    }
                } else { // first run is long
                    if (run2 < 8) { // long, short
                        G5ENCInsertCode(&bb, HORIZ_LONG_SHORT, 2); /* long, short = 10 */
                        G5ENCInsertCode(&bb, run1, iHLen);
                        G5ENCInsertCode(&bb, run2, 3);
                    } else { // long, long
                        G5ENCInsertCode(&bb, HORIZ_LONG_LONG, 2); /* long, long = 11 */
                        G5ENCInsertCode(&bb, run1, iHLen);
                        G5ENCInsertCode(&bb, run2, iHLen);
                    }
                }
               a0 = CurFlips[iCur+1]; /* a0 = a2 */
               if (a0 != xsize) {
                  iCur += 2; /* Skip two color flips */
                   while (RefFlips[iRef] != xsize && RefFlips[iRef] <= a0) {
                       iRef += 2;
                   }
                }
            } else { /* Vertical mode */
               dx = (dx + 3) * 2; /* Convert to index table */
                G5ENCInsertCode(&bb, vtable[dx], vtable[dx+1]);
                a0 = a1;
                a0_c = 1-a0_c;
                if (a0 != xsize) {
                    if (iRef != 0) {
                        iRef -= 2;
                    }
                    iRef++; /* Skip a color change in cur and ref */
                    iCur++;
                    while (RefFlips[iRef] <= a0 && RefFlips[iRef] != xsize) {
                        iRef += 2;
                    }
                }
            } /* vertical mode */
        } /* horiz/vert mode */
    } /* while x < xsize */
    iLen = (int)(bb.pBuf-pImage->pOutBuf);
    if (iLen >= iHighWater) { // not enough space
           pImage->iError = iErr = G5_DATA_OVERFLOW; // we don't have a better error
           return iErr;
    }
    if (pImage->y == pImage->iHeight-1) { // last line of image
        G5ENCFlushBits(&bb); // output the final buffered bits
        // wrap up final output
        pImage->iDataSize = 1 + (int)(bb.pBuf-pImage->pOutBuf);
        iErr = G5_ENCODE_COMPLETE;
    }
    pImage->pCur = RefFlips; // swap current and reference lines
    pImage->pRef = CurFlips;
    pImage->y++;
    memcpy(&pImage->bb, &bb, sizeof(bb));
    return iErr;
} /* g5_encode_encodeLine() */
//
// Returns the number of bytes of G5 created by the encoder
//
static int g5_encode_getOutSize(G5ENCIMAGE *pImage)
{
    int iSize = 0;
    if (pImage != NULL)
        iSize = pImage->iDataSize;
    return iSize;
} /* g5_encode_getOutSize() */
