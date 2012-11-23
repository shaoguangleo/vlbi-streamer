/*
 * resourcetree.c -- Resource tree functions for vlbi-streamer
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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "resourcetree.h"
#include "common_wrt.h"
#include "streamer.h"

extern FILE* logfile;

inline void add_before_ent(struct listed_entity * new, struct listed_entity * old){
  new->child = old;
  new->father = old->father;
  if(old->father != NULL)
    old->father->child = new;
  old->father = new;
}

void add_to_next(struct listed_entity **root, struct listed_entity *toadd, int (*compare)(void*,void*))
{
  toadd->child = NULL;
  toadd->father = NULL;
  if(*root == NULL){
    (*root) = toadd;
  }
  else{
    while((*root) != NULL){
      if(compare != NULL){
	if(compare(toadd->entity, (*root)->entity) == 1){
	  D("Found spot for new entity");
	  struct listed_entity* temp = *root;
	  if((*root)->father == NULL){
	    D("We have a new root element!");
	    *root = toadd;
	  }
	  add_before_ent(toadd, temp);
	  return;
	}
      }
      if((*root)->child == NULL)
	break;
      root = &((*root)->child);
    }
    toadd->father = *root;
    //toadd->child = NULL;
    (*root)->child= toadd;
  }
}
/* Initial add */
void add_to_entlist(struct entity_list_branch* br, struct listed_entity* en)
{
  if(br->mutex_free == 0)
    LOCK(&(br->branchlock));
  add_to_next(&(br->freelist), en, NULL);
  if(br->mutex_free == 0)
    UNLOCK(&(br->branchlock));
}
void mutex_free_change_branch(struct listed_entity **from, struct listed_entity **to, struct listed_entity *en)
{
  if(en == *from){
    *from = en->child;
    if(en->child != NULL)
      en->child->father = NULL;
  }
  else
  {
    en->father->child = en->child;
    if(en->child != NULL)
      en->child->father = en->father;
  }
  add_to_next(to, en, NULL);
}
/* Set this entity into the free to use list		*/
void set_free(struct entity_list_branch *br, struct listed_entity* en)
{
  LOCK(&(br->branchlock));
  //Only special case if the entity is at the start of the list
  D("Changing entity from busy to free");
  mutex_free_change_branch(&(br->busylist), &(br->freelist), en);
  if(en->release != NULL){
    D("Running release on entity");
    int ret = en->release(en->entity);
    if(ret != 0)
      E("Release returned non zero value.(Not handled in any way)");
  }
  D("Entity free'd. Signaling");
  pthread_cond_broadcast(&(br->busysignal));
  UNLOCK(&(br->branchlock));
}
void set_loaded(struct entity_list_branch *br, struct listed_entity* en){
  D("Setting entity to loaded");
  LOCK(&(br->branchlock));
  mutex_free_change_branch(&(br->busylist), &(br->loadedlist), en);
  pthread_cond_broadcast(&(br->busysignal));
  UNLOCK(&(br->branchlock));
}
void mutex_free_set_busy(struct entity_list_branch *br, struct listed_entity* en)
{
  mutex_free_change_branch(&(br->freelist),&(br->busylist), en);
}
void block_until_free(struct entity_list_branch *br, void* val1){
  struct listed_entity * shouldntfind,* checker;
  LOCK(&(br->branchlock));
  do{
    checker = br->busylist;
    shouldntfind = NULL;
    while(checker != NULL && shouldntfind == NULL){
      if(checker->identify(checker->entity, val1, NULL, CHECK_BY_OPTPOINTER) == 1){
	shouldntfind = checker;
	break;
      }
      //if(strcmp(checker->getrecname(checker->entity),recname)== 0){
      else
	checker = checker->child;
    }
    /* Nobody will wake this up if its left to cond_wait */
    if(shouldntfind != NULL){
      pthread_cond_wait(&(br->busysignal), &(br->branchlock));
    }
    }
    while(shouldntfind != NULL);

    /* Release all loaded elements */
    for(checker = br->loadedlist;checker != NULL;){
      if(checker->identify(checker->entity, val1, NULL, CHECK_BY_OPTPOINTER) == 1){
	struct listed_entity *temp = checker;
	checker = checker->child;
	D("Moving loaded to free since thread is exiting");
	mutex_free_change_branch(&br->loadedlist, &br->freelist, temp);
	int ret = temp->release(temp->entity);
	if(ret != 0)
	  E("Release returned non zero value.(Not handled in any way)");
      }
      else
	checker = checker->child;
    }
    UNLOCK(&(br->branchlock));
  }
  void remove_from_branch(struct entity_list_branch *br, struct listed_entity *en, int mutex_free){
    D("Removing entity from branch");
    if(!mutex_free){
      LOCK(&(br->branchlock));
    }
    if(en == br->freelist){
      if(en->child != NULL)
	en->child->father = NULL;
      br->freelist = en->child;
    }
    else if(en == br->busylist){
      if(en->child != NULL)
	en->child->father = NULL;
      br->busylist = en->child;
    }
    else if(en == br->loadedlist){
      if(en->child != NULL)
	en->child->father = NULL;
      br->loadedlist = en->child;
    }
    else{
      /* Weird thing. Segfault when en->father is NULL	*/
      /* Shoulnd't happen but happens on vbs_shutdown	*/
      /* with live sending 				*/
      en->father->child = en->child;
      if(en->child != NULL)
	en->child->father = en->father;
    }
    en->child = NULL;
    en->father = NULL;

    /* This close only frees the entity structure, not the underlying opts etc. 	*/
    en->close(en->entity);
    free(en);

    if(!mutex_free){
      /* Signal so waiting threads can exit if the situation is bad(lost writers	*/
      pthread_cond_broadcast(&(br->busysignal));
      UNLOCK(&(br->branchlock));
    }
    D("Entity removed from branch");
  }
  struct listed_entity * loop_and_check(struct listed_entity* head, void* val1, void* val2, int iden_type){
    while(head != NULL){
      if(head->identify(head->entity, val1, val2, iden_type) == 1){
	return head;
      }
      else{
	head = head->child;
      }
    }
    return NULL;
  }
