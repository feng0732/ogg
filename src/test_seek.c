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

 function: self test for granulepos-based logical stream seeking

 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

#define READCHUNK 4096
#define MAXPAGES  20000
#define MAXPACKETS 20000

/* ----- in-memory datasource ------------------------------------- */

typedef struct {
  unsigned char *data;
  ogg_int64_t size;
  ogg_int64_t cap;
  ogg_int64_t pos;
  ogg_int64_t bytes_read;   /* total bytes handed out by read calls */
} memfile;

static size_t mem_read(void *ptr, size_t size, void *datasource){
  memfile *m = (memfile *)datasource;
  ogg_int64_t avail = m->size - m->pos;
  if(avail <= 0) return 0;
  if((ogg_int64_t)size > avail) size = (size_t)avail;
  memcpy(ptr, m->data + m->pos, size);
  m->pos += size;
  m->bytes_read += size;
  return size;
}

static int mem_seek(void *datasource, ogg_int64_t offset, int whence){
  memfile *m = (memfile *)datasource;
  ogg_int64_t base;
  switch(whence){
  case OGG_SEEK_SET: base = 0; break;
  case OGG_SEEK_CUR: base = m->pos; break;
  case OGG_SEEK_END: base = m->size; break;
  default: return -1;
  }
  offset += base;
  if(offset < 0 || offset > m->size) return -1;
  m->pos = offset;
  return 0;
}

static ogg_int64_t mem_tell(void *datasource){
  return ((memfile *)datasource)->pos;
}

static ogg_seek_callbacks mem_cb = { mem_read, mem_seek, mem_tell };

static void mem_append(memfile *m, const void *data, long len){
  if(m->size + len > m->cap){
    m->cap = (m->size + len)*2 + 4096;
    m->data = realloc(m->data, m->cap);
    if(!m->data){
      fprintf(stderr,"out of memory building test stream\n");
      exit(1);
    }
  }
  memcpy(m->data + m->size, data, len);
  m->size += len;
}

/* ----- reference model of the built file ------------------------- */

typedef struct {
  long serial;
  ogg_int64_t offset;
  ogg_int64_t granule;
} pagerec;

typedef struct {
  long serial;
  int n;
  long bytes[MAXPACKETS];
  ogg_int64_t granule[MAXPACKETS];
} refstream;

static pagerec pages[MAXPAGES];
static int npages;

static unsigned char packet_byte(int packetno, long j){
  return (unsigned char)((packetno + j) & 0xff);
}

/* submit one packet to the encoder and flush it into its own page
   (or several, if it spans); record page and packet reference data */
static void add_packet(memfile *f, ogg_stream_state *os, refstream *rs,
                       long bytes, ogg_int64_t granule, int eos){
  ogg_packet op;
  ogg_page og;
  unsigned char *data = malloc(bytes ? bytes : 1);
  long j;
  int idx = rs->n;

  if(!data){
    fprintf(stderr,"out of memory\n");
    exit(1);
  }
  for(j = 0; j < bytes; j++) data[j] = packet_byte(idx, j);

  memset(&op, 0, sizeof(op));
  op.packet = data;
  op.bytes = bytes;
  op.e_o_s = eos;
  op.granulepos = granule;
  op.b_o_s = (idx == 0);

  rs->bytes[idx] = bytes;
  /* the initial BOS page always carries granulepos 0, whatever the
     first packet claims; record what the decoder will actually see */
  rs->granule[idx] = (idx == 0) ? 0 : granule;
  rs->n++;

  if(ogg_stream_packetin(os, &op)){
    fprintf(stderr,"packetin failed\n");
    exit(1);
  }
  free(data);

  while(ogg_stream_flush(os, &og)){
    if(npages >= MAXPAGES){
      fprintf(stderr,"too many pages\n");
      exit(1);
    }
    pages[npages].serial = ogg_page_serialno(&og);
    pages[npages].offset = f->size;
    pages[npages].granule = ogg_page_granulepos(&og);
    npages++;
    mem_append(f, og.header, og.header_len);
    mem_append(f, og.body, og.body_len);
  }
}

static void build_stream(memfile *f, ogg_stream_state *os, refstream *rs,
                         long serial){
  ogg_stream_init(os, (int)serial);
  rs->serial = serial;
  rs->n = 0;
}

/* expected resume offset: the last page of the stream with a known
   granule position below the target, else the first page of the
   stream */
