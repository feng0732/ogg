/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE Ogg CONTAINER SOURCE CODE.              *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * (C) COPYRIGHT 2026 by the Xiph.Org Foundation                     *
 * http://www.xiph.org/                                             *
 *                                                                  *
 ********************************************************************

 function: unit tests for page boundaries, continued packets,
           packet completion state and granule position

 These tests drive the public decoder API (ogg_sync_* / ogg_stream_*)
 with deterministic, byte-exact pages. They pin down the contract that
 the page boundary -- not the body of a page taken in isolation --
 decides whether a continued packet may be delivered:

   * a packet that spans pages and shares its finishing page with other
     complete packets is reassembled exactly once; only the last packet
     completed on a page carries that page's granulepos, and only the
     last packet completed on an eos page is flagged eos;
   * a legal page whose final packet is merely incomplete is not a
     corrupt page: sync delivers it, pagein succeeds, and no packet is
     emitted until the continuation page arrives;
   * a stream that ends at an incomplete packet reports "no complete
     packet" rather than fabricating one;
   * a missing continuation page is reported as a gap and the partial
     packet is rolled back -- no packet is fabricated from it;
   * a page that fails the CRC check is rejected by sync and no packet
     is ever emitted from its data;
   * a page that claims a continuation without a pending packet has its
     untrusted leading segments discarded.

 Every page used here is constructed in-process from fixed lacing
 tables and a fixed byte pattern, so the tests are offline and
 reproducible: no network, no clock, no external files.

 ********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>

#define SERIALNO 0x11223344

static int checks_run;
static int checks_failed;
static int failed_before;

#define CHECK(cond) do { checks_run++; \
  if(!(cond)){ fprintf(stderr,"FAIL (line %d): %s\n",__LINE__,#cond); \
               checks_failed++; } } while(0)

static void check_long_at(int line,const char *what,long got,long want){
  checks_run++;
  if(got!=want){
    fprintf(stderr,"FAIL (line %d): %s: got %ld, want %ld\n",line,what,got,want);
    checks_failed++;
  }
}

#define CHECK_LONG(what,got,want) \
  check_long_at(__LINE__,what,(long)(got),(long)(want))

static void test_begin(const char *name){
  fprintf(stderr,"test: %s... ",name);
  failed_before=checks_failed;
}

static void test_end(void){
  int failed=checks_failed-failed_before;
  if(failed==0)
    fprintf(stderr,"ok\n");
  else
    fprintf(stderr,"FAILED (%d check%s)\n",failed,failed==1?"":"s");
}

/* Deterministic body bytes. The range 0x10..0x2f deliberately avoids
   'O' (0x4f) so that the resynchronisation search performed after a
   corrupt page cannot latch onto body data (it may, however, run past
   the damaged page and find the next real capture pattern, which is
   exactly what the library is supposed to do). */
static void pattern(unsigned char *dst,long n,int seed){
  long i;
  for(i=0;i<n;i++)
    dst[i]=(unsigned char)(0x10+((seed+i)%0x20));
}

/* Build one complete page at 'out'. Returns the page length, or -1 if
   the supplied buffer is too small. The CRC is filled in. */
static long build_page(unsigned char *out,long cap,int version,int flags,
                       ogg_int64_t granulepos,long pageno,
                       const unsigned char *segtable,int nsegs,
                       const unsigned char *body,long bodylen){
  long i,pos=27;
  ogg_page og;
  ogg_uint64_t gp=(ogg_uint64_t)granulepos;

  if(cap<27+nsegs+bodylen)return(-1);

  memcpy(out,"OggS",4);
  out[4]=(unsigned char)version;
  out[5]=(unsigned char)flags;
  for(i=0;i<8;i++)out[6+i]=(unsigned char)((gp>>(8*i))&0xff);
  for(i=0;i<4;i++)out[14+i]=(unsigned char)((SERIALNO>>(8*i))&0xff);
  for(i=0;i<4;i++)
    out[18+i]=(unsigned char)(((ogg_uint32_t)pageno>>(8*i))&0xff);
  out[22]=out[23]=out[24]=out[25]=0;
  out[26]=(unsigned char)nsegs;
  for(i=0;i<nsegs;i++)out[pos++]=(unsigned char)segtable[i];
  memcpy(out+pos,body,bodylen);

  og.header=out;
  og.header_len=pos;
  og.body=out+pos;
  og.body_len=bodylen;
  ogg_page_checksum_set(&og);

  return(pos+bodylen);
}

