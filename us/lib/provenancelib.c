/*
*
* provenancelib.c
*
* Author: Thomas Pasquier <tfjmp2@cam.ac.uk>
*
* Copyright (C) 2015 University of Cambridge
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2, as
* published by the Free Software Foundation.
*
*/
#include <sys/stat.h>
#include <sys/poll.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "thpool.h"
#include "provenancelib.h"

#define NUMBER_CPUS 256 /* support 256 core max */
#define BASE_NAME "/sys/kernel/debug/provenance"

/* internal variables */
static struct provenance_ops prov_ops;
static uint8_t ncpus;
/* per cpu variables */
static int relay_file[NUMBER_CPUS];
/* worker pool */
static threadpool worker_thpool=NULL;

/* internal functions */
static int open_files(void);
static int close_files(void);
static int create_worker_pool(void);
static int destroy_worker_pool(void);

static void callback_job(void* data);
static void reader_job(void *data);


int provenance_register(struct provenance_ops* ops)
{
  /* copy ops function pointers */
  memcpy(&prov_ops, ops, sizeof(struct provenance_ops));

  /* count how many CPU */
  ncpus = sysconf(_SC_NPROCESSORS_ONLN);
  if(ncpus>NUMBER_CPUS){
    return -1;
  }

  /* open relay files */
  if(open_files()){
    return -1;
  }

  /* create callback threads */
  if(create_worker_pool()){
    close_files();
    return -1;
  }
  return 0;
}

void provenance_stop()
{
  close_files();
  destroy_worker_pool();
}

static int open_files(void)
{
  int i;
  char tmp[4096]; // to store file name

  for(i=0; i<ncpus; i++){
    sprintf(tmp, "%s%d", BASE_NAME, i);
    relay_file[i] = open(tmp, O_RDONLY | O_NONBLOCK);
    if(relay_file[i]<0){
      return -1;
    }
  }
  return 0;
}

static int close_files(void)
{
  int i;
  for(i=0; i<ncpus;i++){
    close(relay_file[i]);
  }
  return 0;
}

static int create_worker_pool(void)
{
  int i;
  uint8_t* cpunb;
  worker_thpool = thpool_init(ncpus*2);
  /* set reader jobs */
  for(i=0; i<ncpus; i++){
    cpunb = (uint8_t*)malloc(sizeof(uint8_t)); // will be freed in worker
    (*cpunb)=i;
    thpool_add_work(worker_thpool, (void*)reader_job, (void*)cpunb);
  }
}

static int destroy_worker_pool(void)
{
  thpool_wait(worker_thpool); // wait for all jobs in queue to be finished
  thpool_destroy(worker_thpool); // destory all worker threads
}

/* per worker thread initialised variable */
static __thread int initialised=0;

/* handle application callbacks */
static void callback_job(void* data)
{
  prov_msg_t* msg = (prov_msg_t*)data;

  /* initialise per worker thread */
  if(!initialised && prov_ops.init!=NULL){
    prov_ops.init();
    initialised=1;
  }

  switch(msg->msg_info.message_id){
    case MSG_EDGE:
      if(prov_ops.log_edge!=NULL)
        prov_ops.log_edge(&(msg->edge_info));
      break;
    case MSG_NODE:
      if(prov_ops.log_node!=NULL)
        prov_ops.log_node(&(msg->node_info));
      break;
    case MSG_STR:
      if(prov_ops.log_str!=NULL)
        prov_ops.log_str(&(msg->str_info));
      break;
    default:
      break;
  }
  free(data); /* free the memory allocated in the reader */
}

/* read from relayfs file */
static void reader_job(void *data)
{
  uint8_t* buf;
  int rc;
  uint8_t cpu = (uint8_t)(*(uint8_t*)data);
  free(data); // free from caller
  struct pollfd pollfd;

  pollfd.fd=relay_file[cpu];
  do{
    /* file to look on */
    pollfd.fd = relay_file[cpu];
    /* something to read */
		pollfd.events = POLLIN;
    /* one file, timeout 100ms */
    rc = poll(&pollfd, 1, 100);
    if(rc<0){
      if(errno!=EINTR){
        break; /* something bad happened */
      }
    }
    buf = (uint8_t*)malloc(sizeof(prov_msg_t)); /* freed by worker thread */
    rc = read(relay_file[cpu], buf, sizeof(prov_msg_t));
    if(!rc){ /* we did not read anything */
      continue;
    }
    if(rc<0){
      if(errno==EAGAIN){ // retry
        continue;
      }
      break; // something bad happened
    }
    /* add job to queue */
    thpool_add_work(worker_thpool, (void*)callback_job, buf);
  }while(1);
}