#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <libaio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/stat.h>

#include "aiowriter.h"
//#include "aioringbuf.h"
#include "streamer.h"

#define MAX_EVENTS 128
#ifdef IOVEC
#define IOVEC_MAX 2
#define MAX_IOCB 2
#else
#define MAX_IOCB MAX_EVENTS
#endif
//Nanoseconds for waiting on busy io
#define TIMEOUT_T 100
struct io_info{
  io_context_t * ctx;
  char *filename;
  int fd;
  long long offset;
  //Used as bytes read in readmode
  long long bytes_exchanged;
  //Used for sending
  INDEX_FILE_TYPE elem_size;
  int f_flags;
  //Used if we're in readmode
  INDEX_FILE_TYPE *indices;
  int read;
  unsigned long indexfile_count;
};
int aiow_open_file(int *fd, int flags, char * filename, loff_t fallosize){
  struct stat statinfo;
  int err =0;

  //ioi->latest_write_num = 0;
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Initializing write point\n");
#endif
  //Check if file exists
  //ioi->f_flags = O_WRONLY|O_DIRECT|O_NOATIME|O_NONBLOCK;
  //ioi->filename = opt->filenames[opt->taken_rpoints++];
  err = stat(filename, &statinfo);
  if (err < 0) {
    if (errno == ENOENT){
      //We're reading the file
      if(flags & O_RDONLY){
	perror("AIOW: File not found, eventhought we're in send-mode");
	return -1;
      }
      else{
#ifdef DEBUG_OUTPUT
	fprintf(stdout, "File doesn't exist. Creating it\n");
#endif
	flags |= O_CREAT;
	err = 0;
      }
    }
    else{
      fprintf(stderr,"Error: %s on %s\n",strerror(errno), filename);
      return -1;
    }
  }

  //This will overwrite existing file.TODO: Check what is the desired default behaviour 
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "Opening file %s\n", filename);
#endif
  *fd = open(filename, flags, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
  if(*fd == -1){
    fprintf(stderr,"Error: %s on %s\n",strerror(errno), filename);
    return -1;
  }
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: File opened\n");
#endif
  if(fallosize > 0){
    err = fallocate(*fd, 0,0, fallosize);
    if(err == -1){
      fprintf(stderr, "Fallocate failed on %s", filename);
      return err;
    }
#ifdef DEBUG_OUTPUT
    fprintf(stdout, "AIOW: File preallocated\n");
#endif
  }
  return err;
}

//TODO: Error handling

/* Fatal error handler */
static void io_error(const char *func, int rc)
{
  fprintf(stderr, "%s: error %d", func, rc);
}

