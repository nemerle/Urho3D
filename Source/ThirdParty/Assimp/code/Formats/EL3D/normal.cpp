/****************************************************************************
 *            normal.c
 *
 * Author: 2008  Daniel Jungmann <dsj@gmx.net>
 * Copyright: See COPYING file that comes with this distribution
 ****************************************************************************/

#include "normal.h"
#include <cmath>

// upper 3 bits
#define SIGN_MASK  0xe000
#define XSIGN_MASK 0x8000
#define YSIGN_MASK 0x4000
#define ZSIGN_MASK 0x2000

// middle 6 bits - xbits
#define TOP_MASK  0x1f80

// lower 7 bits - ybits
#define BOTTOM_MASK  0x007f

void uncompress_normal(const uint16_t value, float *normal)
{
    uint32_t x, y;
    float len;

    /**
     * if we do a straightforward backward transform
     * we will get points on the plane X0,Y0,Z0
     * however we need points on a sphere that goes through
     * these points. Therefore we need to adjust x,y,z so
     * that x^2+y^2+z^2=1 by normalizing the vector. We have
     * already precalculated the amount by which we need to
     * scale, so all we do is a table lookup and a
     * multiplication
     * get the x and y bits
     */

    x = (value & TOP_MASK) >> 7;
    y = value & BOTTOM_MASK;

    // map the numbers back to the triangle (0,0)-(0,126)-(126,0)
    if ((x + y) >= 127)
    {
        x = 127 - x;
        y = 127 - y;
    }

    /**
     * do the inverse transform and normalization
     * costs 3 extra multiplies and 2 subtracts. No big deal.
     */
    normal[0] = x;
    normal[1] = y;
    normal[2] = 126 - x - y;

    // set all the sign bits
    if (value & XSIGN_MASK)
    {
        normal[0] = -normal[0];
    }

    if (value & YSIGN_MASK)
    {
        normal[1] = -normal[1];
    }

    if (value & ZSIGN_MASK)
    {
        normal[2] = -normal[2];
    }

    len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);

    normal[0] /= len;
    normal[1] /= len;
    normal[2] /= len;
}

