/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file bitmath_func.cpp Functions related to bit mathematics. */

#include "../stdafx.h"
#include "bitmath_func.hpp"

#include "../safeguards.h"

#ifndef WITH_BITMATH_BUILTINS

const uint8_t _ffb_64[64] = {
	0,  0,  1,  0,  2,  0,  1,  0,
	3,  0,  1,  0,  2,  0,  1,  0,
	4,  0,  1,  0,  2,  0,  1,  0,
	3,  0,  1,  0,  2,  0,  1,  0,
	5,  0,  1,  0,  2,  0,  1,  0,
	3,  0,  1,  0,  2,  0,  1,  0,
	4,  0,  1,  0,  2,  0,  1,  0,
	3,  0,  1,  0,  2,  0,  1,  0,
};

uint8_t FindFirstBit32(uint32_t x)
{
	if (x == 0) return 0;
	/* The macro FIND_FIRST_BIT is better to use when your x is
	  not more than 128. */

	uint8_t pos = 0;

	if ((x & 0x0000ffff) == 0) { x >>= 16; pos += 16; }
	if ((x & 0x000000ff) == 0) { x >>= 8;  pos += 8;  }
	if ((x & 0x0000000f) == 0) { x >>= 4;  pos += 4;  }
	if ((x & 0x00000003) == 0) { x >>= 2;  pos += 2;  }
	if ((x & 0x00000001) == 0) { pos += 1; }

	return pos;
}

uint8_t FindFirstBit64(uint64_t x)
{
	if (x == 0) return 0;
	if ((x & 0x00000000ffffffffULL) != 0) return FindFirstBit32(static_cast<uint32_t>(x));
	return FindFirstBit32(static_cast<uint32_t>(x >> 32)) + 32;
}

#endif

/**
 * Search the last set bit in a 64 bit variable.
 *
 * This algorithm is a static implementation of a log
 * congruence search algorithm. It checks the second half
 * if there is a bit set search there further. And this
 * way further. If no bit is set return 0.
 *
 * @param x The value to search
 * @return The position of the last bit set
 */
uint8_t FindLastBit64(uint64_t x)
{
	if (x == 0) return 0;

	uint8_t pos = 0;

	if ((x & 0xffffffff00000000ULL) != 0) { x >>= 32; pos += 32; }
	if ((x & 0x00000000ffff0000ULL) != 0) { x >>= 16; pos += 16; }
	if ((x & 0x000000000000ff00ULL) != 0) { x >>= 8;  pos += 8;  }
	if ((x & 0x00000000000000f0ULL) != 0) { x >>= 4;  pos += 4;  }
	if ((x & 0x000000000000000cULL) != 0) { x >>= 2;  pos += 2;  }
	if ((x & 0x0000000000000002ULL) != 0) { pos += 1; }

	return pos;
}