#define FREE_AND_RETURN_LE \
  if(!mutex_free){\
    UNLOCK(&(br->branchlock));\
  }\
  return le;

  struct listed_entity* get_from_all(struct entity_list_branch *br, void *val1, void * val2, int iden_type, int mutex_free){
    struct listed_entity *le = NULL;
    if(mutex_free == 0){
      LOCK(&(br->branchlock));
    }
    if((le = loop_and_check(br->freelist, val1, val2, iden_type)) != NULL){
      FREE_AND_RETURN_LE
    }
    if((le = loop_and_check(br->busylist, val1, val2, iden_type)) != NULL){
      FREE_AND_RETURN_LE
    }
    if((le = loop_and_check(br->loadedlist, val1, val2, iden_type)) != NULL){
      FREE_AND_RETURN_LE
    }
    if(mutex_free == 0){
      UNLOCK(&(br->branchlock));
    }
    return NULL;
  }
#define CHECK_LOADED 1
#define CHECK_BUSY 2
#define CHECK_FREE 3
  //#define CHECK_ANY_FREE 4
  struct listed_entity* get_w_check(struct entity_list_branch *br, int branch_to_check,unsigned long seq,  void* optmatch){
    //struct listed_entity *temp;
    struct listed_entity *le = NULL;
    struct listed_entity **lep = NULL;
    struct listed_entity **other = NULL;
    struct listed_entity **other2 = NULL;
    if(branch_to_check == CHECK_FREE){
      other = &br->busylist;
      other2 = &br->loadedlist;
      lep = &br->freelist;
    }
    else if (branch_to_check == CHECK_BUSY){
      other = &br->freelist;
      other2 = &br->loadedlist;
      lep = &br->busylist;
    }
    else if (branch_to_check == CHECK_LOADED){
      other = &br->freelist;
      other2 = &br->busylist;
      lep = &br->loadedlist;
    }
    else{
      E("Queried list is something weird. Should be loaded,free,or busy of the branch for checking of missing");
      return NULL;
    }
    //le = NULL;
    while(le== NULL){
      le = loop_and_check(*lep, &seq, optmatch, CHECK_BY_SEQ);
      /* If le wasn't found in the list */
      if(le == NULL){
	/* Check if branch is dead */
	D("Checking for dead branch");
	if(*lep == NULL && *other == NULL && *other2 == NULL){
	  E("No entities in list. Returning NULL");
	  return NULL;
	}
	/* Need to check if it exists at all */
	if(optmatch == NULL){
	  D("Looping to check if exists");
	  if(loop_and_check(*other, &seq, NULL, CHECK_BY_SEQ) == NULL && loop_and_check(*other2,&seq, NULL, CHECK_BY_SEQ) == NULL){
	    D("Rec point disappeared!");
	    return NULL;
	  }
	}
	D("Failed to get specific. Sleeping waiting for %lu",,seq);
	pthread_cond_wait(&(br->busysignal), &(br->branchlock));
	D("Woke up! Checking for %lu again",, seq);
      }
    }
    D("Found specific elem id %lu!",, seq);
    return le;
  }
  /* Get a loaded buffer with the specific seq */
  inline void* get_loaded(struct entity_list_branch *br, unsigned long seq, void* opt){
    D("Querying for loaded entity %lu",, seq);
    LOCK(&(br->branchlock));
    struct listed_entity * temp = get_w_check(br, CHECK_LOADED ,seq,  opt);

    if (temp == NULL){
      D("Nothing to return!");
      UNLOCK(&(br->branchlock));
      return NULL;
    }

    mutex_free_change_branch(&(br->loadedlist), &(br->busylist), temp);
    UNLOCK(&(br->branchlock));
    D("Returning loaded entity");
    return temp->entity;
  }
  void* get_lingering(struct entity_list_branch * br, void* opt, void* fhh, int just_check){
    struct listed_entity * temp = NULL;
    struct fileholder* fh = (struct fileholder*)fhh;
    LOCK(&(br->branchlock));
    D("Checking if %lu is already in the buffers",, fh->id);
    temp = loop_and_check(br->freelist, (void*)&(fh->id), (void*)opt, CHECK_BY_OLDSEQ);
    if(temp !=NULL && just_check == 0){
      D("File %lu found in buffer! Setting to loaded",, fh->id);
      mutex_free_change_branch(&(br->freelist), &(br->loadedlist), temp);
      if(temp->acquire !=NULL){
	D("Running acquire on entity");
	int ret = temp->acquire(temp->entity, opt,((void*)fh));
	if(ret != 0)
	  E("Acquire return non-zero value(Not handled)");
      }
    }
    UNLOCK(&(br->branchlock));
    if(temp != NULL)
      return temp->entity;
    else
      return NULL;
  }
  /* Get a specific free entity from branch 		*/
  void* get_specific(struct entity_list_branch *br,void * opt,unsigned long seq, unsigned long bufnum, unsigned long id, int* acquire_result)
  {
    (void)bufnum;
    LOCK(&(br->branchlock));
    struct listed_entity* temp = get_w_check(br,CHECK_FREE ,id, NULL);

    if(temp ==NULL){
      UNLOCK(&(br->branchlock));
      if(acquire_result !=NULL)
	*acquire_result = -1;
      return NULL;
    }

    mutex_free_change_branch(&(br->freelist), &(br->busylist), temp);
    UNLOCK(&(br->branchlock));
    if(temp->acquire !=NULL){
      D("Running acquire on entity");
      int ret = temp->acquire(temp->entity, opt,((void*)&seq));
      if(acquire_result != NULL)
	*acquire_result = ret;
      else{
	if(ret != 0)
	  E("Acquire return non-zero value(Not handled)");
      }
    }
    else
      D("Entity doesn't have an acquire-function");
    D("Returning specific free entity");
    return temp->entity;
  }
  /* Get a free entity from the branch			*/
  void* get_free(struct entity_list_branch *br,void * opt,void* acq, int* acquire_result)
  {
    struct listed_entity *temp = NULL;
    int suitable=0;
    LOCK(&(br->branchlock));
    while(temp == NULL){
      temp = br->freelist;
      //while(br->freelist == NULL){
      while(temp == NULL)
      {
      /* Check if list is empty */
	if(br->busylist == NULL && br->loadedlist == NULL){
	  D("No entities in list. Returning NULL");
	  UNLOCK(&(br->branchlock));
	  return NULL;
	}
	LOG("Failed to get free buffer: Resources might be running out! Sleeping");
	pthread_cond_wait(&(br->busysignal), &(br->branchlock));
	/* Check if something was added to freelist */
	temp = br->freelist;
      }
      if(temp->check != NULL)
      {
	while(temp != NULL && (suitable = temp->check(temp->entity, opt)) != 0){
	  D("Not fit!");
	  temp = temp->child;
	}
      }
    }
    //struct listed_entity * temp = br->freelist;
    //mutex_free_set_busy(br, br->freelist);
    mutex_free_set_busy(br, temp);
    UNLOCK(&(br->branchlock));
    if(temp->acquire !=NULL){
      D("Running acquire on entity");
      int ret = temp->acquire(temp->entity, opt,acq);
      if(acquire_result != NULL)
	*acquire_result = ret;
      else{
	if(ret != 0)
	  E("Acquire return non-zero value(Not handled)");
      }
    }
    else
      D("Entity doesn't have an acquire-function");
    return temp->entity;
  }
  /* Set this entity as busy in this branch		*/
  inline void set_busy(struct entity_list_branch *br, struct listed_entity* en)
  {
    LOCK(&(br->branchlock));
    mutex_free_set_busy(br,en);
    UNLOCK(&(br->branchlock));
  }
  void print_br_stats(struct entity_list_branch *br){
    int free=0,busy=0,loaded=0;
    LOCK(&(br->branchlock));
    struct listed_entity *le = br->freelist;
    while(le != NULL){
      free++;
      le = le->child;
    }
    le = br->busylist;
    while(le != NULL){
      busy++;
      le = le->child;
    }
    le = br->loadedlist;
    while(le != NULL){
      loaded++;
      le = le->child;
    }
    UNLOCK(&(br->branchlock));
    LOG("Free:\t%d\tBusy:\t%d\tLoaded:\t%d\n", free, busy, loaded);
  }
  /* Loop through all entities and do specified OP */
  /* Don't want to write this same thing 4 times , so I'll just add an operation switch */
  /* for it */
  void oper_to_list(struct entity_list_branch *br,struct listed_entity *le, int operation, void*param){
    struct listed_entity * removable = NULL;
    //struct buffer_entity *be;
    while(le != NULL){
      switch(operation){
	case BRANCHOP_STOPANDSIGNAL:
	  ((struct buffer_entity*)le->entity)->stop((struct buffer_entity*)le->entity);
	  break;
	case BRANCHOP_GETSTATS:
	  get_io_stats(((struct recording_entity*)(le->entity))->opt, (struct stats*)param);
	  break;
	case BRANCHOP_CLOSERBUF:
	  ((struct buffer_entity*)le->entity)->close(((struct buffer_entity*)le->entity), param);
	  removable = le;
	  break;
	case BRANCHOP_CLOSEWRITER:
	  D("Closing writer");
	  ((struct recording_entity*)le->entity)->close(((struct recording_entity*)le->entity),param);
	  removable = le;
	  //D("Writer closed");
	  break;
	case BRANCHOP_WRITE_CFGS:
	  D("Writing cfg");
	  ((struct recording_entity*)le->entity)->writecfg(((struct recording_entity*)le->entity), param);
	  break;
	case BRANCHOP_READ_CFGS:
	  ((struct recording_entity*)le->entity)->readcfg(((struct recording_entity*)le->entity), param);
	  break;
	case BRANCHOP_CHECK_FILES:
	  ((struct recording_entity*)le->entity)->check_files(((struct recording_entity*)le->entity), param);
	  break;
      }
      le = le->child;
      if(removable != NULL){
	remove_from_branch(br,removable,1);
	//free(removable);
      }
    }
  }
  void oper_to_all(struct entity_list_branch *br, int operation,void* param)
  {
    LOCK(&(br->branchlock));
    oper_to_list(br,br->freelist,operation,param);
    oper_to_list(br,br->busylist,operation, param);
    oper_to_list(br,br->loadedlist,operation, param);
    UNLOCK(&(br->branchlock));
  }