static void wr_done(io_context_t ctx, struct iocb *iocb, long res, long res2){
  fprintf(stdout, "This will never make it to print\n");
  if(res2 != 0)
    io_error("aio write", res2);
  if(res != iocb->u.c.nbytes){
    fprintf(stderr, "write missed bytes expect %lu got %li", iocb->u.c.nbytes, res2);
  }
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "Write callback done. Wrote %li bytes\n", res);
#endif
  free(iocb);
}
int aiow_handle_indices(struct io_info *ioi){
  char * filename = (char*)malloc(sizeof(char)*FILENAME_MAX);
  int f_flags = O_RDONLY;//|O_DIRECT|O_NOATIME|O_NONBLOCK;
  int fd,err;
  int num_elems;
  //Duplicate stat here, since first done in aiow_read, but meh.
  struct stat statinfo;


  sprintf(filename, "%s%s", ioi->filename, ".index");

  aiow_open_file(&fd, f_flags, filename, 0);

  /*
   * elem_size = Size of the packets we received when receiving the stream
   * INDEX_FILE_TYPE = The size of our index-counter.(Eg 32bit integer or 64bit).
   */
  //Read the elem size from the first index
  err = read(fd, (void*)&(ioi->elem_size), sizeof(INDEX_FILE_TYPE));
  if(err<0){
    perror("AIOW: Index file size read");
    return err;
  }
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "Elem size here %d\n", ioi->elem_size);
#endif 

  //ioi->elem_size = err;

  err = fstat(fd, &statinfo);
  if(err<0){
    perror("FD stat from index");
    return err;
  }
  //NOTE: Reducing first element, which simply tells size of elements
  num_elems = (statinfo.st_size-sizeof(INDEX_FILE_TYPE))/sizeof(INDEX_FILE_TYPE);
  ioi->indices = (INDEX_FILE_TYPE*) malloc(sizeof(INDEX_FILE_TYPE)*(num_elems));

  ioi->indexfile_count = 0;
  INDEX_FILE_TYPE* pointer = ioi->indices;
  while((err = read(fd, (void*)&(pointer), sizeof(INDEX_FILE_TYPE)))>0){
    pointer += sizeof(INDEX_FILE_TYPE);
    ioi->indexfile_count  += 1;
  }
  
  close(fd);
  free(filename);

  return err;
}
//Read init is so similar to write, that i'll just add a parameter
int aiow_init(struct opt_s* opt, struct recording_entity *re){
  int ret;
  //void * errpoint;
  re->opt = (void*)malloc(sizeof(struct io_info));
  struct io_info * ioi = (struct io_info *) re->opt;
  loff_t prealloc_bytes;
  //struct stat statinfo;
  int err =0;
  ioi->read = opt->read;

  //ioi->latest_write_num = 0;
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Initializing write point\n");
#endif
  //Check if file exists
  if(ioi->read == 1){
    ioi->f_flags = O_RDONLY|O_DIRECT|O_NOATIME|O_NONBLOCK;
    prealloc_bytes = 0;
  }
  else{
    ioi->f_flags = O_WRONLY|O_DIRECT|O_NOATIME|O_NONBLOCK;
    //RATE = 10 Gb => RATE = 10*1024*1024*1024/8 bytes/s. Handled on n_threads
    //for s seconds.
    loff_t prealloc_bytes = (RATE*opt->time*1024)/(opt->n_threads*8);
    //Split kb/gb stuff to avoid overflow warning
    prealloc_bytes = prealloc_bytes*1024*1024;
    //set flag FALLOC_FL_KEEP_SIZE to precheck drive for errors
  }
  ioi->filename = opt->filenames[opt->taken_rpoints++];
  err = aiow_open_file(&(ioi->fd),ioi->f_flags, ioi->filename, prealloc_bytes);
  if(err<0)
    return -1;
  //TODO: Set offset accordingly if file already exists. Not sure if
  //needed, since data consistency would take a hit anyway
  ioi->offset = 0;
  ioi->bytes_exchanged = 0;
  if(ioi->read){
    err = aiow_handle_indices(ioi);
    if(err<0){
      perror("AIOW: Reading indices");
      return -1;
    }
    else{
      opt->buf_elem_size = ioi->elem_size;
#ifdef DEBUG_OUTPUT
      fprintf(stdout, "Element size is %d\n", opt->buf_elem_size);
#endif
    }
  }
  else{
    ioi->elem_size = opt->buf_elem_size;
  }

#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Preparing iostructs\n");
#endif
  ioi->ctx =(io_context_t *) malloc(sizeof(io_context_t));
  void * errpoint = memset((void*)ioi->ctx, 0, sizeof(*(ioi->ctx)));
  if(errpoint== NULL){
    perror("AIOW: Memset ctx");
    return -1;
  }
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Queue init\n");
#endif
  ret = io_queue_init(MAX_EVENTS, ioi->ctx);
  if(ret < 0){
    perror("AIOW: IO_QUEUE_INIT");
    return -1;
  }
  return ret;
}

