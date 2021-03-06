#ifndef COMMON_FILEHANDLING_H
#define COMMON_FILEHANDLING_H
#include "streamer.h"
#include "active_file_index.h"
#include "datatypes.h"

#define ALL_DONE 3123
#define DONTRYLOADNOMORE 	B(2) 

struct sender_tracking{
  int allocated_to_load;
  unsigned long files_sent;
  unsigned long files_skipped;
  unsigned long files_loaded;
  unsigned long packets_loaded;
  unsigned long packets_sent;
  unsigned long files_in_loading;       ///!< How many files are ready loaded.
  unsigned long n_packets_probed;
  unsigned long n_files_probed;
  unsigned long total_bytes_to_send;
  int 		status_probed;
  unsigned long packetcounter;
  unsigned long inc;
  //unsigned long packetpeek;
  TIMERTYPE now;
#if(SEND_DEBUG)
  TIMERTYPE reference;
#endif
#if!(PREEMPTKERNEL)
  TIMERTYPE onenano;
  unsigned long minsleep;
#endif
  TIMERTYPE req;
};

void init_sender_tracking(struct opt_s *opt, struct sender_tracking *st);
int start_loading(struct opt_s * opt, struct buffer_entity *be, struct sender_tracking *st);
int should_i_be_running(struct opt_s *opt, struct sender_tracking *st);
void throttling_count(struct opt_s* opt, struct sender_tracking * st);
int jump_to_next_file(struct opt_s *opt, struct streamer_entity *se, struct sender_tracking *st);
void init_resq(struct resq_info* resq);
#endif
