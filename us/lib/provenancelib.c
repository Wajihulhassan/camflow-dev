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

#define NUMBER_CPUS           256 /* support 256 core max */
#define PROV_BASE_NAME        "/sys/kernel/debug/provenance"
#define LONG_PROV_BASE_NAME   "/sys/kernel/debug/long_provenance"

#define ENABLE_FILE           "/sys/kernel/security/provenance/enable"
#define ALL_FILE              "/sys/kernel/security/provenance/all"
#define OPAQUE_FILE           "/sys/kernel/security/provenance/opaque"
#define NODE_FILE           "/sys/kernel/security/provenance/node"
#define EDGE_FILE           "/sys/kernel/security/provenance/edge"

/* internal variables */
static struct provenance_ops prov_ops;
static uint8_t ncpus;
/* per cpu variables */
static int relay_file[NUMBER_CPUS];
static int long_relay_file[NUMBER_CPUS];
/* worker pool */
static threadpool worker_thpool=NULL;

/* internal functions */
static int open_files(void);
static int close_files(void);
static int create_worker_pool(void);
static int destroy_worker_pool(void);

static void callback_job(void* data);
static void long_callback_job(void* data);
static void reader_job(void *data);
static void long_reader_job(void *data);


int provenance_register(struct provenance_ops* ops)
{
  int err;
  /* the provenance usher will not appear in trace */
  err = provenance_set_opaque(true);
  if(err)
  {
    return err;
  }
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
    sprintf(tmp, "%s%d", PROV_BASE_NAME, i);
    relay_file[i] = open(tmp, O_RDONLY | O_NONBLOCK);
    if(relay_file[i]<0){
      return -1;
    }
    sprintf(tmp, "%s%d", LONG_PROV_BASE_NAME, i);
    long_relay_file[i] = open(tmp, O_RDONLY | O_NONBLOCK);
    if(long_relay_file[i]<0){
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
    close(long_relay_file[i]);
  }
  return 0;
}

static int create_worker_pool(void)
{
  int i;
  uint8_t* cpunb;
  worker_thpool = thpool_init(ncpus*4);
  /* set reader jobs */
  for(i=0; i<ncpus; i++){
    cpunb = (uint8_t*)malloc(sizeof(uint8_t)); // will be freed in worker
    (*cpunb)=i;
    thpool_add_work(worker_thpool, (void*)reader_job, (void*)cpunb);
    thpool_add_work(worker_thpool, (void*)long_reader_job, (void*)cpunb);
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

  switch(msg->msg_info.message_type){
    case MSG_EDGE:
      if(prov_ops.log_edge!=NULL)
        prov_ops.log_edge(&(msg->edge_info));
      break;
    case MSG_TASK:
      if(prov_ops.log_task!=NULL)
        prov_ops.log_task(&(msg->task_info));
      break;
    case MSG_INODE:
      if(prov_ops.log_inode!=NULL)
        prov_ops.log_inode(&(msg->inode_info));
      break;
    case MSG_DISC_NODE:
      if(prov_ops.log_disc!=NULL)
        prov_ops.log_disc(&(msg->disc_node_info));
      break;
    default:
      printf("Error: unknown message type %u\n", msg->msg_info.message_type);
      break;
  }
  free(data); /* free the memory allocated in the reader */
}

/* handle application callbacks */
static void long_callback_job(void* data)
{
  long_prov_msg_t* msg = (long_prov_msg_t*)data;

  /* initialise per worker thread */
  if(!initialised && prov_ops.init!=NULL){
    prov_ops.init();
    initialised=1;
  }

  switch(msg->msg_info.message_type){
    case MSG_STR:
      if(prov_ops.log_str!=NULL)
        prov_ops.log_str(&(msg->str_info));
      break;
    case MSG_LINK:
      if(prov_ops.log_link!=NULL)
        prov_ops.log_link(&(msg->link_info));
      break;
    case MSG_UNLINK:
      if(prov_ops.log_unlink!=NULL)
        prov_ops.log_unlink(&(msg->unlink_info));
      break;
    default:
      printf("Error: unknown message type %u\n", msg->msg_info.message_type);
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
  struct pollfd pollfd;

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
    if(rc==0){ /* we did not read anything */
      continue;
    }
    if(rc<0){
      if(errno==EAGAIN){ // retry
        continue;
      }
      break; // something bad happened
    }
    if(rc!=sizeof(prov_msg_t)){
      perror("Did not read a full message");
      printf("read: %d\n", rc);
      /* to we need to seek? */
      break; // something bad happened
    }
    /* add job to queue */
    thpool_add_work(worker_thpool, (void*)callback_job, buf);
  }while(1);
}

/* read from relayfs file */
static void long_reader_job(void *data)
{
  uint8_t* buf;
  int rc;
  uint8_t cpu = (uint8_t)(*(uint8_t*)data);
  struct pollfd pollfd;

  do{
    /* file to look on */
    pollfd.fd = long_relay_file[cpu];
    /* something to read */
		pollfd.events = POLLIN;
    /* one file, timeout 100ms */
    rc = poll(&pollfd, 1, 100);
    if(rc<0){
      if(errno!=EINTR){
        break; /* something bad happened */
      }
    }
    buf = (uint8_t*)malloc(sizeof(long_prov_msg_t)); /* freed by worker thread */
    rc = read(long_relay_file[cpu], buf, sizeof(long_prov_msg_t));
    if(rc==0){ /* we did not read anything */
      continue;
    }
    if(rc<0){
      if(errno==EAGAIN){ // retry
        continue;
      }
      break; // something bad happened
    }
    if(rc!=sizeof(long_prov_msg_t)){
      rc = read(long_relay_file[cpu], buf+rc, sizeof(long_prov_msg_t)-rc);
      if(rc<0){
        break;
      }
    }
    /* add job to queue */
    thpool_add_work(worker_thpool, (void*)long_callback_job, buf);
  }while(1);
}

int provenance_set_enable(bool value){
  int fd = open(ENABLE_FILE, O_WRONLY);

  if(fd<0)
  {
    return fd;
  }
  if(value)
  {
    write(fd, "1", sizeof(char));
  }else{
    write(fd, "0", sizeof(char));
  }
  close(fd);
  return 0;
}

int provenance_set_all(bool value){
  int fd = open(ALL_FILE, O_WRONLY);

  if(fd<0)
  {
    return fd;
  }
  if(value)
  {
    write(fd, "1", sizeof(char));
  }else{
    write(fd, "0", sizeof(char));
  }
  close(fd);
  return 0;
}

int provenance_set_opaque(bool value){
  int fd = open(OPAQUE_FILE, O_WRONLY);

  if(fd<0)
  {
    return fd;
  }
  if(value)
  {
    write(fd, "1", sizeof(char));
  }else{
    write(fd, "0", sizeof(char));
  }
  close(fd);
  return 0;
}

int provenance_disclose_node(struct disc_node_struct* node){
  int rc;
  int fd = open(NODE_FILE, O_WRONLY);

  if(fd<0)
  {
    return fd;
  }
  rc = write(fd, node, sizeof(struct disc_node_struct));
  close(fd);
  return rc;
}

int provenance_disclose_edge(struct edge_struct* edge){
  int rc;
  int fd = open(EDGE_FILE, O_WRONLY);

  if(fd<0)
  {
    return fd;
  }
  rc = write(fd, edge, sizeof(struct edge_struct));
  close(fd);
  return rc;
}
