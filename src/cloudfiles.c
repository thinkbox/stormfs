/*
 * stormfs - A FUSE abstraction layer for cloud storage
 * Copyright (C) 2011 Ben LeMasurier <ben.lemasurier@gmail.com>
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#define _GNU_SOURCE

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib.h>
#include "stormfs.h"
#include "cloudfiles.h"
#include "cloudfiles-curl.h"
#include "curl.h"

struct cloudfiles {
  struct stormfs *stormfs;
} cloudfiles;

static int
headers_to_stat(GList *headers, struct stat *stbuf)
{
  GList *head = NULL,
        *next = NULL;

  head = g_list_first(headers);
  while(head != NULL) {
    next = head->next;
    HTTP_HEADER *header = head->data;

    // TODO: clean this up.
    if(strcmp(header->key, "X-Object-Meta-uid") == 0)
      stbuf->st_uid = get_uid(header->value);
    else if(strcmp(header->key, "X-Object-Meta-gid") == 0)
      stbuf->st_gid = get_gid(header->value);
    else if(strcmp(header->key, "X-Object-Meta-ctime") == 0)
      stbuf->st_ctime = get_ctime(header->value);
    else if(strcmp(header->key, "X-Object-Meta-mtime") == 0)
      stbuf->st_mtime = get_mtime(header->value);
    else if(strcmp(header->key, "X-Object-Meta-rdev") == 0)
      stbuf->st_rdev = get_rdev(header->value);
    else if(strcmp(header->key, "Last-Modified") == 0 && stbuf->st_mtime == 0)
      stbuf->st_mtime = get_mtime(header->value);
    else if(strcmp(header->key, "X-Object-Meta-mode") == 0)
      stbuf->st_mode = get_mode(header->value);
    else if(strcmp(header->key, "Content-Length") == 0)
      stbuf->st_size = get_size(header->value);
    else if(strcmp(header->key, "Content-Type") == 0)
      if(strstr(header->value, "x-directory"))
        stbuf->st_mode |= S_IFDIR;

    head = next;
  }

  return 0;
}

static GList *
objectlist_to_files(const char *path, char *xml)
{
  GList *files = NULL;
  char *tmp = strdup(xml), *p = NULL;

  p = strtok(tmp, "\r\n");
  while(p != NULL) {
    char *name;
    char *fullpath;

    name = strdup(p);
    fullpath = get_path(path, name);
    files = add_file_to_list(files, fullpath, NULL);
    free(name);
    free(fullpath);

    p = strtok(NULL, "\r\n");
  }

  free(tmp);

  return files;
}

void
cloudfiles_destroy(void)
{
}

int
cloudfiles_getattr(const char *path, struct stat *st)
{
  int result;
  GList *headers = NULL;

  if((result = cloudfiles_curl_head(path, &headers)) != 0) {
    free_headers(headers);
    return result;
  }

  result = headers_to_stat(headers, st);
  free_headers(headers);

  return result;
}

int
cloudfiles_getattr_multi(const char *path, GList *files)
{
  return -ENOTSUP;
}

int
cloudfiles_chmod(const char *path, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_chown(const char *path, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_create(const char *path, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_init(struct stormfs *stormfs)
{
  cloudfiles.stormfs = stormfs;

  if(cloudfiles_curl_init(stormfs) != 0) {
    fprintf(stderr, "%s: unable to initialize libcurl\n", stormfs->progname);
    exit(EXIT_FAILURE);
  }

  return 0;
}

int
cloudfiles_mkdir(const char *path, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_mknod(const char *path, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_open(const char *path, FILE *f)
{
  return -ENOTSUP;
}

int
cloudfiles_readdir(const char *path, GList **files)
{
  int result;
  char *data = NULL;

  if((result = cloudfiles_curl_list_objects(path, &data)) != 0) {
    free(data);
    return -EIO;
  }

  *files = objectlist_to_files(path, data);
  free(data);

  return result;
}

int
cloudfiles_release(const char *path, int fd, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_rename(const char *from, const char *to, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_rmdir(const char *path)
{
  return -ENOTSUP;
}

int
cloudfiles_symlink(const char *from, const char *to, struct stat *st)
{
  return -ENOTSUP;
}

int
cloudfiles_unlink(const char *path)
{
  return -ENOTSUP;
}

int
cloudfiles_utimens(const char *path, struct stat *st)
{
  return -ENOTSUP;
}
