#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "streamer.h"
#include "fanout.h"
#define CAPTURE_W_FANOUT 0
#define CAPTURE_W_TODO 1

extern char *optarg;
extern int optind, optopt;


static struct opt_s opt;
/*
static const char *device_name;
static int fanout_type;
static int fanout_id;
static int capture_type;
*/

/*
 * Stuff stolen from lindis sendfileudp
 */
static void usage(char *binary){
  fprintf(stderr, 
      "usage: %s [OPTION]... name time\n"
      "-i INTERFACE	Which interface to capture from\n"
      "-t {fanout|TODO	Capture type(fanout only one available atm)\n"
      "-a {lb|hash}	Fanout type\n"
      ,binary);
}
static void parse_options(int argc, char **argv){
  int ret;

  memset(&opt, 0, sizeof(struct opt_s));
  opt.filename = NULL;
  opt.device_name = NULL;
  opt.capture_type = CAPTURE_W_FANOUT;
  opt.fanout_type = PACKET_FANOUT_LB;
  opt.root_pid = getpid();
  for(;;){
    ret = getopt(argc, argv, "i:t:a:");
    if(ret == -1){
      break;
    }
    switch (ret){
      case 'i':
	opt.device_name = strdup(optarg);
	break;
      case 't':
	if (!strcmp(optarg, "fanout"))
	  opt.capture_type = CAPTURE_W_FANOUT;
	else if (!strcmp(optarg, "TODO"))
	  opt.capture_type = CAPTURE_W_TODO;
	//TODO: Add if other capture types added
	else {
	  fprintf(stderr, "Unknown packet capture type [%s]\n", optarg);
	  usage(argv[0]);
	  exit(1);
	}
	break;
      case 'a':
	if (!strcmp(optarg, "hash"))
	  opt.fanout_type = PACKET_FANOUT_HASH;
	else if (!strcmp(optarg, "lb"))
	  opt.fanout_type = PACKET_FANOUT_LB;
	else {
	  fprintf(stderr, "Unknown fanout type [%s]\n", optarg);
	  usage(argv[0]);
	  exit(1);
	}
	break;
      default:
	usage(argv[0]);
	exit(1);
    }
  }
  if(argc -optind != 2){
    usage(argv[0]);
    exit(1);
  }
  argv +=optind;
  argc -=optind;
  opt.filename = argv[0];
  opt.time = atoi(argv[1]);
}
int main(int argc, char **argv)
{
  //int fd, err;
  int i;

  /*
   * TODO: Make proper arg parsing
   * TODO: Make time dependent capture and add argument spot
   */
  /*
     if (argc != 4) {
     fprintf(stderr, "Usage: %s INTERFACE {fanout|TODO} {hash|lb}\n", argv[0]);
     return EXIT_FAILURE;
     }
     */

  parse_options(argc,argv);

  struct streamer_entity threads[THREADS];
  pthread_t pthreads_array[THREADS];
  //pthread_attr_t attr;
  void *status;
  int rc;

  //device_name = argv[1];
  //fanout_id = getpid() & 0xffff;

  //pthread_attr_init(&attr);
  //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  //Init all threads
  for(i=0;i<THREADS;i++){
    switch(opt.capture_type)
    {
      case CAPTURE_W_FANOUT:
	//threads[i].device_name = device_name;
	//TODO: Check if this can be used with pthreads identifying
	//threads[i].parent_pid = fanout_id;
	//threads[i].fanout_type = fanout_type;
	threads[i].open = setup_socket;
	//threads[i].open = setup_socket(&opt);
	threads[i].start = fanout_thread;
	threads[i].close = close_fanout;
	threads[i].opt = threads[i].open((void*)&opt);
	//threads[i].opt = &opt;
	break;
      case CAPTURE_W_TODO:
	break;
    }

  }
  for(i=0;i<THREADS;i++){
    switch(opt.capture_type)
    {
      case CAPTURE_W_FANOUT:
	printf("In main, starting thread %d\n", i);
	//rc = pthread_create(&pthreads_array[i], &attr, (void*)&threads[i].start, threads[i].opt);
	rc = pthread_create(&pthreads_array[i], NULL, threads[i].start, threads[i].opt);
	if (rc){
	  printf("ERROR; return code from pthread_create() is %d\n", rc);
	  exit(-1);
	}
	break;
      case CAPTURE_W_TODO:
	break;
    }
  }
  //pthread_attr_destroy(&attr);
  for (i = 0; i < THREADS; i++) {
    rc = pthread_join(pthreads_array[i], NULL);
    if (rc) {
      printf("ERROR; return code from pthread_join() is %d\n", rc);
      exit(-1);
    }
    //printf("Main: completed join with thread %ld having a status of %ld\n",t,(long)status);
  }
  //int status;

  //wait(&status);
  //Close all threads
  for(i=0;i<THREADS;i++){
    switch(opt.capture_type)
    {
      case CAPTURE_W_FANOUT:
	threads[i].close(threads[i].opt);
	break;
      case CAPTURE_W_TODO:
	break;
    }
  }

  //return 0;
  pthread_exit(NULL);
}