static ogg_int64_t expect_offset(refstream *rs, ogg_int64_t target){
  ogg_int64_t best = -1;
  int i;
  for(i = 0; i < npages; i++){
    if(pages[i].serial != rs->serial) continue;
    if(best < 0) best = pages[i].offset;
    if(pages[i].granule != -1){
      if(pages[i].granule < target)
        best = pages[i].offset;
      else
        break;
    }
  }
  return best;
}

/* index of the first packet whose ending granule is >= target */
static int first_target_packet(refstream *rs, ogg_int64_t target){
  int i;
  for(i = 0; i < rs->n; i++)
    if(rs->granule[i] >= target) return i;
  return -1;
}

/* ----- post-seek decode verification ----------------------------- */

static int packet_matches(refstream *rs, int idx, ogg_packet *op){
  long j;
  if(idx >= rs->n) return 0;
  if(op->bytes != rs->bytes[idx]) return 0;
  for(j = 0; j < op->bytes; j++)
    if(op->packet[j] != packet_byte(idx, j)) return 0;
  return 1;
}

/* Continue demuxing from the current datasource position using the
   (freshly seeked) sync and stream states.  Verifies that no hole is
   reported, that the emitted packets form an unbroken suffix of the
   reference stream, and that the first packet with an ending granule
   >= target is among them. */
static int demux_verify(memfile *m, ogg_sync_state *oy, ogg_stream_state *os,
                        refstream *rs, ogg_int64_t target){
  ogg_page og;
  ogg_packet op;
  int k = -1;              /* reference index of the next packet */
  int fti = first_target_packet(rs, target);
  int saw_target = 0;
  int done = 0;

  if(fti < 0){
    fprintf(stderr,"bad test: no target packet\n");
    return 1;
  }

  while(!done){
    int ret = ogg_sync_pageout(oy, &og);
    if(ret == 0){
      char *buf = ogg_sync_buffer(oy, READCHUNK);
      size_t n = mem_read(buf, READCHUNK, m);
      if(n == 0){
        fprintf(stderr,"ran out of data before EOS packet\n");
        return 1;
      }
      ogg_sync_wrote(oy, (long)n);
      continue;
    }
    if(ret < 0){
      fprintf(stderr,"sync layer reported a hole after seek\n");
      return 1;
    }
    if(ogg_page_serialno(&og) != rs->serial) continue; /* other streams */
    if(ogg_stream_pagein(os, &og)){
      fprintf(stderr,"pagein failed\n");
      return 1;
    }
    for(;;){
      ret = ogg_stream_packetout(os, &op);
      if(ret == 0) break;
      if(ret < 0){
        fprintf(stderr,"stream layer reported a hole after seek\n");
        return 1;
      }
      if(k < 0){
        /* first packet: locate it in the reference stream */
        int i;
        for(i = 0; i < rs->n; i++)
          if(packet_matches(rs, i, &op)) break;
        if(i == rs->n){
          fprintf(stderr,"first packet after seek matches no reference packet\n");
          return 1;
        }
        k = i;
        if(k > fti){
          fprintf(stderr,"first target packet was skipped (k=%d fti=%d)\n",
                  k, fti);
          return 1;
        }
      }else{
        if(!packet_matches(rs, k, &op)){
          fprintf(stderr,"packet %d data mismatch after seek\n", k);
          return 1;
        }
      }
      /* in this test layout every packet terminates its page, so every
         decoded packet carries its encoding granule */
      if(op.granulepos != rs->granule[k]){
        fprintf(stderr,"packet %d granule mismatch: %ld != %ld\n", k,
                (long)op.granulepos, (long)rs->granule[k]);
        return 1;
      }
      if(k == fti){
        if(op.granulepos < target){
          fprintf(stderr,"target packet granule below target\n");
          return 1;
        }
        saw_target = 1;
      }
      if(op.e_o_s) done = 1;
      k++;
      if(k > rs->n){
        fprintf(stderr,"more packets than reference\n");
        return 1;
      }
    }
  }

  if(k != rs->n){
    fprintf(stderr,"decode stopped early: %d of %d packets\n", k, rs->n);
    return 1;
  }
  if(!saw_target){
    fprintf(stderr,"first packet with granule >= target not emitted\n");
    return 1;
  }
  return 0;
}

/* ----- test driver ------------------------------------------------ */

static int failures = 0;

static void check(int cond, const char *what){
  if(!cond){
    fprintf(stderr,"FAIL: %s\n", what);
    failures++;
  }
}

/* run one successful seek and verify position, read budget and the
   following decode */