/* Append bytes to the sync buffer. */
static int push(ogg_sync_state *oy,const unsigned char *data,long len){
  char *buf=ogg_sync_buffer(oy,len);
  if(!buf)return(-1);
  memcpy(buf,data,len);
  return ogg_sync_wrote(oy,len);
}

static int sync_page_at(int line,const char *what,ogg_sync_state *oy,
                        ogg_page *og,int want){
  int got;
  checks_run++;
  got=ogg_sync_pageout(oy,og);
  if(got!=want){
    fprintf(stderr,"FAIL (line %d): %s: got %d, want %d\n",line,what,got,want);
    checks_failed++;
    return(0);
  }
  return(1);
}

#define SYNC_PAGE(what,oy,og,want) \
  sync_page_at(__LINE__,what,oy,og,want)

/* Submit a synced page to the stream layer. */
static int pagein_at(int line,const char *what,ogg_stream_state *os,
                     ogg_page *og){
  int got=ogg_stream_pagein(os,og);
  checks_run++;
  if(got!=0){
    fprintf(stderr,"FAIL (line %d): %s: got %d, want 0\n",line,what,got);
    checks_failed++;
    return(0);
  }
  return(1);
}

#define PAGEIN(what,os,og) pagein_at(__LINE__,what,os,og)

/* Pull one packet and compare every observable property. If the call
   does not return a packet, the packet buffer is not touched -- this
   keeps the test from dereferencing an unfilled ogg_packet. */
static void check_packet_at(int line,const char *what,ogg_stream_state *os,
                            long want_bytes,int want_bos,int want_eos,
                            long want_gp,const unsigned char *want,
                            long want_len){
  ogg_packet op;
  int got;

  checks_run++;
  got=ogg_stream_packetout(os,&op);
  if(got!=1){
    fprintf(stderr,"FAIL (line %d): %s: got %d, want 1\n",line,what,got);
    checks_failed++;
    return;
  }

  check_long_at(line,what,op.bytes,want_bytes);
  CHECK_LONG("packet b_o_s presence",op.b_o_s?1:0,want_bos);
  CHECK_LONG("packet e_o_s presence",op.e_o_s?1:0,want_eos);
  CHECK_LONG("packet granulepos",op.granulepos,want_gp);

  checks_run++;
  if(want_len!=op.bytes){
    fprintf(stderr,"FAIL (line %d): %s: body length %ld, want %ld\n",
            line,what,op.bytes,want_len);
    checks_failed++;
    return;
  }
  {
    long i;
    for(i=0;i<want_len;i++){
      if(op.packet[i]!=want[i]){
        checks_run++;
        fprintf(stderr,"FAIL (line %d): %s: byte %ld is 0x%02x, want 0x%02x\n",
                line,what,i,(unsigned)op.packet[i],(unsigned)want[i]);
        checks_failed++;
        return;
      }
    }
  }
}

#define CHECK_PACKET(what,os,bytes,bos,eos,gp,body,len) \
  check_packet_at(__LINE__,what,os,bytes,bos,eos,gp,body,len)

/* Assert the exact return value of packetout, without consuming packet
   fields when no packet is produced. */
static void expect_packetout_at(int line,const char *what,
                               ogg_stream_state *os,int want){
  ogg_packet op;
  int got;
  checks_run++;
  got=ogg_stream_packetout(os,&op);
  if(got!=want){
    fprintf(stderr,"FAIL (line %d): %s: got %d, want %d\n",line,what,got,want);
    checks_failed++;
  }
}

#define EXPECT_PACKETOUT(what,os,want) \
  expect_packetout_at(__LINE__,what,os,want)

static void expect_peek_at(int line,const char *what,
                           ogg_stream_state *os,int want){
  int got;
  checks_run++;
  got=ogg_stream_packetpeek(os,NULL);
  if(got!=want){
    fprintf(stderr,"FAIL (line %d): %s: got %d, want %d\n",line,what,got,want);
    checks_failed++;
  }
}

