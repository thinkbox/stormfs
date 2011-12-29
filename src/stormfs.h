/*
 * stormfs - A FUSE abstraction layer for cloud storage
 * Copyright (C) 2011 Ben LeMasurier <ben.lemasurier@gmail.com>
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#ifndef stormfs_H
#define stormfs_H

#define FIVE_GB 5368709120LL

struct file {
  char *name;           /* file name */
  char *path;           /* file path */
  GList *headers;       /* http headers */
  struct stat *st;      /* stat(2) buffer */
  time_t valid;         /* entry timeout */
  pthread_mutex_t lock; /* file-level thread lock */
};

GList *add_optional_headers(GList *headers);
char *get_path(const char *path, const char *name);
char *stormfs_virtual_url(char *url, char *bucket);
void free_file(struct file *f);
GList *stat_to_headers(GList *headers, struct stat st);
int stormfs_getattr_multi(const char *path, GList *files);

#endif // stormfs_H