static void test_seek_ok(memfile *f, refstream *rs, ogg_int64_t target,
                         ogg_int64_t budget){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_int64_t expect = expect_offset(rs, target);
  int ret;
  char msg[128];

  ogg_sync_init(&oy);
  ogg_stream_init(&os, (int)rs->serial);
  f->pos = 0;
  f->bytes_read = 0;

  ret = ogg_sync_seek(&oy, &os, rs->serial, target, &mem_cb, f);

  snprintf(msg, sizeof(msg), "seek to %ld returns OK (got %d)",
           (long)target, ret);
  check(ret == OGG_SEEK_OK, msg);

  if(ret == OGG_SEEK_OK){
    snprintf(msg, sizeof(msg),
             "seek to %ld lands at %ld (expected %ld)",
             (long)target, (long)mem_tell(f), (long)expect);
    check(mem_tell(f) == expect, msg);

    if(budget >= 0){
      snprintf(msg, sizeof(msg),
               "seek to %ld reads %ld bytes (budget %ld)",
               (long)target, (long)f->bytes_read, (long)budget);
      check(f->bytes_read <= budget, msg);
    }

    if(demux_verify(f, &oy, &os, rs, target)){
      fprintf(stderr,"  ... while verifying decode after seek to %ld\n",
              (long)target);
      failures++;
    }
  }

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
}

static void test_seek_err(memfile *f, long serial, ogg_int64_t target,
                          int expect_ret){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_int64_t saved;
  int ret;
  char msg[128];

  ogg_sync_init(&oy);
  ogg_stream_init(&os, (int)serial);
  f->pos = 123; /* somewhere the seek must restore on failure */
  if(f->pos > f->size) f->pos = 0;
  saved = f->pos;

  ret = ogg_sync_seek(&oy, &os, serial, target, &mem_cb, f);

  snprintf(msg, sizeof(msg), "seek serial %ld target %ld returns %d (got %d)",
           serial, (long)target, expect_ret, ret);
  check(ret == expect_ret, msg);
  snprintf(msg, sizeof(msg), "position restored after failed seek");
  check(mem_tell(f) == saved, msg);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
}

