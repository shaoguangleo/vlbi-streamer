/*
 * ringbuf.c -- ringbuffer implementation for vlbi-streamer
 *
 * Written by Tomi Salminen (tlsalmin@gmail.com)
 * Copyright 2012 Metsähovi Radio Observatory, Aalto University.
 * All rights reserved
 * This file is part of vlbi-streamer.
 *
 * vlbi-streamer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * vlbi-streamer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with vlbi-streamer.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */
#include <stdio.h>
#include <malloc.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include "config.h"
#ifdef HAVE_HUGEPAGES
#include <sys/mman.h>
#endif

#include "ringbuf.h"
#include "common_wrt.h"
#define DO_WRITES_IN_FIXED_BLOCKS

extern FILE* logfile;

/* Check if we really have HUGEPAGE-support 	*/
/* moved to configure-script */
/*
#ifndef MAP_HUGETLB
#undef HAVE_HUGEPAGES
#endif
*/

int rbuf_init(struct opt_s* opt, struct buffer_entity * be){
  //Moved buffer init to writer(Choosable by netreader-thread)
  int err;
  struct ringbuf * rbuf = (struct ringbuf*) malloc(sizeof(struct ringbuf));
  if(rbuf == NULL)
    return -1;
  be->opt = rbuf;
  rbuf->opt = opt;

  //be->membranch = opt->membranch;
  //be->diskbranch = opt->diskbranch;
  D("Adding ringbuf to membranch");
  struct listed_entity *le = (struct listed_entity*)malloc(sizeof(struct listed_entity));
  le->entity = (void*)be;
  le->child = NULL;
  le->father = NULL;
  be->self = le;
  add_to_entlist(rbuf->opt->membranch, be->self);
  D("Ringbuf added to membranch");

  
  /* Main arg for bidirectionality of the functions 		*/
  //rbuf->read = opt->read;

  //rbuf->opt->optbits = opt->optbits;
  rbuf->async_writes_submitted = 0;
#ifdef WRITE_WHOLE_BUFFER
  rbuf->ready_to_w = 0;
#endif

  //rbuf->opt->buf_num_elems = opt->buf_num_elems;
  //rbuf->opt->packet_size = opt->packet_size;
  //rbuf->writer_head = 0;
  //rbuf->tail = rbuf->hdwriter_head = (rbuf->opt->buf_num_elems-1);
  if(rbuf->opt->optbits & READMODE){
    rbuf->hdwriter_head = rbuf->writer_head = 0;
    rbuf->tail = -1;
  /* If we're reading, we want to fill in the buffer 		*/
    //rbuf->tail = -1;
    //if(rbuf->opt->optbits & ASYNC_WRITE)
      //rbuf->hdwriter_head =0;
  }
  else{
    rbuf->tail = rbuf->hdwriter_head = rbuf->writer_head = 0;
  }
#ifdef USE_DIFF
  rbuf->diff =0 ;
#endif
  rbuf->running = 0;
  //rbuf->opt->do_w_stuff_every = opt->do_w_stuff_every;

  be->headlock = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
  be->iosignal = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
  pthread_mutex_init(be->headlock, NULL);
  pthread_cond_init(be->iosignal, NULL);



  /* TODO: Make choosable or just get rid of async totally 	*/
  //rbuf->async = opt->async;

#ifdef HAVE_HUGEPAGES
  if(rbuf->opt->optbits & USE_HUGEPAGE){
    rbuf->buffer = mmap(NULL, ((unsigned long)rbuf->opt->buf_num_elems)*((unsigned long)rbuf->opt->packet_size), PROT_READ|PROT_WRITE , MAP_ANONYMOUS|MAP_SHARED|MAP_HUGETLB, 0,0);
    if(rbuf->buffer ==MAP_FAILED){
      perror("MMAP");
      fprintf(stderr, "RINGBUF: Couldn't allocate hugepages\n");
      //remove(hugefs);
      err = -1;
    }
    else{
      err = 0;
      D("RINGBUF: mmapped to hugepages");
    }
  }
  else
#endif /* HAVE_HUGEPAGES */
  {
    D("RINGBUF: Memaligning buffer");
    err = posix_memalign((void**)&(rbuf->buffer), sysconf(_SC_PAGESIZE), ((unsigned long)rbuf->opt->buf_num_elems)*((unsigned long)rbuf->opt->packet_size));
  }
  if (err < 0 || rbuf->buffer == 0) {
    perror("make_write_buffer");
    return -1;
  }
  D("RINGBUF: Memory allocated");

  return 0;
}
int  rbuf_close(struct buffer_entity* be, void *stats){
//TODO: error handling
  struct ringbuf * rbuf = (struct ringbuf *)be->opt;
  pthread_mutex_destroy(be->headlock);
  free(be->headlock);
  free(be->iosignal);

#ifdef HAVE_HUGEPAGES
  if(rbuf->opt->optbits & USE_HUGEPAGE){
    munmap(rbuf->buffer, rbuf->opt->packet_size*rbuf->opt->buf_num_elems);
  }
  else
#endif /* HAVE_HUGEPAGES */
    free(rbuf->buffer);
  free(rbuf);
  //free(be->recer);
  D("RINGBUF: Buffer closed");
  return 0;
}

