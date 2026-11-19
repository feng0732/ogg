/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2018             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: granulepos-based seeking within a logical Ogg bitstream

 ********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <ogg/ogg.h>

/* Nominal read chunk and bisection cutoff.  Once the bisection window
   narrows to this many bytes the search finishes with a bounded linear
   scan, so the total amount of data read during a seek grows
   logarithmically with the size of the datasource. */
#define OGG_SEEK_CHUNK 16384

/* State for pulling successive pages out of the datasource.  'offset'
   tracks the file offset of the next byte the sync layer has not yet
   consumed, so the absolute position of every captured page is
   known. */
typedef struct {
  const ogg_seek_callbacks *cb;
  void             *datasource;
  ogg_sync_state   *oy;
  ogg_int64_t       offset;
  ogg_int64_t       limit;   /* do not return pages beginning at or after
                                this offset; -1 for no limit */
} ogg_seek_fetch;

/* Reposition the datasource and restart capture at a known offset. */
static int _fetch_at(ogg_seek_fetch *f, ogg_int64_t offset, ogg_int64_t limit){
  if(f->cb->seek_func(f->datasource, offset, OGG_SEEK_SET)) return -1;
  ogg_sync_reset(f->oy);
  f->offset = offset;
  f->limit = limit;
  return 0;
}

/* Capture the next page at or after the current fetch position.
   Returns 1 and fills og/page_offset on success, 0 on end of data (or
   when the fetch limit is reached), -1 on error.  Garbage between
   pages (eg. when landing mid-page after a bisection hop) is skipped
   silently. */
static int _fetch_page(ogg_seek_fetch *f, ogg_page *og, ogg_int64_t *page_offset){
  for(;;){
    long ret;

    if(f->limit >= 0 && f->offset >= f->limit) return 0;

    ret = ogg_sync_pageseek(f->oy, og);
    if(ret > 0){
      if(page_offset) *page_offset = f->offset;
      f->offset += ret;
      return 1;
    }
    if(ret < 0){
      /* skipped unparseable bytes */
      f->offset += -ret;
      continue;
    }

    /* need more data */
    {
      char *buffer = ogg_sync_buffer(f->oy, OGG_SEEK_CHUNK);
      size_t bytes;
      if(!buffer) return -1;
      bytes = f->cb->read_func(buffer, OGG_SEEK_CHUNK, f->datasource);
      if(bytes == 0) return 0; /* end of data */
      if(ogg_sync_wrote(f->oy, (long)bytes)) return -1;
    }
  }
}

/* Seek the datasource so that decoding the logical stream 'serialno'
   can resume at the first packet whose ending granule position is
   greater than or equal to 'target'.

   On success the datasource is positioned at the beginning of the
   page carrying the most recent known granule position before the
   target (or at the first page of the stream if there is none), the
   sync state 'oy' is reset and ready to be refilled, and the stream
   state 'os' (if non-NULL; it must belong to serialno) is reset so
   that continued decoding does not report a hole at the seek point.

   Returns OGG_SEEK_OK on success, OGG_SEEK_EINVAL for bad arguments,
   OGG_SEEK_ENOSEEK if the datasource is not seekable, OGG_SEEK_EREAD
   on read failure, OGG_SEEK_ENOSTREAM if no logical stream with the
   requested serial number exists, or OGG_SEEK_ERANGE if the target
   lies beyond the end of the stream.  On failure the datasource
   position is restored. */
