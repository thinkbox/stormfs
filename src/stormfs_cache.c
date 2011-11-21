/*
 * stormfs - A FUSE abstraction layer for cloud storage
 * Copyright (C) 2011 Ben LeMasurier <ben.lemasurier@gmail.com>
 *
 * The contents of this file owe great credit to Miklos Szeredi and sshfs.
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <fuse.h>
#include <fuse_opt.h>
#include <pthread.h>
#include "stormfs_cache.h"

#define DEFAULT_CACHE_TIMEOUT 20
#define MAX_CACHE_SIZE 10000
#define MIN_CACHE_CLEAN_INTERVAL 5
#define CACHE_CLEAN_INTERVAL 60

static struct cache {
  int on;
  time_t last_cleaned;
  unsigned stat_timeout;
  GHashTable *table;
  pthread_mutex_t lock;
  struct fuse_cache_operations *next_oper;
} cache;

struct node {
  time_t valid;
  time_t stat_valid;
  struct stat stat;
};

static struct node *
cache_lookup(const char *path)
{
  return (struct node *) g_hash_table_lookup(cache.table, path);
}

static struct node *
cache_get(const char *path)
{
  struct node *node = cache_lookup(path);
  if(node == NULL) {
    char *tmp = strdup(path);
    node = g_new0(struct node, 1);
    g_hash_table_insert(cache.table, tmp, node);
  }

  return node;
}

static void
free_node(gpointer node_)
{
  struct node *node = (struct node *) node_;
  g_free(node);
}

static int
cache_clean_entry(void *key_, struct node *node, time_t *now)
{
  (void) key_;
  if(*now > node->valid)
    return TRUE;
  else
    return FALSE;
}

static void
cache_clean(void)
{
  time_t now = time(NULL);
  if(now > cache.last_cleaned + MIN_CACHE_CLEAN_INTERVAL &&
      (g_hash_table_size(cache.table) > MAX_CACHE_SIZE ||
       now > cache.last_cleaned + CACHE_CLEAN_INTERVAL)) {
    g_hash_table_foreach_remove(cache.table,
        (GHRFunc) cache_clean_entry, &now);
    cache.last_cleaned = now;
  }
}

static void
cache_purge(const char *path)
{
  g_hash_table_remove(cache.table, path);
}

void
cache_invalidate(const char *path)
{
  if(!cache.on) 
    return;

  pthread_mutex_lock(&cache.lock);
  cache_purge(path);
  pthread_mutex_unlock(&cache.lock);
}

void
cache_add_attr(const char *path, const struct stat *stbuf)
{
  time_t now;
  struct node *node;

  if(!cache.on)
    return;

  pthread_mutex_lock(&cache.lock);
  node = cache_get(path);
  now  = time(NULL);
  node->stat = *stbuf;
  node->stat_valid = time(NULL) + cache.stat_timeout;
  if(node->stat_valid > node->valid)
    node->valid = node->stat_valid;
  cache_clean();
  pthread_mutex_unlock(&cache.lock);
}

static int
cache_get_attr(const char *path, struct stat *stbuf)
{
  int result = -EAGAIN;
  struct node *node;

  pthread_mutex_lock(&cache.lock);
  node = cache_lookup(path);
  if(node != NULL) {
    time_t now = time(NULL);
    if(node->stat_valid - now >= 0) {
      *stbuf = node->stat;
      result = 0;
    }
  }
  pthread_mutex_unlock(&cache.lock);

  return result;
}

static int
cache_getattr(const char *path, struct stat *stbuf)
{
  int result;
  if((result = cache_get_attr(path, stbuf)) != 0) {
    result = cache.next_oper->oper.getattr(path, stbuf);
    if(result == 0)
      cache_add_attr(path, stbuf);
  }

  return result;
}

static int
cache_utimens(const char *path, const struct timespec ts[2])
{
  int result = cache.next_oper->oper.utimens(path, ts);
  if(result == 0)
    cache_invalidate(path);

  return result;
}

static void
cache_unity_fill(struct fuse_cache_operations *oper, 
    struct fuse_operations *cache_oper)
{
  cache_oper->create   = oper->oper.create;
  cache_oper->chmod    = oper->oper.chmod;
  cache_oper->chown    = oper->oper.chown;
  cache_oper->destroy  = oper->oper.destroy;
  cache_oper->getattr  = oper->oper.getattr;
  cache_oper->init     = oper->oper.init;
  cache_oper->flush    = oper->oper.flush;
  cache_oper->mkdir    = oper->oper.mkdir;
  cache_oper->mknod    = oper->oper.mknod;
  cache_oper->open     = oper->oper.open;
  cache_oper->read     = oper->oper.read;
  cache_oper->readdir  = oper->oper.readdir;
  cache_oper->readlink = oper->oper.readlink;
  cache_oper->release  = oper->oper.release;
  cache_oper->rename   = oper->oper.rename;
  cache_oper->rmdir    = oper->oper.rmdir;
  cache_oper->statfs   = oper->oper.statfs;
  cache_oper->symlink  = oper->oper.symlink;
  cache_oper->truncate = oper->oper.truncate;
  cache_oper->unlink   = oper->oper.unlink;
  cache_oper->utimens  = oper->oper.utimens;
  cache_oper->write    = oper->oper.write;
}

static void
cache_fill(struct fuse_cache_operations *oper,
    struct fuse_operations *cache_oper)
{
  cache_oper->getattr = oper->oper.getattr ? cache_getattr : NULL;
  cache_oper->utimens = oper->oper.utimens ? cache_utimens : NULL;
}

struct fuse_operations *
cache_init(struct fuse_cache_operations *oper)
{
  static struct fuse_operations cache_oper;
  cache.next_oper = oper;

  cache_unity_fill(oper, &cache_oper);
  if(cache.on) {
    cache_fill(oper, &cache_oper);
    pthread_mutex_init(&cache.lock, NULL);
    cache.table = g_hash_table_new_full(g_str_hash, g_str_equal,
        g_free, free_node);
    if(cache.table == NULL) {
      fprintf(stderr, "error initializing cache\n");
      return NULL;
    }
  }

  return &cache_oper;
}

static const struct fuse_opt cache_opts[] = {
  {"cache=yes",             offsetof(struct cache, on), 1},
  {"cache=no",              offsetof(struct cache, on), 0},
  {"cache_timeout=%u",      offsetof(struct cache, stat_timeout), 0},
  {"cache_stat_timeout=%u", offsetof(struct cache, stat_timeout), 0},
  FUSE_OPT_END
};

int
cache_parse_options(struct fuse_args *args)
{
  cache.on = 1;
  cache.stat_timeout = DEFAULT_CACHE_TIMEOUT;

  return fuse_opt_parse(args, &cache, cache_opts, NULL);
}