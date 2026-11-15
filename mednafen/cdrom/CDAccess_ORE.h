/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __MDFN_CDACCESS_ORE_H
#define __MDFN_CDACCESS_ORE_H

#include "CDAccess.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An ORE, oxyromon's disc format, read through libsmelt. */
CDAccess *CDAccess_ORE_New(bool *success, const char *path,
      bool image_memcache);

/* Whether the file at path begins with an ORE's magic, "ORE1". */
bool cdaccess_file_is_ore(const char *path);

#ifdef __cplusplus
}
#endif

#endif
