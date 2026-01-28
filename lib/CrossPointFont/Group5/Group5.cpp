#include "g5enc.inl"
#include "g5dec.inl"
//
// Group5 1-bit image compression library
// Written by Larry Bank (bitbank@pobox.com)
// Decoder C++ wrapper functions
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
int G5DECODER::init(int iWidth, int iHeight, uint8_t *pData, int iDataSize)
{
    return g5_decode_init(&_g5dec, iWidth, iHeight, pData, iDataSize);
} /* init() */

int G5DECODER::decodeLine(uint8_t *pOut)
{
    return g5_decode_line(&_g5dec, pOut);
} /* decodeLine() */

//
// Encoder C++ wrapper functions
//
int G5ENCODER::init(int iWidth, int iHeight, uint8_t *pOut, int iOutSize)
{
    return g5_encode_init(&_g5enc, iWidth, iHeight, pOut, iOutSize);
} /* init() */

int G5ENCODER::encodeLine(uint8_t *pPixels)
{
    return g5_encode_encodeLine(&_g5enc, pPixels);
} /* encodeLine() */

int G5ENCODER::size()
{
    return g5_encode_getOutSize(&_g5enc);
} /* size() */

