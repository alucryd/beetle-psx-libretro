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

/* An ORE read through libsmelt, as the CUE it was made from reads through
 * CDAccess_Image: the same tracks at the same LBAs, the same sectors, a
 * PREGAP the files do not hold read as zeros, and the SBI it carries
 * applied to the Q subchannel as one beside a CUE is.
 *
 * libsmelt gives whole 2352-byte sectors by LBA across the tracks, the
 * track list, and the Q each SBI entry replaces. The ingot is only read
 * from Read_Raw_Sector, which cdromif calls from one thread at a time;
 * Read_Raw_PW and Read_TOC use the tables built at open, so the emu
 * thread may call them while the read thread reads. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <boolean.h>

#include <streams/file_stream.h>
#include <libretro.h>

#include "../mednafen.h"
#include "../error.h"

#include "CDAccess.h"
#include "CDAccess_ORE.h"
#include "cdaccess_track.h"
#include "CDUtility.h"

#include <smelt.h>

extern retro_log_printf_t log_cb;

enum
{
   DI_FORMAT_AUDIO     = 0x00,
   DI_FORMAT_MODE1_RAW = 0x02,
   DI_FORMAT_MODE2_RAW = 0x06
};

struct CDAccess_ORE
{
   CDAccess            base;
   smelt_ingot        *ingot;

   int32_t             NumTracks;
   int32_t             FirstTrack;
   int32_t             LastTrack;
   int32_t             total_sectors;
   uint8_t             disc_type;
   TOC                *ptoc;

   CDRFILE_TRACK_INFO  Tracks[100];
   subq_map            SubQReplaceMap;
};

bool cdaccess_file_is_ore(const char *path)
{
   RFILE  *fp;
   uint8_t magic[4];
   int64_t got;

   fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!fp)
      return false;

   got = filestream_read(fp, magic, sizeof(magic));
   filestream_close(fp);

   /* A later layout is an ORE too: libsmelt refuses it by name. */
   return got == (int64_t)sizeof(magic) && !memcmp(magic, "ORE", 3)
         && magic[3] >= '1' && magic[3] <= '9';
}

/* As CDAccess_Image_MakeSubPQ: ORs the P and Q this LBA generates, or
 * its SBI's Q, into SubPWBuf, and returns its track. */
static int32_t CDAccess_ORE_MakeSubPQ(struct CDAccess_ORE *self,
      int32_t lba, uint8_t *SubPWBuf)
{
   unsigned        i;
   uint8_t         buf[0xC];
   uint8_t         adr;
   uint8_t         control;
   int32_t         track;
   uint32_t        lba_relative;
   uint32_t        ma, sa, fa;
   uint32_t        m, s, f;
   uint8_t         pause_or    = 0x00;
   bool            track_found = false;
   const uint8_t  *sbi_replacement;

   for (track = self->FirstTrack;
         track < (self->FirstTrack + self->NumTracks); track++)
   {
      if (lba >= (self->Tracks[track].LBA - self->Tracks[track].pregap_dv
                  - self->Tracks[track].pregap)
            && lba < (self->Tracks[track].LBA + self->Tracks[track].sectors
                      + self->Tracks[track].postgap))
      {
         track_found = true;
         break;
      }
   }

   if (!track_found)
      track = self->FirstTrack;

   lba_relative = abs((int32_t)lba - self->Tracks[track].LBA);

   f = (lba_relative % 75);
   s = ((lba_relative / 75) % 60);
   m = (lba_relative / 75 / 60);

   {
      int32_t lba_a = (int32_t)((uint32_t)lba + 150u);
      fa = lba_a % 75;
      sa = (lba_a / 75) % 60;
      ma = (lba_a / 75 / 60);
   }

   adr     = 0x1;
   control = self->Tracks[track].subq_control;

   /* Pause bit (D7) - set when in pregap or postgap. */
   if (lba < self->Tracks[track].LBA
         || lba >= self->Tracks[track].LBA + self->Tracks[track].sectors)
      pause_or = 0x80;

   /* Pregap between audio->data track. */
   {
      int32_t pg_offset = (int32_t)lba - self->Tracks[track].LBA;
      if (pg_offset < -150)
      {
         if ((self->Tracks[track].subq_control & SUBQ_CTRLF_DATA)
               && (self->FirstTrack < track)
               && !(self->Tracks[track - 1].subq_control & SUBQ_CTRLF_DATA))
            control = self->Tracks[track - 1].subq_control;
      }
   }

   memset(buf, 0, 0xC);
   buf[0] = (adr << 0) | (control << 4);
   buf[1] = U8_to_BCD(track);

   if (lba < self->Tracks[track].LBA)   /* Index is 00 in pregap */
      buf[2] = U8_to_BCD(0x00);
   else
      buf[2] = U8_to_BCD(0x01);

   buf[3] = U8_to_BCD(m);
   buf[4] = U8_to_BCD(s);
   buf[5] = U8_to_BCD(f);
   buf[6] = 0;
   buf[7] = U8_to_BCD(ma);
   buf[8] = U8_to_BCD(sa);
   buf[9] = U8_to_BCD(fa);

   subq_generate_checksum(buf);

   sbi_replacement = subq_map_find(&self->SubQReplaceMap, LBA_to_ABA(lba));
   if (sbi_replacement)
      memcpy(buf, sbi_replacement, 12);

   for (i = 0; i < 96; i++)
      SubPWBuf[i] |= (((buf[i >> 3] >> (7 - (i & 0x7))) & 1) ? 0x40 : 0x00)
                     | pause_or;

   return track;
}