//Calling this directly requires, that you know you won't go past a restrictor
void increment_amount(struct ringbuf * rbuf, int* head, int amount)
{
  int split = rbuf->opt->buf_num_elems - *head;
  //Gone over
  if(split <= amount){
    *head = (amount-split);
  }
  else{
    *head += amount;
  }
}
int diff_max(int a , int b, int max){
  if(a>b)
    return (max+1)-a+b;
  else
    return b-a;
}
/* Changing the packet-head (writed_head on record and tail on send) 	*/
/* are the only callers for this so increment diff			*/
int increment(struct ringbuf * rbuf, int *head, int *restrainer){
  //if(*head == (*restrainer-1)){
  if(diff_max(*head, *restrainer, rbuf->opt->buf_num_elems) == 1){
    D("Can't give buffer as head at %d restained by %d", *head,*restrainer);
    return 0;
  }
  else{
    increment_amount(rbuf, head, 1);
    return 1;
  }
}
/* NOTE: Needs to be called inside critical section. Breaks modularity 	*/
/* a little, but else we could write a buffer that hasn't been filled 	*/
/* with data yet							*/
void * rbuf_get_buf_to_write(struct buffer_entity *be){
  struct ringbuf * rbuf = (struct ringbuf*) be->opt;
  void *spot;
  int *head, *rest;
    /*
     * In reading situation we try to fill the buffer from HD-values as fast as possible.
     * when asked for a buffer to send, we give the tail buffer and so the tail chases the head
     * where hdwriter_head tells how many spots we've gotten into the memory
     */
  if(!(rbuf->opt->optbits & ASYNC_WRITE)){
    if(rbuf->opt->optbits & READMODE){
      head = &(rbuf->tail);
      rest = &(rbuf->writer_head);
    }
    else{
      head = &(rbuf->writer_head);
      rest = &(rbuf->tail);
    }
  }
  else{
    if(rbuf->opt->optbits & READMODE){
      head = &(rbuf->tail);
      rest = &(rbuf->hdwriter_head);
    }
    /*
     * In writing situation we simple fill the buffer with packets as fast as we can. Here
     * the head chases the tail
     */
    else{
      head = &(rbuf->writer_head);
      rest = &(rbuf->tail);
    }
  }

  if(!increment(rbuf, head, rest)){
    spot = NULL;
    D("RINGBUF_H: BUF FULL");
  }
  else
    spot = rbuf->buffer + (((long unsigned)*head)*(long unsigned)rbuf->opt->packet_size);
  return spot;
}
/*
void rbuf_set_ready(struct buffer_entity *be){
  ((struct ringbuf*)be->opt)->ready = 1;
}
*/