int ogg_sync_seek(ogg_sync_state *oy, ogg_stream_state *os, long serialno,
                  ogg_int64_t target, const ogg_seek_callbacks *cb,
                  void *datasource){
  ogg_seek_fetch f;
  ogg_page og;
  ogg_int64_t save, length, begin, best, lo, hi, poff = 0;
  ogg_int64_t granule = -1, nogram = -1;
  int ret = OGG_SEEK_OK, r, found;

  if(!oy || !cb || !cb->read_func || !cb->seek_func || !cb->tell_func ||
     target < 0)
    return OGG_SEEK_EINVAL;

  /* the datasource must be seekable; also determine its length */
  save = cb->tell_func(datasource);
  if(save < 0) return OGG_SEEK_ENOSEEK;
  if(cb->seek_func(datasource, 0, OGG_SEEK_END)) return OGG_SEEK_ENOSEEK;
  length = cb->tell_func(datasource);
  if(length < 0){
    cb->seek_func(datasource, save, OGG_SEEK_SET);
    return OGG_SEEK_ENOSEEK;
  }

  f.cb = cb;
  f.datasource = datasource;
  f.oy = oy;
  f.offset = 0;
  f.limit = -1;

  /* Locate the first page of the requested logical stream.  In a
     grouped (multiplexed) stream this is among the header pages at
     the start of the file; in a chained stream it may be preceded by
     earlier chain segments. */
  begin = -1;
  if(_fetch_at(&f, 0, -1)){
    ret = OGG_SEEK_ENOSEEK;
    goto out;
  }
  while((r = _fetch_page(&f, &og, &poff)) > 0){
    if((ogg_uint32_t)ogg_page_serialno(&og) == (ogg_uint32_t)serialno){
      begin = poff;
      break;
    }
  }
  if(r < 0){
    ret = OGG_SEEK_EREAD;
    goto out;
  }
  if(begin < 0){
    ret = OGG_SEEK_ENOSTREAM;
    goto out;
  }

  /* Bisect the datasource for the transition across the target
     granule.  Invariant: 'best' (= 'lo') is the offset of a page of
     the target stream known to end before the target (or the first
     page of the stream), and the page we're looking for begins
     before 'hi'.  Pages that carry no ending granule (granulepos -1,
     eg. inside a packet spanning multiple pages) hold no information
     and are skipped. */
  lo = best = begin;
  hi = length;
  while(hi - lo > OGG_SEEK_CHUNK){
    ogg_int64_t probe = lo + (hi - lo)/2;
    ogg_int64_t limit = hi;

    /* If an earlier probe already established that no further granule
       positions of the target stream exist at/after 'nogram', don't
       scan past it again. */
    if(nogram >= 0 && nogram < limit) limit = nogram;

    if(_fetch_at(&f, probe, limit)){
      ret = OGG_SEEK_ENOSEEK;
      goto out;
    }
    found = 0;
    while((r = _fetch_page(&f, &og, &poff)) > 0){
      if((ogg_uint32_t)ogg_page_serialno(&og) != (ogg_uint32_t)serialno)
        continue;
      granule = ogg_page_granulepos(&og);
      if(granule == -1) continue;
      found = 1;
      break;
    }
    if(r < 0){
      ret = OGG_SEEK_EREAD;
      goto out;
    }

    if(found && granule < target){
      /* the page at poff still ends before the target */
      best = lo = poff;
    }else{
      /* the target is at or before the probe point (or no further
         granule positions exist beyond it) */
      hi = probe;
      if(!found && cb->tell_func(datasource) >= length)
        nogram = probe; /* scanned to EOF: no granule pages at/after
                           the probe point */
    }
  }

  /* Linear scan forward from the most recent known position before
     the target: move the resume point up to the last page whose
     ending granule is still below the target. */
  if(_fetch_at(&f, best, -1)){
    ret = OGG_SEEK_ENOSEEK;
    goto out;
  }
  for(;;){
    r = _fetch_page(&f, &og, &poff);
    if(r < 0){
      ret = OGG_SEEK_EREAD;
      goto out;
    }
    if(r == 0){
      /* end of data without ever reaching the target */
      ret = OGG_SEEK_ERANGE;
      goto out;
    }
    if((ogg_uint32_t)ogg_page_serialno(&og) != (ogg_uint32_t)serialno)
      continue;
    granule = ogg_page_granulepos(&og);
    if(granule != -1){
      if(granule < target){
        best = poff;
      }else{
        /* this page ends at or after the target; resume at the most
           recent page before it with a known granule position */
        break;
      }
    }
    if(ogg_page_eos(&og)){
      /* the stream ended before the target */
      ret = OGG_SEEK_ERANGE;
      goto out;
    }
  }

  /* Success.  Position at the resume point and leave the sync and
     stream states clean: the next page decoded begins exactly on a
     page boundary, so no spurious gap is reported at the seek
     point. */
  if(cb->seek_func(datasource, best, OGG_SEEK_SET)){
    ret = OGG_SEEK_ENOSEEK;
    goto out;
  }
  ogg_sync_reset(oy);
  if(os) ogg_stream_reset(os);
  return OGG_SEEK_OK;

 out:
  ogg_sync_reset(oy);
  cb->seek_func(datasource, save, OGG_SEEK_SET);
  return ret;
}