static bool CDAccess_ORE_Read_Raw_Sector(CDAccess *base_self, uint8_t *buf,
      int32_t lba)
{
   struct CDAccess_ORE *self = (struct CDAccess_ORE *)base_self;
   CDRFILE_TRACK_INFO  *ct;
   int32_t              track;
   smelt_err            err;

   if (lba >= self->total_sectors)
   {
      uint8_t mode = self->Tracks[self->LastTrack].DIFormat
            == DI_FORMAT_MODE2_RAW ? 0x02 : 0x01;
      synth_leadout_sector_lba(mode, self->ptoc, lba, buf);
      return true;
   }

   memset(buf + 2352, 0, 96);
   track = CDAccess_ORE_MakeSubPQ(self, lba, buf + 2352);
   ct    = &self->Tracks[track];

   /* A PREGAP the files do not hold, or a sector before the disc */
   if (lba < (ct->LBA - ct->pregap_dv) || lba >= (ct->LBA + ct->sectors))
   {
      memset(buf, 0, 2352);
      return true;
   }

   err = smelt_read_sectors(self->ingot, (uint32_t)lba, 1, buf);
   if (err != SMELT_OK)
   {
      log_cb(RETRO_LOG_ERROR, "[ORE] sector %d: %s\n", (int)lba,
            smelt_last_error());
      memset(buf, 0, 2352);
      return true;
   }

#ifdef MSB_FIRST
   /* Path 2 contract: host-endian samples, and an ORE's audio is the
    * disc's, little-endian. */
   if (ct->DIFormat == DI_FORMAT_AUDIO)
   {
      int32_t i;
      for (i = 0; i < 588 * 2 * 2; i += 4)
      {
         uint32_t v;
         memcpy(&v, buf + i, 4);
         v = ((v & 0xFF00FF00U) >> 8) | ((v & 0x00FF00FFU) << 8);
         memcpy(buf + i, &v, 4);
      }
   }
#endif
   return true;
}

static bool CDAccess_ORE_Read_Raw_PW(CDAccess *base_self, uint8_t *buf,
      int32_t lba)
{
   struct CDAccess_ORE *self = (struct CDAccess_ORE *)base_self;
   memset(buf, 0, 96);
   CDAccess_ORE_MakeSubPQ(self, lba, buf);
   return true;
}

static bool CDAccess_ORE_Read_TOC(CDAccess *base_self, TOC *toc)
{
   struct CDAccess_ORE *self = (struct CDAccess_ORE *)base_self;
   int32_t              i;

   TOC_Clear(toc);

   toc->first_track = self->FirstTrack;
   toc->last_track  = self->LastTrack;
   toc->disc_type   = self->disc_type;

   for (i = self->FirstTrack; i <= self->LastTrack; i++)
   {
      toc->tracks[i].control = self->Tracks[i].subq_control;
      toc->tracks[i].adr     = ADR_CURPOS;
      toc->tracks[i].lba     = self->Tracks[i].LBA;
   }

   toc->tracks[100].lba     = self->total_sectors;
   toc->tracks[100].adr     = ADR_CURPOS;
   toc->tracks[100].control = toc->tracks[toc->last_track].control & 0x4;

   /* Convenience leadout track duplication. */
   if (toc->last_track < 99)
      toc->tracks[toc->last_track + 1] = toc->tracks[100];

   self->ptoc = toc;
   return true;
}

static void CDAccess_ORE_Eject(CDAccess *base_self, bool eject_status)
{
   (void)base_self;
   (void)eject_status;
}

static void CDAccess_ORE_destroy(CDAccess *base_self)
{
   struct CDAccess_ORE *self = (struct CDAccess_ORE *)base_self;
   smelt_close(self->ingot);
   free(self);
}

