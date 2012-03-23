#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>


#include "streamer.h"
#include "common_wrt.h"


/* These should be moved somewhere general, since they should be used by all anyway */
/* writers anyway */
int common_open_file(int *fd, int flags, char * filename, loff_t fallosize){
  struct stat statinfo;
  int err =0;

  //ioi->latest_write_num = 0;
  //Check if file exists
  //ioi->f_flags = O_WRONLY|O_DIRECT|O_NOATIME|O_NONBLOCK;
  //ioi->filename = opt->filenames[opt->taken_rpoints++];
  err = stat(filename, &statinfo);
  if (err < 0) {
    if (errno == ENOENT){
      //We're reading the file
      if(flags & O_RDONLY){
	perror("COMMON_WRT: File not found, eventhought we're in send-mode");
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
  if(*fd < 0){
    fprintf(stderr,"Error: %s on %s\n",strerror(errno), filename);
    return -1;
  }
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "COMMON_WRT: File opened\n");
#endif
  if(fallosize > 0){
    err = fallocate(*fd, 0,0, fallosize);
    if(err == -1){
      fprintf(stderr, "Fallocate failed on %s\n", filename);
      return err;
    }
#ifdef DEBUG_OUTPUT
    fprintf(stdout, "COMMON_WRT: File preallocated\n");
#endif
  }
  else
#ifdef DEBUG_OUTPUT
    fprintf(stdout, "COMMON_WRT: Not fallocating\n");
#endif
  return err;
}
int common_write_index_data(const char * filename_orig, int elem_size, void *data, int count){
  //struct io_info * ioi = (struct io_info*)re->opt;
  int err = 0;
  char * filename = (char*)malloc(sizeof(char)*FILENAME_MAX);
  int fd;

#ifdef DEBUG_OUTPUT
  fprintf(stdout, "COMMON_WRT: Writing index file\n");
#endif
  sprintf(filename, "%s%s", filename_orig, ".index");
  int f_flags = O_WRONLY;//|O_DIRECT|O_NOATIME|O_NONBLOCK;

  common_open_file(&fd, f_flags, filename, 0);

  //Write the elem size to the first index
  err = write(fd, (void*)&(elem_size), sizeof(INDEX_FILE_TYPE));
  if(err<0)
    perror("COMMON_WRT: Index file size write");
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "Wrote %d as elem size",elem_size);
#endif

  //Write the data
  err = write(fd, data, count*sizeof(INDEX_FILE_TYPE));
  if(err<0){
    perror("COMMON_WRT: Index file write");
    fprintf(stderr, "Filename was %s\n", filename);
  }

  close(fd);
  free(filename);
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "COMMON_WRT: Index file written\n");
#endif

  return err;
}
int common_handle_indices(const char *filename_orig, INDEX_FILE_TYPE * elem_size, void * pindex, INDEX_FILE_TYPE *count){
  char * filename = (char*)malloc(sizeof(char)*FILENAME_MAX);
  int f_flags = O_RDONLY;//|O_DIRECT|O_NOATIME|O_NONBLOCK;
  int fd,err;
  int num_elems;
  //Duplicate stat here, since first done in aiow_read, but meh.
  struct stat statinfo;


  sprintf(filename, "%s%s", filename_orig, ".index");

  common_open_file(&fd, f_flags, filename, 0);

  /*
   * elem_size = Size of the packets we received when receiving the stream
   * INDEX_FILE_TYPE = The size of our index-counter.(Eg 32bit integer or 64bit).
   */
  //Read the elem size from the first index
  err = read(fd, elem_size, sizeof(INDEX_FILE_TYPE));
  if(err<0){
    perror("COMMON_WRT: Index file size read");
    return err;
  }
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "Elem size here %lu\n", *elem_size);
#endif 

  //ioi->elem_size = err;

  err = fstat(fd, &statinfo);
  if(err<0){
    perror("FD stat from index");
    return err;
  }
  //NOTE: Reducing first element, which simply tells size of elements
  num_elems = (statinfo.st_size-sizeof(INDEX_FILE_TYPE))/sizeof(INDEX_FILE_TYPE);
  pindex = (INDEX_FILE_TYPE*) malloc(sizeof(INDEX_FILE_TYPE)*(num_elems));

  *count = 0;
  INDEX_FILE_TYPE* pointer = pindex;
  while((err = read(fd, pointer, sizeof(INDEX_FILE_TYPE)))>0){
    pointer++;
    count += 1;
  }
  
  close(fd);
  free(filename);

  return err;
}
int common_w_init(struct opt_s* opt, struct recording_entity *re){
  //void * errpoint;
  re->opt = (void*)malloc(sizeof(struct common_io_info));
  struct common_io_info * ioi = (struct common_io_info *) re->opt;
  loff_t prealloc_bytes;
  //struct stat statinfo;
  int err =0;
  ioi->read = opt->read;

  //ioi->latest_write_num = 0;
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "COMMON_WRT: Initializing write point\n");
#endif
  //Check if file exists
  if(ioi->read == 1){
    ioi->f_flags = O_RDONLY|O_DIRECT|O_NOATIME;
    prealloc_bytes = 0;
  }
  else{
    //ioi->f_flags = O_WRONLY|O_DIRECT|O_NOATIME|O_NONBLOCK;
    ioi->f_flags = O_WRONLY|O_DIRECT|O_NOATIME;
    //RATE = 10 Gb => RATE = 10*1024*1024*1024/8 bytes/s. Handled on n_threads
    //for s seconds.
    prealloc_bytes = (RATE*opt->time*1024)/(opt->n_threads*8);
    //Split kb/gb stuff to avoid overflow warning
    prealloc_bytes = prealloc_bytes*1024*1024;
    //set flag FALLOC_FL_KEEP_SIZE to precheck drive for errors
  }
  //fprintf(stdout, "wut\n");
  ioi->filename = opt->filenames[opt->taken_rpoints++];
  err = common_open_file(&(ioi->fd),ioi->f_flags, ioi->filename, prealloc_bytes);
  if(err<0){
    fprintf(stderr, "COMMON_WRT: Error in file open: %s\n", ioi->filename);
    return -1;
  }
  //TODO: Set offset accordingly if file already exists. Not sure if
  //needed, since data consistency would take a hit anyway
  ioi->offset = 0;
  ioi->bytes_exchanged = 0;
  if(ioi->read==1){
    err = common_handle_indices(ioi->filename, &(ioi->elem_size), (void*)ioi->indices, &(ioi->indexfile_count));
    if(err<0){
      perror("DEFWRITER: Reading indices");
      return -1;
    }
    else{
      opt->buf_elem_size = ioi->elem_size;
#ifdef DEBUG_OUTPUT
      fprintf(stdout, "Element size is %lu\n", opt->buf_elem_size);
#endif
    }
  }
  else{
    ioi->elem_size = opt->buf_elem_size;
  }
  return err;
}
INDEX_FILE_TYPE * common_pindex(struct recording_entity *re){
  struct common_io_info * ioi = re->opt;
  return ioi->indices;
  //return ((struct common_io_info)re->opt)->indices;
}
unsigned long common_nofpacks(struct recording_entity *re){
  struct common_io_info * ioi = re->opt;
  return ioi->indexfile_count;
  //return ((struct common_io_info*)re->opt)->indexfile_count;
}
int common_close(struct recording_entity * re, void * stats){
  int err;
  struct common_io_info * ioi = (struct common_io_info*)re->opt;

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
      perror("COMMON_WRT: ftruncate");
  }
  else{
    free(ioi->indices);
    /* No need to close indice-file since it was read into memory */
  }
  close(ioi->fd);

  //ioi->ctx = NULL;
  free(ioi->filename);

  free(ioi);
#ifdef DEBUG_OUTPUT
  fprintf(stdout, "COMMON_WRT: Writer closed\n");
#endif
  return 0;
}
const char * common_wrt_get_filename(struct recording_entity *re){
  return ((struct common_io_info *)re->opt)->filename;
}