#define EXPECT_PEEK(what,os,want) expect_peek_at(__LINE__,what,os,want)

/* ------------------------------------------------------------------ */

/* A packet that spans pages and finishes on a page that also carries
   complete packets must be reassembled exactly once, must not steal the
   granulepos of the packets that follow it, and only the last packet
   completed on the page owns the page granulepos / eos flag. */
static void test_continued_shares_page_with_complete_packets(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024];
  unsigned char body0[17],body1[510],body2[100],expect[560];
  const unsigned char seg0[1]={17};
  const unsigned char seg1[2]={255,255};
  const unsigned char seg2[3]={50,20,30};
  long n;

  test_begin("continued packet sharing a page with complete packets");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);

  pattern(body0,sizeof(body0),0);
  pattern(body1,sizeof(body1),1);
  pattern(body2,sizeof(body2),2);

  /* page 0: BOS page completing one packet */
  n=build_page(page,sizeof(page),0,0x02,0,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  CHECK_LONG("push page 0",push(&oy,page,n),0);
  SYNC_PAGE("sync page 0",&oy,&og,1);
  PAGEIN("pagein page 0",&os,&og);
  CHECK_PACKET("P0",&os,17,1,0,0,body0,17);

  /* page 1: begins a 560 byte packet but ends incomplete. This is a
     legal page: continued==0, no packet completes and granulepos is -1. */
  n=build_page(page,sizeof(page),0,0x00,-1,1,seg1,2,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push page 1",push(&oy,page,n),0);
  SYNC_PAGE("sync page 1",&oy,&og,1);
  CHECK_LONG("page 1 continued flag",ogg_page_continued(&og),0);
  CHECK_LONG("page 1 complete packets",ogg_page_packets(&og),0);
  CHECK_LONG("page 1 granulepos",ogg_page_granulepos(&og),-1);
  PAGEIN("pagein page 1",&os,&og);
  EXPECT_PACKETOUT("no packet while incomplete",&os,0);
  EXPECT_PEEK("no packetpeek while incomplete",&os,0);

  /* page 2: finishes the spanned packet, then completes two more. The
     page granulepos belongs to the last packet completed here. */
  n=build_page(page,sizeof(page),0,0x01|0x04,200,2,seg2,3,body2,sizeof(body2));
  CHECK(n>0);
  CHECK_LONG("push page 2",push(&oy,page,n),0);
  SYNC_PAGE("sync page 2",&oy,&og,1);
  CHECK_LONG("page 2 continued flag",ogg_page_continued(&og),1);
  CHECK_LONG("page 2 eos flag",ogg_page_eos(&og),4);
  CHECK_LONG("page 2 complete packets",ogg_page_packets(&og),3);
  CHECK_LONG("page 2 granulepos",ogg_page_granulepos(&og),200);
  PAGEIN("pagein page 2",&os,&og);

  memcpy(expect,body1,sizeof(body1));
  memcpy(expect+sizeof(body1),body2,50);

  CHECK_PACKET("P1 spans pages",&os,560,0,0,-1,expect,560);
  CHECK_PACKET("P2 middle packet",&os,20,0,0,-1,body2+50,20);
  CHECK_PACKET("P3 owns page granulepos",&os,30,0,1,200,body2+70,30);

  EXPECT_PACKETOUT("no packet left",&os,0);
  CHECK_LONG("stream reports eos",ogg_stream_eos(&os),1);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}

/* A stream that ends at an incomplete packet is not corrupt: the page
   itself decodes (pagein returns 0), but no packet is produced, no eos
   is reported, and sync does not treat the tail as a resync hole. */
static void test_stream_ends_at_incomplete_packet(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024];
  unsigned char body0[17],body1[510];
  const unsigned char seg0[1]={17};
  const unsigned char seg1[2]={255,255};
  long n;

  test_begin("stream ending at an incomplete packet");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);
  pattern(body0,sizeof(body0),3);
  pattern(body1,sizeof(body1),4);

  n=build_page(page,sizeof(page),0,0x02,0,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  CHECK_LONG("push page 0",push(&oy,page,n),0);
  n=build_page(page,sizeof(page),0,0x00,-1,1,seg1,2,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push page 1",push(&oy,page,n),0);

  SYNC_PAGE("sync page 0",&oy,&og,1);
  PAGEIN("pagein page 0",&os,&og);
  CHECK_PACKET("P0",&os,17,1,0,0,body0,17);

  /* The page with the incomplete tail must sync as a page (return 1),
     never as a resync/hole (negative return). */
  SYNC_PAGE("legal incomplete page syncs",&oy,&og,1);
  CHECK_LONG("legal incomplete page is not continued",
             ogg_page_continued(&og),0);
  CHECK_LONG("legal incomplete page completes nothing",
             ogg_page_packets(&og),0);
  PAGEIN("legal incomplete page pagein",&os,&og);

  EXPECT_PACKETOUT("no packet from incomplete tail",&os,0);
  EXPECT_PEEK("no packetpeek from incomplete tail",&os,0);
  CHECK_LONG("stream is not eos",ogg_stream_eos(&os),0);
  SYNC_PAGE("sync needs more data at end",&oy,&og,0);
  EXPECT_PACKETOUT("still no packet at end",&os,0);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}

/* If the continuation page is missing, the partial packet is rolled
   back and a gap is reported; the complete packets on the next page are
   still delivered and keep their own granulepos. */
static void test_missing_continuation_page(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024];
  unsigned char body0[17],body1[510],body3[30];
  const unsigned char seg0[1]={17};
  const unsigned char seg1[2]={255,255};
  const unsigned char seg3[2]={10,20};
  long n;

  test_begin("missing continuation page reports a gap");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);
  pattern(body0,sizeof(body0),5);
  pattern(body1,sizeof(body1),6);
  pattern(body3,sizeof(body3),7);

  n=build_page(page,sizeof(page),0,0x02,0,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  CHECK_LONG("push page 0",push(&oy,page,n),0);
  n=build_page(page,sizeof(page),0,0x00,-1,1,seg1,2,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push page 1",push(&oy,page,n),0);
  /* page 2 is missing entirely */
  n=build_page(page,sizeof(page),0,0x00,400,3,seg3,2,body3,sizeof(body3));
  CHECK(n>0);
  CHECK_LONG("push page 3",push(&oy,page,n),0);

  SYNC_PAGE("sync page 0",&oy,&og,1);
  PAGEIN("pagein page 0",&os,&og);
  CHECK_PACKET("P0",&os,17,1,0,0,body0,17);

  SYNC_PAGE("sync page 1",&oy,&og,1);
  PAGEIN("pagein page 1",&os,&og);
  EXPECT_PACKETOUT("no packet from partial packet",&os,0);

  SYNC_PAGE("sync page 3",&oy,&og,1);
  PAGEIN("pagein page 3",&os,&og);

  EXPECT_PACKETOUT("gap reported for lost continuation",&os,-1);
  CHECK_PACKET("P3",&os,10,0,0,-1,body3,10);
  CHECK_PACKET("P4",&os,20,0,0,400,body3+10,20);
  EXPECT_PACKETOUT("no packet left",&os,0);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}

#ifndef DISABLE_CRC
/* A page with a bad CRC is rejected by sync: it is never handed to the
   stream layer, so it cannot produce a packet. Because the stream never
   saw the damaged page, the following page is out of sequence and the
   stream correctly reports a gap before the packet it carries. */
static void test_corrupt_page_is_skipped(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024],corrupt[1024];
  unsigned char body0[10],body1[5];
  const unsigned char seg0[1]={10};
  const unsigned char seg1[1]={5};
  long n,nc;

  test_begin("corrupt page rejected without producing packets");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);
  pattern(body0,sizeof(body0),8);
  pattern(body1,sizeof(body1),9);

  n=build_page(page,sizeof(page),0,0x02,100,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  memcpy(corrupt,page,n);
  nc=n;
  corrupt[27+1]^=0xff; /* damage a body byte, keep the stale CRC */

  CHECK_LONG("push corrupt page",push(&oy,corrupt,nc),0);
  SYNC_PAGE("corrupt page is a hole",&oy,&og,-1);
  SYNC_PAGE("no page after corrupt page",&oy,&og,0);
  EXPECT_PACKETOUT("corrupt page yields no packet",&os,0);
  EXPECT_PEEK("corrupt page yields no peek",&os,0);

  /* Still able to decode a following valid page. */
  n=build_page(page,sizeof(page),0,0x00,101,1,seg1,1,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push valid page",push(&oy,page,n),0);
  SYNC_PAGE("valid page still syncs",&oy,&og,1);
  PAGEIN("pagein valid page",&os,&og);
  EXPECT_PACKETOUT("gap because page 0 was never delivered",&os,-1);
  CHECK_PACKET("valid packet",&os,5,0,0,101,body1,5);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}

/* A corrupt continuation page must not complete the pending packet:
   the continuation is lost, the gap is reported, and none of the
   corrupt page's bytes reach a packet. */
static void test_corrupt_continuation_yields_no_packet(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024],corrupt[1024];
  unsigned char body0[17],body1[510],bad[60],body3[30];
  const unsigned char seg0[1]={17};
  const unsigned char seg1[2]={255,255};
  const unsigned char segbad[2]={50,10};
  const unsigned char seg3[2]={10,20};
  long n,nc;

  test_begin("corrupt continuation cannot complete a packet");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);
  pattern(body0,sizeof(body0),10);
  pattern(body1,sizeof(body1),11);
  pattern(bad,sizeof(bad),12);
  pattern(body3,sizeof(body3),13);

  n=build_page(page,sizeof(page),0,0x02,0,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  CHECK_LONG("push page 0",push(&oy,page,n),0);
  n=build_page(page,sizeof(page),0,0x00,-1,1,seg1,2,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push page 1",push(&oy,page,n),0);

  SYNC_PAGE("sync page 0",&oy,&og,1);
  PAGEIN("pagein page 0",&os,&og);
  CHECK_PACKET("P0",&os,17,1,0,0,body0,17);
  SYNC_PAGE("sync page 1",&oy,&og,1);
  PAGEIN("pagein page 1",&os,&og);
  EXPECT_PACKETOUT("no packet from partial packet",&os,0);

  /* page 2 would have finished the spanned packet; corrupt it. */
  n=build_page(page,sizeof(page),0,0x01,500,2,segbad,2,bad,sizeof(bad));
  CHECK(n>0);
  memcpy(corrupt,page,n);
  nc=n;
  corrupt[27+2]^=0xff; /* damage the first continuation body byte */

  CHECK_LONG("push corrupt continuation",push(&oy,corrupt,nc),0);
  SYNC_PAGE("corrupt continuation is a hole",&oy,&og,-1);
  SYNC_PAGE("no page from corrupt continuation",&oy,&og,0);
  EXPECT_PACKETOUT("no packet from corrupt continuation",&os,0);
  EXPECT_PEEK("no peek from corrupt continuation",&os,0);

  /* A later complete page is still usable: the stream reports a gap and
     delivers only the packets actually present on that page. */
  n=build_page(page,sizeof(page),0,0x00,600,3,seg3,2,body3,sizeof(body3));
  CHECK(n>0);
  CHECK_LONG("push page 3",push(&oy,page,n),0);
  SYNC_PAGE("sync page 3",&oy,&og,1);
  PAGEIN("pagein page 3",&os,&og);
  EXPECT_PACKETOUT("gap after lost continuation",&os,-1);
  CHECK_PACKET("P3",&os,10,0,0,-1,body3,10);
  CHECK_PACKET("P4",&os,20,0,0,600,body3+10,20);
  EXPECT_PACKETOUT("no packet left",&os,0);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}
#endif /* DISABLE_CRC */

/* A page that sets the continued flag although no packet is pending
   has its untrusted leading segments discarded; only packets fully
   contained in the page are delivered. */
static void test_continued_without_pending_packet(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024];
  unsigned char body0[17],body1[30];
  const unsigned char seg0[1]={17};
  const unsigned char seg1[2]={10,20};
  long n;

  test_begin("continued page without a pending packet");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);
  pattern(body0,sizeof(body0),14);
  pattern(body1,sizeof(body1),15);

  n=build_page(page,sizeof(page),0,0x02,0,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  CHECK_LONG("push page 0",push(&oy,page,n),0);
  SYNC_PAGE("sync page 0",&oy,&og,1);
  PAGEIN("pagein page 0",&os,&og);

  n=build_page(page,sizeof(page),0,0x01,123,1,seg1,2,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push continued page",push(&oy,page,n),0);
  SYNC_PAGE("sync continued page",&oy,&og,1);
  CHECK_LONG("continued page flag",ogg_page_continued(&og),1);
  PAGEIN("pagein continued page",&os,&og);

  CHECK_PACKET("P0",&os,17,1,0,0,body0,17);

  /* Only the 20 byte packet is complete on this page; the leading 10
     bytes are dropped as an orphaned continuation. */
  CHECK_PACKET("orphan continuer dropped",&os,20,0,0,123,body1+10,20);
  EXPECT_PACKETOUT("no packet left",&os,0);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}

/* A packet whose length is an exact multiple of 255 needs a zero lacing
   value to terminate. When the span crosses a page boundary, the zero
   termination in the continuation page completes the packet; it must
   not be dropped as an empty packet of its own. */
static void test_zero_termination_across_pages(void){
  ogg_sync_state oy;
  ogg_stream_state os;
  ogg_page og;
  unsigned char page[1024];
  unsigned char body0[17],body1[255],body2[5];
  const unsigned char seg0[1]={17};
  const unsigned char seg1[1]={255};
  const unsigned char seg2[2]={0,5};
  long n;

  test_begin("zero termination of a 255 byte packet across pages");

  ogg_sync_init(&oy);
  ogg_stream_init(&os,SERIALNO);
  pattern(body0,sizeof(body0),16);
  pattern(body1,sizeof(body1),17);
  pattern(body2,sizeof(body2),18);

  n=build_page(page,sizeof(page),0,0x02,0,0,seg0,1,body0,sizeof(body0));
  CHECK(n>0);
  CHECK_LONG("push page 0",push(&oy,page,n),0);
  n=build_page(page,sizeof(page),0,0x00,-1,1,seg1,1,body1,sizeof(body1));
  CHECK(n>0);
  CHECK_LONG("push page 1",push(&oy,page,n),0);
  n=build_page(page,sizeof(page),0,0x01|0x04,777,2,seg2,2,body2,sizeof(body2));
  CHECK(n>0);
  CHECK_LONG("push page 2",push(&oy,page,n),0);

  SYNC_PAGE("sync page 0",&oy,&og,1);
  PAGEIN("pagein page 0",&os,&og);
  CHECK_PACKET("P0",&os,17,1,0,0,body0,17);

  SYNC_PAGE("sync page 1",&oy,&og,1);
  PAGEIN("pagein page 1",&os,&og);
  EXPECT_PACKETOUT("no packet while unterminated",&os,0);

  SYNC_PAGE("sync page 2",&oy,&og,1);
  CHECK_LONG("page 2 complete packets",ogg_page_packets(&og),2);
  PAGEIN("pagein page 2",&os,&og);

  CHECK_PACKET("P1 zero-terminated across pages",&os,255,0,0,-1,body1,255);
  CHECK_PACKET("P2",&os,5,0,1,777,body2,5);
  EXPECT_PACKETOUT("no packet left",&os,0);

  ogg_stream_clear(&os);
  ogg_sync_clear(&oy);
  test_end();
}

/* Round-trip: the pages the encoder emits for a packet that spans page
   boundaries must agree with what the decoder reports. In particular,
   a page starts a continuation exactly when the previous page ended
   with an unterminated segment, a page that completes no packet has
   granulepos -1, and the last packet completed on a page owns that
   page's granulepos -- so reassembled packet granule positions match
   the values fed to the encoder. */
static void test_encoder_decoder_span_roundtrip(void){
  ogg_stream_state os_en, os_de;
  ogg_sync_state oy;
  ogg_page og;
  ogg_packet op;
  const long sizes[3]={17,70000,30};
  const ogg_int64_t gps[3]={10,20,30};
  long total=0;
  long pageno=0;
  int prev_incomplete=0;
  int next_packet=0;
  int i,j;

  test_begin("encoder/decoder continued-page round trip");

  for(i=0;i<3;i++)total+=sizes[i];
  {
    unsigned char *data=malloc((size_t)total);
    long dofs=0;
    CHECK(data!=NULL);
    if(!data){
      test_end();
      return;
    }
    for(i=0;i<(int)total;i++)data[i]=(unsigned char)(0x10+(i%0x20));

    ogg_stream_init(&os_en,SERIALNO);
    ogg_stream_init(&os_de,SERIALNO);
    ogg_sync_init(&oy);

    {
      long off=0;
      for(i=0;i<3;i++){
        op.packet=data+off;
        op.bytes=sizes[i];
        op.b_o_s=(i==0);
        op.e_o_s=(i==2);
        op.granulepos=gps[i];
        off+=sizes[i];
        CHECK_LONG("packetin",ogg_stream_packetin(&os_en,&op),0);

        while(ogg_stream_flush(&os_en,&og)){
          int continued=ogg_page_continued(&og);
          int completed=ogg_page_packets(&og);
          ogg_int64_t page_gp=ogg_page_granulepos(&og);
          int last_val=og.header[27+og.header[26]-1];

          /* A page is continued iff the previous emitted page ended
             with a lacing value of 255 (an unterminated packet). */
          CHECK_LONG("continued flag matches previous page",continued,
                     pageno==0?0:prev_incomplete);
          prev_incomplete=(last_val==255);

          /* A page that completes no packet must advertise -1; a page
             that completes one must carry a real position. */
          if(completed==0)
            CHECK_LONG("no completion implies granulepos -1",page_gp,-1);
          else
            CHECK_LONG("completion implies granulepos",page_gp>=0,1);

          CHECK_LONG("push page",push(&oy,og.header,og.header_len),0);
          CHECK_LONG("push body",push(&oy,og.body,og.body_len),0);
          SYNC_PAGE("sync round-trip page",&oy,&og,1);
          PAGEIN("pagein round-trip page",&os_de,&og);

          while(ogg_stream_packetpeek(&os_de,NULL)>0){
            int got=ogg_stream_packetout(&os_de,&op);
            CHECK_LONG("packetout round-trip",got,1);
            if(got!=1)break;
            CHECK_LONG("packet bytes match",op.bytes,sizes[next_packet]);
            CHECK_LONG("packet b_o_s",op.b_o_s?1:0,next_packet==0);
            CHECK_LONG("packet e_o_s",op.e_o_s?1:0,next_packet==2);
            /* Only the last packet completed on a page carries the
               page granulepos; earlier packets completed on the same
               page carry -1. The BOS page always advertises 0, so the
               first packet may legitimately carry 0. Any other present
               value must be the one fed to the encoder for this packet
               -- never another packet's value. */
            CHECK_LONG("packet granulepos is -1, 0 (bos) or its own value",
                       op.granulepos==-1 || op.granulepos==gps[next_packet] ||
                       (next_packet==0 && op.granulepos==0),1);
            if(next_packet==2)
              CHECK_LONG("eos packet owns final granulepos",
                         op.granulepos,gps[2]);
            for(j=0;j<op.bytes;j++){
              if(op.packet[j]!=(unsigned char)(0x10+((dofs+j)%0x20))){
                CHECK_LONG("packet body mismatch at offset",j,-1);
                break;
              }
            }
            dofs+=op.bytes;
            next_packet++;
          }
          pageno++;
        }
      }
    }

    CHECK_LONG("all packet bytes accounted for",dofs,total);
    CHECK_LONG("all packets decoded",next_packet,3);
    CHECK_LONG("multiple pages were used",pageno>3,1);
    CHECK_LONG("stream reports eos",ogg_stream_eos(&os_de),1);

    ogg_sync_clear(&oy);
    ogg_stream_clear(&os_de);
    ogg_stream_clear(&os_en);
    free(data);
  }
  test_end();
}

int main(void){
  test_continued_shares_page_with_complete_packets();
  test_stream_ends_at_incomplete_packet();
  test_missing_continuation_page();
#ifndef DISABLE_CRC
  test_corrupt_page_is_skipped();
  test_corrupt_continuation_yields_no_packet();
#else
  fprintf(stderr,"test: corrupt-page cases skipped (--disable-crc)\n");
#endif
  test_continued_without_pending_packet();
  test_zero_termination_across_pages();
  test_encoder_decoder_span_roundtrip();

  fprintf(stderr,"%d checks, %d failure%s\n",checks_run,checks_failed,
          checks_failed==1?"":"s");
  return(checks_failed?1:0);
}