int aiow_write(struct recording_entity * re, void * start, size_t count){
  int ret;
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Performing read/write\n");
#endif

  struct io_info * ioi = (struct io_info * )re->opt;

  struct iocb *ib[1];
  ib[0] = (struct iocb*) malloc(sizeof(struct iocb));
  if(ioi->read == 1)
    io_prep_pread(ib[0], ioi->fd, start, count, ioi->offset);
  else
    io_prep_pwrite(ib[0], ioi->fd, start, count, ioi->offset);

  //io_set_callback(ib[0], wr_done);

#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Prepared read/write for %lu bytes\n", count);
#endif

  //Not sure if 3rd argument safe, but running 
  //one iocb at a time anyway
  ret = io_submit(*(ioi->ctx), 1, ib);

#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Submitted %d reads/writes\n", ret);
#endif
  if(ret <0){
    perror("AIOW: io_submit");
    return -1;
  }
  ioi->offset += count;
  //ioi->bytes_exchanged += count;
  return ret;
}
int aiow_check(struct recording_entity * re){
  //Just poll, so we can keep receiving more packets
  struct io_info * ioi = (struct io_info *)re->opt;
  static struct timespec timeout = { 0, 0 };
  struct io_event event;
  int ret = io_getevents(*(ioi->ctx), 0, 1, &event, &timeout);
  //
  if(ret > 0){
    if((signed long )event.res > 0){
    ioi->bytes_exchanged += event.res;
#ifdef DEBUG_OUTPUT
    fprintf(stdout, "AIOW: Check return %d, read/written %lu bytes\n", ret, event.res);
#endif
    }
    else{
      fprintf(stderr, "AIOW: Write check return error %ld\n", event.res);
      perror("AIOW: Check");
      return -1;
    }
  }

  /*
   * TODO: Change implementation for reads also
   * Might need a unified writer backend to
   * queue reads properly, thought reads
   * go to different file
   */

  return ret;
}
//Not used, since can't update status etc.
//Using queue-stuff instead
//TODO: Make proper sleep. io_queue_wait doesn't work
int aiow_wait_for_write(struct recording_entity* re){
  //struct rec_point * rp = (struct rec_point *) recpoint;
  struct io_info * ioi = (struct io_info *)re->opt;
  //Needs to be static so ..durr
  //static struct timespec timeout = { 1, TIMEOUT_T };
  //Not sure if this works, since io_queue_run doesn't
  //work (have to use io_getevents), or then I just
  //don't know how to use it
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Buffer full %s. Going to sleep\n", ioi->filename);
#endif
  //Doesn't really sleep :/
  //return io_queue_wait(*(ioi->ctx), &timeout);
  return usleep(5000);
}
int aiow_close(struct recording_entity * re, void * stats){
  int err;
  struct io_info * ioi = (struct io_info*)re->opt;

  struct stats* stat = (struct stats*)stats;
  stat->total_written += ioi->bytes_exchanged;
  /*
     char * indexfile = malloc(sizeof(char)*FILENAME_MAX);
     sprintf(indexfile, "%s%s", ioi->filename, ".index");
     int statfd = open(ioi->filename, ioi->f_flags, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);

     close(statfd);
     */

  //Shrink to size we received if we're writing
  if(ioi->read == 1){
    err = ftruncate(ioi->fd, ioi->bytes_exchanged);
    if(err<0)
      perror("AIOW: ftruncate");
  }
  else{
    free(ioi->indices);
    /* No need to close indice-file since it was read into memory */
  }
  close(ioi->fd);
  io_destroy(*(ioi->ctx));

  ioi->ctx = NULL;
  free(ioi->filename);


  free(ioi);
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Writer closed\n");
#endif
  return 0;
}
int aiow_write_index_data(struct recording_entity *re, void *data, int count){
  struct io_info * ioi = (struct io_info*)re->opt;
  int err = 0;
  char * filename = (char*)malloc(sizeof(char)*FILENAME_MAX);
  int fd;

#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Writing index file\n");
#endif
  sprintf(filename, "%s%s", ioi->filename, ".index");
  int f_flags = O_WRONLY;//|O_DIRECT|O_NOATIME|O_NONBLOCK;

  aiow_open_file(&fd, f_flags, filename, 0);

  //Write the elem size to the first index
  err = write(fd, (void*)&(ioi->elem_size), sizeof(INDEX_FILE_TYPE));
  if(err<0)
    perror("AIOW: Index file size write");
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "Wrote %d as elem size", ioi->elem_size);
#endif

  //Write the data
  err = write(fd, data, count*sizeof(INDEX_FILE_TYPE));
  if(err<0){
    perror("AIOW: Index file write");
    fprintf(stderr, "Filename was %s\n", filename);
  }

  close(fd);
  free(filename);
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "AIOW: Index file written\n");
#endif

  return err;
}
int * aiow_pindex(struct recording_entity *re){
  struct io_info * ioi = re->opt;
  return ioi->indices;
  //return ((struct io_info)re->opt)->indices;
}
unsigned long aiow_nofpacks(struct recording_entity *re){
  struct io_info * ioi = re->opt;
  return ioi->indexfile_count;
  //return ((struct io_info*)re->opt)->indexfile_count;
}
/*
 * Helper function for initializing a recording_entity
 */
int aiow_init_rec_entity(struct opt_s * opt, struct recording_entity * re){
  re->init = aiow_init;
  re->write = aiow_write;
  re->wait = aiow_wait_for_write;
  re->close = aiow_close;
  re->check = aiow_check;
  re->write_index_data = aiow_write_index_data;
  re->get_n_packets = aiow_nofpacks;
  re->get_packet_index = aiow_pindex;

  return re->init(opt,re);
}
int aiow_init_dummy(struct opt_s *op , struct recording_entity *re){
  /*
  re->init = null;
  re-write = dummy_write_wrapped;
  */
  return 1;
}
