/*
 * Copyright 2003 James Bursa <bursa@users.sourceforge.net>
 * Copyright 2004 John Tytgat <John.Tytgat@aaug.net>
 *
 * This file is part of NetSurf, http://www.netsurf-browser.org/
 *
 * NetSurf is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * NetSurf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>

#ifndef _LIBNSGIF_LOG_H_
#define _LIBNSGIF_LOG_H_

#ifdef NDEBUG
#  define LOG(x) ((void) 0)
#else
#  ifdef __GNUC__
#    define LOG(x) do { printf x, fputc('\n', stdout)); } while (0)
#  elif defined(__CC_NORCROFT)
#    define LOG(x) do { printf x, fputc('\n', stdout)); } while (0)
#  else
#    define LOG(x) do { printf x, fputc('\n', stdout)); } while (0)
#  endif
#endif

#endif