int main(void){
  memfile f1;   /* multiplexed: streams A and B interleaved */
  memfile f2;   /* chained: stream A2, then stream C */
  refstream ra, rb, ra2, rc;
  ogg_stream_state os_a, os_b;
  int i;

  memset(&f1, 0, sizeof(f1));
  memset(&f2, 0, sizeof(f2));

  /* ---- file 1: grouped streams A (serial 0x111) and B (0x222) ---- */
  fprintf(stderr,"building multiplexed test stream... ");
  npages = 0;
  build_stream(&f1, &os_a, &ra, 0x111);
  build_stream(&f1, &os_b, &rb, 0x222);

#define NA 3000
#define NB 500
#define BIGPKT 1500   /* index of a packet spanning several pages */
#define NILPKT 700    /* index of a zero length packet */

  add_packet(&f1, &os_a, &ra, 100, 0, 0);  /* BOS pages of both */
  add_packet(&f1, &os_b, &rb, 60, 2, 0);   /* streams come first */
  for(i = 1; i < NA; i++){
    long bytes = 50 + (i*7919L) % 1200;
    if(i == BIGPKT) bytes = 150000;  /* spans multiple pages */
    if(i == NILPKT) bytes = 0;
    add_packet(&f1, &os_a, &ra, bytes, (ogg_int64_t)i*100, i == NA-1);
    /* stream B is interleaved with A throughout the whole file */
    if(i % 6 == 0 && i/6 < NB){
      int j = i/6;
      add_packet(&f1, &os_b, &rb, 20 + (j*31L) % 300,
                 (ogg_int64_t)j*13 + 2, j == NB-1);
    }
  }
  ogg_stream_clear(&os_a);
  ogg_stream_clear(&os_b);
  fprintf(stderr,"%ld bytes, %d pages\n", (long)f1.size, npages);

  /* ---- seeks within the multiplexed stream ----------------------- */
  fprintf(stderr,"testing seeks (multiplexed)...\n");

  /* read budget: the file is ~2MB; a logarithmic seek must need far
     less than a linear scan */
  {
    ogg_int64_t budget = f1.size/4;
    test_seek_ok(&f1, &ra, 0, budget);                 /* very beginning */
    test_seek_ok(&f1, &ra, 15000, budget);             /* exact granule */
    test_seek_ok(&f1, &ra, 15001, budget);             /* just past one */
    test_seek_ok(&f1, &ra, 70000, budget);             /* past nil packet */
    test_seek_ok(&f1, &ra, 150000, budget);            /* spanning packet */
    test_seek_ok(&f1, &ra, 149999, budget);            /* just before it */
    test_seek_ok(&f1, &ra, 250000, budget);            /* mid stream */
    test_seek_ok(&f1, &ra, (NA-1)*100, budget);        /* last granule */
    test_seek_ok(&f1, &rb, 200*13+2, budget);          /* other stream */
    test_seek_ok(&f1, &rb, (NB-1)*13+2, budget);       /* its last granule */
  }

  /* seek without a stream state, then seek again on the same states */
  {
    ogg_sync_state oy;
    ogg_stream_state os;
    ogg_int64_t expect;
    int ret;
    ogg_sync_init(&oy);
    ogg_stream_init(&os, 0x111);
    f1.pos = 0;
    ret = ogg_sync_seek(&oy, NULL, 0x111, 250000, &mem_cb, &f1);
    check(ret == OGG_SEEK_OK, "seek with NULL stream state succeeds");
    expect = expect_offset(&ra, 250000);
    check(ret == OGG_SEEK_OK && mem_tell(&f1) == expect,
          "seek with NULL stream state lands correctly");
    /* a second seek on the same states must work just as well */
    ret = ogg_sync_seek(&oy, &os, 0x111, 15000, &mem_cb, &f1);
    check(ret == OGG_SEEK_OK, "repeated seek on same states succeeds");
    expect = expect_offset(&ra, 15000);
    check(ret == OGG_SEEK_OK && mem_tell(&f1) == expect,
          "repeated seek lands correctly");
    ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
  }

  /* ---- file 2: chained segments A2 (0x444) then C (0x333) -------- */
  fprintf(stderr,"building chained test stream... ");
  npages = 0;
  build_stream(&f2, &os_a, &ra2, 0x444);
  for(i = 0; i < 60; i++)
    add_packet(&f2, &os_a, &ra2, 30 + (i*17L) % 200,
               (ogg_int64_t)i*10, i == 59);
  ogg_stream_clear(&os_a);

  build_stream(&f2, &os_b, &rc, 0x333);
  for(i = 0; i < 90; i++)
    add_packet(&f2, &os_b, &rc, 40 + (i*23L) % 250,
               (ogg_int64_t)i*20 + 5, i == 89);
  ogg_stream_clear(&os_b);
  fprintf(stderr,"%ld bytes, %d pages\n", (long)f2.size, npages);

  /* ---- seeks within chained segments ----------------------------- */
  fprintf(stderr,"testing seeks (chained)...\n");
  test_seek_ok(&f2, &ra2, 250, -1);        /* first chain segment */
  test_seek_ok(&f2, &ra2, 590, -1);        /* its last granule */
  test_seek_ok(&f2, &rc, 0, -1);           /* second chain segment */
  test_seek_ok(&f2, &rc, 805, -1);         /* mid second segment */
  test_seek_ok(&f2, &rc, 89*20+5, -1);     /* its last granule */

  /* ---- error conditions ------------------------------------------- */
  fprintf(stderr,"testing error conditions...\n");
  test_seek_err(&f1, 0x999, 100, OGG_SEEK_ENOSTREAM); /* no such stream */
  test_seek_err(&f1, 0x111, (NA-1)*100+1, OGG_SEEK_ERANGE); /* too far */
  test_seek_err(&f1, 0x111, -5, OGG_SEEK_EINVAL);     /* negative target */
  test_seek_err(&f2, 0x333, 89*20+6, OGG_SEEK_ERANGE);/* past chain end */

  /* NULL / incomplete callback sets */
  {
    ogg_sync_state oy;
    ogg_seek_callbacks bad = mem_cb;
    int ret;
    ogg_sync_init(&oy);
    bad.tell_func = NULL;
    ret = ogg_sync_seek(&oy, NULL, 0x111, 100, &bad, &f1);
    check(ret == OGG_SEEK_EINVAL, "NULL tell callback rejected");
    ret = ogg_sync_seek(&oy, NULL, 0x111, 100, NULL, &f1);
    check(ret == OGG_SEEK_EINVAL, "NULL callbacks rejected");
    ogg_sync_clear(&oy);
  }

  free(f1.data);
  free(f2.data);

  if(failures){
    fprintf(stderr,"%d seek test(s) FAILED\n", failures);
    return 1;
  }
  fprintf(stderr,"seek tests ok.\n");
  return 0;
}
