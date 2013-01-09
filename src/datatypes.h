#ifndef DATATYPES_H
#define DATATYPES_H
#define BITMASK_30 0xfffffff3
#define BITMASK_24 0xffffff00
#define RBITMASK_30 0x3fffffff
#define RBITMASK_24 0x00ffffff

/* Really VDIF could be 32 bytes, but we won't care about extended user stuff anyway */
#define HSIZE_VDIF 16
#define HSIZE_MARK5B 16
#define HSIZE_UDPMON 8
#define HSIZE_MARK5BNET 20

#define FRAMENUM_FROM_VDIF(x) (long)(*((uint32_t*)((x)+4)) & RBITMASK_24)
#define SET_FRAMENUM_FOR_VDIF(target,framenum) *((uint32_t*)(target+4)) = framenum & RBITMASK_24
#define SECOND_FROM_VDIF(x) (long)(*((uint32_t*)(x))) & RBITMASK_30;
#define SET_SECOND_FOR_VDIF(target,second) *((uint32_t*)(target)) = second & RBITMASK_30

#define SET_FRAMENUM_FOR_UDPMON(target,framenum) *((uint64_t*)(target)) = be64toh((uint64_t)(framenum));
#define SET_FRAMENUM_FOR_MARK5BNET(target,framenum) *((uint32_t*)(target+4)) = (uint32_t)(framenum);
#define SET_FRAMENUM_FOR_MARK5B(target,framenum) *((uint32_t*)(target)) = (uint32_t)(framenum) & get_mask(0,14);
#include "streamer.h"

struct resq_info{
  long  *inc_before, *inc;
  void  *buf, *usebuf, *bufstart, *bufstart_before;
  struct buffer_entity * before;
  long current_seq;
  long seqstart_current;
  int i;
  int packets_per_second;
  /* Special if the packets are spaced for example every */
  /* fifth second.					*/
  int packetsecdif;
  int starting_second;
};
inline long getseq_vdif(void* header, struct resq_info *resq);
inline long getseq_mark5b_net(void* header);
inline long getseq_udpmon(void* header);
int copy_metadata(void* source, void* target, struct opt_s* opt);
int init_header(void** target, struct opt_s* opt);
int check_and_fill(void * buffer, struct opt_s* opt, long fileid, int *expected_errors);
inline long header_match(void* target, void* match, struct opt_s * opt);

#endif