/* The tracks libsmelt gives, as CDAccess_Image lays a CUE's out. */
static bool CDAccess_ORE_Tracks(struct CDAccess_ORE *self)
{
   size_t   count = 0;
   size_t   i;
   int32_t  end   = 0;

   if (smelt_track_count(self->ingot, &count) != SMELT_OK || count == 0
         || count > 99)
   {
      MDFN_Error(0, "ORE: not a CD");
      return false;
   }

   for (i = 0; i < count; i++)
   {
      smelt_track         st;
      CDRFILE_TRACK_INFO *t;
      int32_t             start;

      if (smelt_track_at(self->ingot, i, &st) != SMELT_OK)
      {
         MDFN_Error(0, "ORE: %s", smelt_last_error());
         return false;
      }
      start = (int32_t)st.lba - (int32_t)st.pregap;
      if (st.number < 1 || st.number > 99 || st.high_density
            || (i > 0 && st.number != self->LastTrack + 1) || start < end)
      {
         MDFN_Error(0, "ORE: track %u is not a PlayStation disc's",
               (unsigned)st.number);
         return false;
      }

      t = &self->Tracks[st.number];
      memset(t, 0, sizeof(*t));
      t->LBA       = (int32_t)st.lba;
      t->pregap_dv = (int32_t)st.pregap;
      /* What lies between the last track's end and this one's file is
       * a PREGAP the files do not hold */
      t->pregap    = start - end;
      t->sectors   = (int32_t)st.sectors;
      t->index[0]  = st.pregap ? start : -1;
      t->index[1]  = t->LBA;
      /* Its cue sheet's FLAGS, and its data bit */
      t->subq_control = st.control;
      switch (st.mode)
      {
         case SMELT_TRACK_AUDIO:
            t->DIFormat     = DI_FORMAT_AUDIO;
            break;
         case SMELT_TRACK_MODE1:
            t->DIFormat     = DI_FORMAT_MODE1_RAW;
            break;
         default:
            t->DIFormat     = DI_FORMAT_MODE2_RAW;
            self->disc_type = DISC_TYPE_CD_XA;
            break;
      }

      if (i == 0)
         self->FirstTrack = st.number;
      self->LastTrack = st.number;
      end             = t->LBA + t->sectors;
   }

   self->NumTracks     = self->LastTrack - self->FirstTrack + 1;
   self->total_sectors = end;
   return true;
}

/* Its SBI's Q, as CDAccess_Image_LoadSBI keeps an SBI's: with the CRC
 * a drive would compute, inverted, as libcrypt's sectors carry it. */
static void CDAccess_ORE_SBI(struct CDAccess_ORE *self)
{
   int32_t lba;
   uint8_t q[12];
   uint8_t replaced;

   subq_map_clear(&self->SubQReplaceMap);
   /* shortcut: every LBA asked for, libsmelt listing no SBI entries;
    * a few ms a disc at open, add a listing to the ABI if it shows */
   for (lba = 0; lba < self->total_sectors; lba++)
   {
      if (smelt_subq_replacement(self->ingot, (uint32_t)lba, q, &replaced)
            != SMELT_OK)
      {
         log_cb(RETRO_LOG_WARN, "[ORE] SBI ignored: %s\n",
               smelt_last_error());
         subq_map_clear(&self->SubQReplaceMap);
         return;
      }
      if (!replaced)
         continue;
      subq_generate_checksum(q);
      q[10] ^= 0xFF;
      q[11] ^= 0xFF;
      subq_map_insert(&self->SubQReplaceMap, LBA_to_ABA(lba), q);
   }
   subq_map_finalize(&self->SubQReplaceMap);
}

CDAccess *CDAccess_ORE_New(bool *success, const char *path,
      bool image_memcache)
{
   struct CDAccess_ORE *self =
      (struct CDAccess_ORE *)calloc(1, sizeof(*self));
   (void)image_memcache;

   if (!self)
   {
      *success = false;
      return NULL;
   }

   self->base.Read_Raw_Sector = CDAccess_ORE_Read_Raw_Sector;
   self->base.Read_Raw_PW     = CDAccess_ORE_Read_Raw_PW;
   self->base.Read_TOC        = CDAccess_ORE_Read_TOC;
   self->base.Eject           = CDAccess_ORE_Eject;
   self->base.destroy         = CDAccess_ORE_destroy;
   self->disc_type            = DISC_TYPE_CDDA_OR_M1;

   /* shortcut: opened by path, not through the frontend's VFS, as
    * libsmelt reads files itself; a VFS-only frontend needs a reader
    * callback in its ABI */
   if (smelt_open(path, NULL, &self->ingot) != SMELT_OK)
   {
      MDFN_Error(0, "ORE: \"%s\": %s", path, smelt_last_error());
      *success = false;
      return &self->base;
   }
   if (!CDAccess_ORE_Tracks(self))
   {
      *success = false;
      return &self->base;
   }
   CDAccess_ORE_SBI(self);

   *success = true;
   return &self->base;
}
