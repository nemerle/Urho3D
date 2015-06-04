/****************************************************************************
 *            normal.h
 *
 * Author: 2008  Daniel Jungmann <dsj@gmx.net>
 * Copyright: See COPYING file that comes with this distribution
 ****************************************************************************/

#ifndef	_NORMAL_H_
#define	_NORMAL_H_

#include <stdint.h>

uint16_t compress_normal(const float *normal);
void uncompress_normal(uint16_t value, float *normal);

#endif	/* _NORMAL_H_ */