/* The final function that really writes. tail is a pointer 	*/
/* since it isn't subject to critical section problems and	*/
/* It's the part we want to update				*/
int write_bytes(struct buffer_entity * be, int head, int *tail, int diff){
  struct ringbuf * rbuf = (struct ringbuf * )be->opt;
  /* diff should be precalculated  since it contains critical section stuff */
  int i;
  long ret;
  //int requests = 1+((rbuf->writer_head < rbuf->hdwriter_head) && rbuf->writer_head > 0);
  int requests = 1+((head < *tail) && head > 0);
  for(i=0;i<requests;i++){
    void * start;
    long count;
    long endi;
    if(i == 0){
      //start = rbuf->buffer + (rbuf->hdwriter_head * rbuf->opt->packet_size);
      start = rbuf->buffer + (*tail * rbuf->opt->packet_size);
      if(requests ==2){
	//endi = rbuf->opt->buf_num_elems - rbuf->hdwriter_head;
	endi = rbuf->opt->buf_num_elems - *tail;
	diff -= endi;
      }
      else
	endi = diff;
    }
    else{
      start = rbuf->buffer;
      endi = diff;
    }
    count = (endi) * ((long)(rbuf->opt->packet_size));

    D("RINGBUF: Blocking writes. Write from %i to %lu diff %lu elems %i, "
      "%lu bytes", *tail, *tail+endi, endi, rbuf->opt->buf_num_elems, count);
    ret = be->recer->write(be->recer, start, count);
    if(ret<0){
      fprintf(stderr, "RINGBUF: Error in Rec entity write: %ld\n", ret);
      return -1;
    }
    else if (ret == 0){
      //Don't increment
    }
    else{
      if(rbuf->opt->optbits & ASYNC_WRITE)
	rbuf->async_writes_submitted++;
      if(ret != count){
	fprintf(stderr, "RINGBUF_H: Write wrote %ld out of %lu\n", ret, count);
	/* TODO: Handle incrementing so we won't lose data */
      }
      //increment_amount(rbuf, &(rbuf->hdwriter_head), endi);
      //pthread_mutex_lock(be->headlock);
      increment_amount(rbuf, tail, endi);
#ifdef USE_DIFF
      rbuf->diff-=endi;
#endif
      //pthread_mutex_unlock(be->headlock);
    }
  }
  return 0;
}
void rbuf_init_head_n_tail(struct ringbuf *rbuf, int** head, int** tail){
  if(!(rbuf->opt->optbits & ASYNC_WRITE)){
    if(rbuf->opt->optbits & READMODE){
      *tail = &(rbuf->writer_head);
      *head = &(rbuf->tail);
    }
    else{
      *tail = &(rbuf->tail);
      *head = &(rbuf->writer_head);
    }
  }
  else{
    //TODO: Something fishy here. Think this through
    if(rbuf->opt->optbits & READMODE){
      *tail = &(rbuf->writer_head);
      *head= &(rbuf->tail);
    }
    else{
      *tail = &(rbuf->hdwriter_head);
      *head = &(rbuf->writer_head);
    }
  }
}
//rbuf->last_io_i = diff_final;
/* main func for reading and sleeping on full buffer */
void *rbuf_read_loop(void *buffo){
  struct buffer_entity * be = (struct buffer_entity *)buffo;
  struct ringbuf * rbuf = (struct ringbuf *)be->opt;
  int diff,ret=0;
  int *head, *tail;
  pthread_exit(NULL);
}
/* Need to do 512 byte aligned write at end */
int end_transaction(struct buffer_entity * be, int head, int *tail, int diff){
  struct ringbuf * rbuf = (struct ringbuf * )be->opt;
  unsigned long wrote_extra = 0;
  long int ret = 0;
  
  unsigned long count = diff*(rbuf->opt->packet_size);
  void * start = rbuf->buffer + (*tail * rbuf->opt->packet_size);
  while(count % 512 != 0){
    count++;
    wrote_extra++;
  }
  D("RINGBUF: Have to write %lu extra bytes", wrote_extra);

  ret = be->recer->write(be->recer, start, count);
  if(ret<0){
    fprintf(stderr, "RINGBUF: Error in Rec entity write: %ld\n", ret);
    return -1;
  }
  else if (ret == 0){
    //Don't increment
  }
  else{
    if(rbuf->opt->optbits & ASYNC_WRITE)
      rbuf->async_writes_submitted++;
    if((unsigned long)ret != count){
      fprintf(stderr, "RINGBUF_H: Write wrote %ld out of %lu\n", ret, count);
      /* TODO: Handle incrementing so we won't lose data */
    }
    //increment_amount(rbuf, &(rbuf->hdwriter_head), endi);
    pthread_mutex_lock(be->headlock);
    increment_amount(rbuf, tail, diff);
#ifdef USE_DIFF
    rbuf->diff-= diff;
#endif
    pthread_mutex_unlock(be->headlock);
  }
  return 0;
}
/* main func for writing and sleeping on buffer empty */ 
void *rbuf_write_loop(void *buffo){
  struct buffer_entity * be = (struct buffer_entity *)buffo;
  struct ringbuf * rbuf = (struct ringbuf *)be->opt;
  int ret=0,i=0;
  unsigned int diff;
  //int i;
  int *head, *tail;
#ifdef DO_WRITES_IN_FIXED_BLOCKS
  int limited_head;
#endif
  int dow_div_packet = rbuf->opt->do_w_stuff_every/rbuf->opt->packet_size;
  rbuf_init_head_n_tail(rbuf,&head,&tail);
  rbuf->running = 1;
  while(rbuf->running){
    //Check if we've written a whole buf and need to change our backend and set ourselves free
    if(i == rbuf->opt->buf_num_elems){
      D("RINGBUF: Write cycle complete. Setting self to free");
      i=0;
      set_free(rbuf->opt->membranch, be->self);
      set_free(rbuf->opt->diskbranch, be->recer->self);
      be->recer = NULL;
    }
    pthread_mutex_lock(be->headlock);
    while(((diff = diff_max(*tail, *head, rbuf->opt->buf_num_elems)) < dow_div_packet) && rbuf->running)
    {
      D("Not enough to write: %d left %d running", diff, rbuf->running);
      pthread_cond_wait(be->iosignal, be->headlock);
    }
    D("RINGBUF: Writing: diffmax reported: we have %d left. tail: %d, head: %d",
      diff, *tail, *head);
    pthread_mutex_unlock(be->headlock);
    /* We're not yet attached to a backend write-end. Go get one */
    if(be->recer == NULL){
      /* Not fixed */
      be->recer = (struct recording_entity*)get_free(rbuf->opt->diskbranch,rbuf->opt,0,0);
    }

    /* We might be stopped inbetween by the streamer entity */
    if(!rbuf->running)
      break;
#ifdef DO_WRITES_IN_FIXED_BLOCKS
    diff = diff < dow_div_packet ? diff : dow_div_packet;
    limited_head = (*tail+diff)%rbuf->opt->buf_num_elems;
#else
    limited_head = *head;
#endif
    if(diff > 0){
      ret = write_bytes(be,limited_head, tail,diff);
      if(ret!=0){
	fprintf(stderr, "RINGBUG: Write returned error %d. Stopping\n", ret);
	//Close the recer if there's a problem
	be->recer->close(be->recer,NULL);
      }
      /* We get a zero if its not a straight error, but not a full write 	*/
      /* Or an io_submit that wasn't queued					*/
      /*
	 else if (ret == 0){
	 fprintf(stderr, "RINGBUF: Incomplete or failed async write. Not stopping\n");
	 }
	 */
      else{
	/* If we either just wrote the stuff in write_after_checks(synchronious 	*/
	/* blocking write or check in async mode) returned a > 0 amount of writes  */
	/* done */
	/* Update: Only signal if the streamer is blocked */
#ifdef CHECK_FOR_BLOCK_BEFORE_SIGNAL
	if(!(rbuf->opt->optbits & ASYNC_WRITE) && be->se->is_blocked(be->se) == 1)
#else
	  if(!(rbuf->opt->optbits & ASYNC_WRITE))
#endif
	  {
	    i+= diff;
	  }
	  else
	    rbuf_check(be);
	/* Decrement the packet number we wan't to read */
      }
    }
  }
  D("RINGBUF: Closing rbuf thread");
  // Main thread stopped so just write
  /* TODO: On asynch io, this will not check the last writes */
  if(ret >=0){
    /* TODO: Move away from silly async-wait times and 	*/
    /* Just use a counter for number of writes 		*/
    while((diff = diff_max(*tail, *head, rbuf->opt->buf_num_elems)) > 0){
      int ret = 0;
      D("RINGBUF: Closing up: diffmax reported: we have %d left in buffer "
        "after completion", diff);
#ifdef DO_WRITES_IN_FIXED_BLOCKS
      //aio can overload on write requests, so making just one big for it
      //if(!(rbuf->opt->optbits & ASYNC_WRITE)){
      /* NOTE: Due to arbitrary size packets, we need to write extra data 	*/
      /* on the disk. IO_DIRECT requires block bytes aligned writes(512) 	*/
      /* So we increment it to the next full bytecount				*/
      if(diff < dow_div_packet){
	ret = end_transaction(be, *head,tail,diff);
      }
      else{
	if (diff > dow_div_packet)
	  diff = dow_div_packet;
	limited_head = (*tail+diff)%rbuf->opt->buf_num_elems;
	//}
	//else
	//limited_head = *head;
#else
	limited_head = *head;
#endif
	ret = write_bytes(be,limited_head,tail,diff);
	if(ret != 0){
	  fprintf(stderr, "RINGBUF: Error in write. Exiting\n");
	  break;
	}
	/* TODO: add a counter for n. of writes so we'll be sure we've written everything in async */
	if(rbuf->opt->optbits & ASYNC_WRITE){
	  usleep(100);
	  rbuf_check(be);
	}
    }
    }
    if(rbuf->opt->optbits & ASYNC_WRITE){
      while(rbuf->async_writes_submitted >0){
	D("RINGBUF: Sleeping and waiting for async to complete %d writes "
          "submitted", rbuf->async_writes_submitted);
	usleep(1000);
	rbuf_check(be);
      }
    }
    //TODO: Make the previous loop "eternal" as in vlbistreamer should be only invoked once
    D("Final write cycle complete. Freeing both");
    //set_free(rbuf->opt->membranch, be->self);
    if(be->recer != NULL){
      set_free(rbuf->opt->diskbranch, be->recer->self);
      be->recer = NULL;
    }
  }
  pthread_exit(NULL);
}
int rbuf_check(struct buffer_entity *be){
  int ret = 0, returnable = 0;
  struct ringbuf * rbuf = (struct ringbuf * )be->opt;
  while ((ret = be->recer->check(be->recer,0))>0){
    /* Write done so decrement async_writes_submitted */
    if(rbuf->opt->optbits & ASYNC_WRITE)
      rbuf->async_writes_submitted--;
    D("RINGBUF: %lu Writes complete.", ret/rbuf->opt->packet_size);
    returnable = ret;
    int * to_increment;
    unsigned long num_written;
    //TODO: Augment for bidirectionality
    if(rbuf->opt->optbits & READMODE)
      to_increment = &(rbuf->hdwriter_head);
    else
      to_increment = &(rbuf->tail);

    /* ret tells us how many bytes were written */
    /* ret/buf_size = number of buffs we've written */
    if(ret > rbuf->opt->do_w_stuff_every)
      ret = rbuf->opt->do_w_stuff_every;

    num_written = ret/rbuf->opt->packet_size;

    /* This might happen on last write, when we need to write some extra */
    /* stuff to keep the writes Block aligned				*/

    /* TODO: If we receive IO done out of order, we'll potentially release  */
    /* Buffer entries that haven't yet been released. Trusting that this 	*/
    /* won't happen 							*/

    pthread_mutex_lock(be->headlock);
    increment_amount(rbuf, to_increment, num_written);
    pthread_cond_signal(be->iosignal);
    pthread_mutex_unlock(be->headlock);
    //rbuf->last_io_i = 0;
    //Only used cause IO_WAIT doesn't work with EXT4 yet 
    //ret = WRITE_COMPLETE_DONT_SLEEP;
  }
  return returnable;
}
/* Not used anymore! Use dummy loop */
int dummy_write(struct ringbuf *rbuf){
  int writable = diff_max(rbuf->hdwriter_head, rbuf->writer_head, rbuf->opt->buf_num_elems);
  increment_amount(rbuf, &(rbuf->hdwriter_head), writable);
  //Dummy write completes right away
  dummy_return_from_write(rbuf);
  return 1;
}
//alias for completiong from asynchronous write
inline void dummy_return_from_write(struct ringbuf *rbuf){
  int written = diff_max(rbuf->tail, rbuf->hdwriter_head, rbuf->opt->buf_num_elems);
  increment_amount(rbuf, &(rbuf->tail), written);
}
int dummy_write_wrapped(struct buffer_entity *be, int force){
  struct ringbuf * rbuf = (struct ringbuf*)be->opt;
  dummy_write(rbuf);
  return 1;
}
void rbuf_stop_running(struct buffer_entity *be){
  ((struct ringbuf *)be->opt)->running = 0 ;
}
void rbuf_stop_and_signal(struct buffer_entity *be){
  rbuf_stop_running(be);
  pthread_mutex_lock(be->headlock);
  pthread_cond_signal(be->iosignal);
  pthread_mutex_unlock(be->headlock);
}
int rbuf_wait(struct buffer_entity * be){
  return be->recer->wait(be->recer);
}
#ifdef CHECK_FOR_BLOCK_BEFORE_SIGNAL
int rbuf_is_blocked(struct buffer_entity *be){
  return((struct ringbuf*)be->opt)->is_blocked;
}
#endif
void rbuf_init_mutex_n_signal(struct buffer_entity *be, void * mutex, void * signal){
  struct ringbuf *rbuf = be->opt;
  be->headlock = (pthread_mutex_t *)mutex;
  be->iosignal = (pthread_cond_t *) signal;
  //return 1;
}
/* We only need to cancel it while writing, so use writer_head */
void rbuf_cancel_writebuf(struct buffer_entity *be){
  struct ringbuf *rbuf = be->opt;
  rbuf->writer_head = (rbuf->writer_head -1) % rbuf->opt->buf_num_elems;
}
int rbuf_init_buf_entity(struct opt_s * opt, struct buffer_entity *be){
  be->init = rbuf_init;
  //be->write = rbuf_aio_write;
  be->get_writebuf = rbuf_get_buf_to_write;
  be->wait = rbuf_wait;
  be->close = rbuf_close;
  if(opt->optbits & READMODE)
    be->write_loop = rbuf_read_loop;
  else
    be->write_loop = rbuf_write_loop;
  //be->simple_get_writebuf = simple_get_buf;
  be->stop = rbuf_stop_running;
  be->cancel_writebuf = rbuf_cancel_writebuf;
#ifdef CHECK_FOR_BLOCK_BEFORE_SIGNAL
  be->is_blocked = rbuf_is_blocked;
#endif
  be->init_mutex = rbuf_init_mutex_n_signal;

  return be->init(opt,be); 
}
