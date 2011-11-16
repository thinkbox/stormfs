/*
 * stormfs - A FUSE abstraction layer for cloud storage
 * Copyright (C) 2011 Ben LeMasurier <ben.lemasurier@gmail.com>
 *
 * This program can be distributed under the terms of the GNU GPL.
 * See the file COPYING.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h>
#include <glib.h>
#include "stormfs_curl.h"

#define SHA1_BLOCK_SIZE 64
#define SHA1_LENGTH 20

struct stormfs_curl {
  const char *url;
  const char *bucket;
  const char *access_key;
  const char *secret_key;
} stormfs_curl;

typedef struct {
  char   *memory;
  size_t size;
} HTTP_RESPONSE;

static char *
gid_to_s(gid_t gid)
{
  char s[100];
  snprintf(s, 100, "%lu", (unsigned long) gid);

  return strdup(s);
}

static char *
uid_to_s(uid_t uid)
{
  char s[100];
  snprintf(s, 100, "%lu", (unsigned long) uid);

  return strdup(s);
}

static char *
mode_to_s(mode_t mode)
{
  char s[100];
  snprintf(s, 100, "%lu", (unsigned long) mode);

  return strdup(s);
}

static char *
time_to_s(time_t t)
{
  char s[100];
  snprintf(s, 100, "%ld", (long) t);

  return strdup(s);
}

char
char_to_hex(char c)
{
  static char hex[] = "0123456789abcdef";

  return hex[c & 15];
}

static int
cmpstringp(const void *p1, const void *p2)
{
  return strcmp(*(char **) p1, *(char **) p2);
}

char *
url_encode(char *s)
{
  char *p = s;
  char *buf = g_malloc((strlen(s) * 3) + 1);
  char *pbuf = buf;

  while(*p) {
    if(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') 
      *pbuf++ = *p;
    else if(*p == ' ') 
      *pbuf++ = '+';
    else 
      *pbuf++ = '%', *pbuf++ = char_to_hex(*p >> 4), *pbuf++ = char_to_hex(*p & 15);

    p++;
  }

  *pbuf = '\0';
  return buf;
}

char *
header_to_s(HTTP_HEADER *h)
{
  char *s;
  s = g_malloc(sizeof(char) * strlen(h->key) + strlen(h->value) + 2);
  s = strcpy(s, h->key);
  s = strcat(s, ":");
  s = strncat(s, h->value, strlen(h->value));

  return s;
}

static char *
hmac_sha1(const char *key, const char *message)
{
  unsigned int i;
  GChecksum *checksum;
  char *real_key;
  guchar ipad[SHA1_BLOCK_SIZE];
  guchar opad[SHA1_BLOCK_SIZE];
  guchar inner[SHA1_LENGTH];
  guchar digest[SHA1_LENGTH];
  gsize key_length, inner_length, digest_length;

  g_return_val_if_fail(key, NULL);
  g_return_val_if_fail(message, NULL);

  checksum = g_checksum_new(G_CHECKSUM_SHA1);

  /* If the key is longer than the block size, hash it first */
  if(strlen(key) > SHA1_BLOCK_SIZE) {
    guchar new_key[SHA1_LENGTH];

    key_length = sizeof(new_key);

    g_checksum_update(checksum, (guchar*)key, strlen(key));
    g_checksum_get_digest(checksum, new_key, &key_length);
    g_checksum_reset(checksum);

    real_key = g_memdup(new_key, key_length);
  } else {
    real_key = g_strdup(key);
    key_length = strlen(key);
  }

  /* Sanity check the length */
  g_assert(key_length <= SHA1_BLOCK_SIZE);

  /* Protect against use of the provided key by NULLing it */
  key = NULL;

  /* Stage 1 */
  memset(ipad, 0, sizeof(ipad));
  memset(opad, 0, sizeof(opad));

  memcpy(ipad, real_key, key_length);
  memcpy(opad, real_key, key_length);

  /* Stage 2 and 5 */
  for(i = 0; i < sizeof(ipad); i++) {
    ipad[i] ^= 0x36;
    opad[i] ^= 0x5C;
  }

  /* Stage 3 and 4 */
  g_checksum_update(checksum, ipad, sizeof(ipad));
  g_checksum_update(checksum, (guchar*) message, strlen(message));
  inner_length = sizeof(inner);
  g_checksum_get_digest(checksum, inner, &inner_length);
  g_checksum_reset(checksum);

  /* Stage 6 and 7 */
  g_checksum_update(checksum, opad, sizeof(opad));
  g_checksum_update(checksum, inner, inner_length);

  digest_length = sizeof(digest);
  g_checksum_get_digest(checksum, digest, &digest_length);

  g_checksum_free(checksum);
  g_free(real_key);

  return g_base64_encode(digest, digest_length);
}

static char *
rfc2822_timestamp()
{
  char s[40];
  char *date;

  time_t t = time(NULL);
  strftime(s, sizeof(s), "%a, %d %b %Y %T %z", gmtime(&t));

  date = strdup(s);

  return date;
}

static char *
get_resource(const char *path)
{
  int path_len;
  int bucket_len;
  char *resource;

  path_len   = strlen(path);
  bucket_len = strlen(stormfs_curl.bucket);
  char tmp[1 + path_len + bucket_len + 1];

  strcpy(tmp, "/");
  strncat(tmp, stormfs_curl.bucket, bucket_len);
  strncat(tmp, path, path_len);
  resource = strdup(tmp);

  return resource;
}

static int
http_response_errno(CURL *handle)
{
  long http_response;

  if(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_response) != 0)
    return -EIO;

  if(http_response == 401)
    return -EACCES;

  if(http_response == 403)
    return -EACCES;

  if(http_response == 404)
    return -ENOENT;

  if(http_response >= 400)
    return -EIO; 

  return 0;
}

GList *
strip_header(GList *headers, const char *key)
{
  GList *new = NULL;
  GList *head = NULL;
  GList *next = NULL;

  head = g_list_first(headers);
  while(head != NULL) {
    next = head->next;
    HTTP_HEADER *header = head->data;

    if(strstr(header->key, key) != NULL) {
      g_free(header->key);
      g_free(header->value);
      g_free(header);

      head = next;
      continue;
    }

    HTTP_HEADER *h;
    h = g_malloc(sizeof(HTTP_HEADER));
    h->key   = strdup(header->key);
    h->value = strdup(header->value);

    g_free(header->key);
    g_free(header->value);
    g_free(header);

    new = g_list_append(new, h);
    head = next;
  }

  g_list_free(headers);

  return new;
}

HTTP_HEADER *
get_copy_source(const char *path)
{
  HTTP_HEADER *h;
  h = g_malloc(sizeof(HTTP_HEADER));

  h->key = strdup("x-amz-copy-source");
  // TODO: IDUNNOWTF.
  //h->value = url_encode(get_resource(path));
  h->value = get_resource(path);

  return h;
}

HTTP_HEADER *
get_gid_header(gid_t gid)
{
  char *s = gid_to_s(gid);
  HTTP_HEADER *h = g_malloc(sizeof(HTTP_HEADER));

  h->key   = strdup("x-amz-meta-gid");
  h->value = s;

  return h;
}

HTTP_HEADER *
get_uid_header(uid_t uid)
{
  char *s = uid_to_s(uid);
  HTTP_HEADER *h = g_malloc(sizeof(HTTP_HEADER));

  h->key   = strdup("x-amz-meta-uid");
  h->value = s;

  return h;
}

HTTP_HEADER *
get_mode_header(mode_t mode)
{
  char *s = mode_to_s(mode);
  HTTP_HEADER *h = g_malloc(sizeof(HTTP_HEADER));

  h->key   = strdup("x-amz-meta-mode");
  h->value = s;

  return h;
}

HTTP_HEADER *
get_mtime_header(time_t t)
{
  HTTP_HEADER *h;
  char *s = time_to_s(t);
  h = g_malloc(sizeof(HTTP_HEADER));

  h->key = strdup("x-amz-meta-mtime");
  h->value = s;

  return h;
}

HTTP_HEADER *
get_replace_header()
{
  HTTP_HEADER *h;
  h = g_malloc(sizeof(HTTP_HEADER));

  h->key   = strdup("x-amz-metadata-directive");
  h->value = strdup("REPLACE");

  return h;
}

static int
sign_request(const char *method, 
    struct curl_slist **headers, const char *path)
{
  char *signature;
  GString *to_sign;
  GString *date_header;
  GString *authorization;
  struct curl_slist *next;
  struct curl_slist *header;
  char *date = rfc2822_timestamp();
  char *resource = get_resource(path);

  to_sign = g_string_new("");
  to_sign = g_string_append(to_sign, method);
  to_sign = g_string_append(to_sign, "\n\n\n");
  to_sign = g_string_append(to_sign, date);
  to_sign = g_string_append_c(to_sign, '\n');

  header = *headers;
  if(header != NULL) {
    do {
      next = header->next;
      if(strstr(header->data, "x-amz") != NULL) {
        to_sign = g_string_append(to_sign, header->data);
        to_sign = g_string_append_c(to_sign, '\n');
      }
      header = next;
    } while(next);
  }

  to_sign = g_string_append(to_sign, resource);

  signature = hmac_sha1(stormfs_curl.secret_key, to_sign->str);
  
  authorization = g_string_new("Authorization: AWS ");
  authorization = g_string_append(authorization, stormfs_curl.access_key);
  authorization = g_string_append(authorization, ":");
  authorization = g_string_append(authorization, signature);

  date_header = g_string_new("Date: ");
  date_header = g_string_append(date_header, date);
  *headers = curl_slist_append(*headers, date_header->str);
  *headers = curl_slist_append(*headers, authorization->str);

  g_free(date);
  g_free(resource);
  g_free(signature);
  g_string_free(to_sign, TRUE);
  g_string_free(date_header, TRUE);
  g_string_free(authorization, TRUE);

  return 0;
}

static int
set_curl_defaults(CURL **c)
{
  // curl_easy_setopt(*c, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(*c, CURLOPT_NOPROGRESS, 1L);
  curl_easy_setopt(*c, CURLOPT_USERAGENT, "stormfs");

  return 0;
}

static char *
get_url(const char *path)
{
  char *url;
  char tmp[strlen(stormfs_curl.url) + strlen(path) + 1];

  strcpy(tmp, stormfs_curl.url);
  strncat(tmp, path, strlen(path) + 1);
  url = strdup(tmp);

  return(url);
}

static size_t
write_memory_cb(void *ptr, size_t size, size_t nmemb, void *data)
{
  size_t realsize = size * nmemb;
  HTTP_RESPONSE *mem = data;

  mem->memory = g_realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL) {
    fprintf(stderr, "stormfs: memory allocation failed\n");
    abort();
  }

  memcpy(&(mem->memory[mem->size]), ptr, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

static int
extract_meta(char *headers, GList **meta)
{
  char *p;
  char *to_extract[8] = {
    "Content-Type",
    "Content-Length",
    "Last-Modified",
    "ETag",
    "x-amz-meta-gid",
    "x-amz-meta-uid",
    "x-amz-meta-mode",
    "x-amz-meta-mtime"
  };

  p = strtok(headers, "\n");
  while(p != NULL) {
    int i;

    for(i = 0; i < 8; i++) {
      HTTP_HEADER *h;
      char *key = to_extract[i];
      char *value;

      if(!strstr(p, key))
        continue;

      h = g_malloc(sizeof(HTTP_HEADER));
      h->key = strdup(key);
      value = strstr(p, " ");
      value++;                         // remove leading space
      value[strlen(value) - 1] = '\0'; // remove trailing '\r'
      h->value = strdup(value);

      *meta = g_list_append(*meta, h);
      break;
    }

    p = strtok(NULL, "\n");
  }

  return 0;
}

static CURL *
get_curl_handle(const char *url)
{
  CURL *c;
  c = curl_easy_init();
  set_curl_defaults(&c);
  curl_easy_setopt(c, CURLOPT_URL, url);

  return c;
}

static int
destroy_curl_handle(CURL *c)
{
  curl_easy_cleanup(c);

  return 0;
}

int
stormfs_curl_init(const char *bucket, const char *url)
{
  CURLcode result;
  stormfs_curl.url = url;
  stormfs_curl.bucket = bucket;

  if((result = curl_global_init(CURL_GLOBAL_ALL)) != CURLE_OK)
    return -1;

  return 0;
}

int
stormfs_curl_set_auth(const char *access_key, const char *secret_key)
{
  stormfs_curl.access_key = access_key;
  stormfs_curl.secret_key = secret_key;

  return 0;
}

int
stormfs_curl_get(const char *path, char **data)
{
  int result;
  char *url = get_url(path);
  CURL *c = get_curl_handle(url);
  struct curl_slist *req_headers = NULL; 
  HTTP_RESPONSE body;

  body.memory = g_malloc(1);
  body.size = 0;

  sign_request("GET", &req_headers, path);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *) &body);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_memory_cb);

  curl_easy_perform(c);
  result = http_response_errno(c);

  *data = strdup(body.memory);

  if(body.memory)
    g_free(body.memory);

  g_free(url);
  destroy_curl_handle(c);
  curl_slist_free_all(req_headers);

  return result;
}

int
stormfs_curl_get_file(const char *path, FILE *f)
{
  int result;
  char *url = get_url(path);
  CURL *c = get_curl_handle(url);
  struct curl_slist *req_headers = NULL; 

  sign_request("GET", &req_headers, path);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);

  curl_easy_perform(c);
  result = http_response_errno(c);
  rewind(f);

  g_free(url);
  destroy_curl_handle(c);
  curl_slist_free_all(req_headers);

  return result;
}

int
stormfs_curl_head(const char *path, GList **meta)
{
  int status;
  char *url = get_url(path);
  char *response_headers;
  CURL *c = get_curl_handle(url);
  struct curl_slist *req_headers = NULL;
  HTTP_RESPONSE data;

  data.memory = g_malloc(1);
  data.size = 0;

  sign_request("HEAD", &req_headers, path);
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);    // HEAD
  curl_easy_setopt(c, CURLOPT_FILETIME, 1L);  // Last-Modified
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, (void *) &data);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_memory_cb);

  curl_easy_perform(c);
  status = http_response_errno(c);

  response_headers = strdup(data.memory);
  extract_meta(response_headers, &(*meta));

  if(data.memory)
    g_free(data.memory);

  g_free(url);
  g_free(response_headers);
  destroy_curl_handle(c);
  curl_slist_free_all(req_headers);

  return status;
}

int
stormfs_curl_upload(const char *path, GList *headers, int fd)
{
  FILE *f;
  int status;
  char *url;
  CURL *c;
  GList *head = NULL, *next = NULL;
  struct curl_slist *req_headers = NULL;
  struct stat st;

  if(fstat(fd, &st) != 0)
    return -errno;

  // TODO: support multipart uploads (>5GB files)
  if(st.st_size > 5368709120LL)
    return -EFBIG;
  
  if((f = fdopen(fd, "rb")) == NULL)
    return -errno;

  url = get_url(path);
  c = get_curl_handle(url);
  headers = g_list_sort(headers, (GCompareFunc) cmpstringp);

  head = g_list_first(headers);
  while(head != NULL) {
    next = head->next;

    HTTP_HEADER *header = head->data;
    if(strstr(header->key, "x-amz-") != NULL)
      req_headers = curl_slist_append(req_headers, header_to_s(header));

    head = next;
  }

  sign_request("PUT", &req_headers, path);
  curl_easy_setopt(c, CURLOPT_INFILE, f);
  curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t) st.st_size); 
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);

  curl_easy_perform(c);
  status = http_response_errno(c);

  g_free(url);
  destroy_curl_handle(c);
  curl_slist_free_all(req_headers);

  return status;
}

int
stormfs_curl_create(const char *path, uid_t uid, gid_t gid, mode_t mode, time_t mtime)
{
  int status;
  char *url = get_url(path);
  CURL *c = get_curl_handle(url);
  struct curl_slist *req_headers = NULL;
  GList *headers = NULL, *head = NULL, *next = NULL;

  headers = g_list_append(headers, get_gid_header(gid));
  headers = g_list_append(headers, get_uid_header(uid));
  headers = g_list_append(headers, get_mode_header(mode));
  headers = g_list_append(headers, get_mtime_header(mtime));
  headers = g_list_sort(headers, (GCompareFunc) cmpstringp);

  head = g_list_first(headers);
  while(head != NULL) {
    next = head->next;
    HTTP_HEADER *header = head->data;

    req_headers = curl_slist_append(req_headers, header_to_s(header));

    g_free(header->key);
    g_free(header->value);
    g_free(header);

    head = next;
  }

  sign_request("PUT", &req_headers, path);
  curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);    // HTTP PUT
  curl_easy_setopt(c, CURLOPT_INFILESIZE, 0); // Content-Length: 0
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);

  curl_easy_perform(c);
  status = http_response_errno(c);

  g_free(url);
  destroy_curl_handle(c);
  curl_slist_free_all(req_headers);

  return status;
}

int
stormfs_curl_utimens(const char *path, time_t t)
{
  int status;
  char *url = get_url(path);
  CURL *c = get_curl_handle(url);
  struct curl_slist *req_headers = NULL;
  GList *meta = NULL, *head = NULL, *next = NULL;
  HTTP_RESPONSE body;

  body.memory = g_malloc(1);
  body.size = 0;

  // get metadata from original object
  if((status = stormfs_curl_head(path, &meta)) != 0) {
    g_free(url);
    destroy_curl_handle(c);

    return status;
  }

  meta = strip_header(meta, "x-amz-meta-mtime");
  meta = g_list_append(meta, get_copy_source(path));
  meta = g_list_append(meta, get_mtime_header(t));
  meta = g_list_sort(meta, (GCompareFunc) cmpstringp);
  meta = g_list_append(meta, get_replace_header());

  head = g_list_first(meta);
  while(head != NULL) {
    next = head->next;
    HTTP_HEADER *header = head->data;

    if(strstr(header->key, "x-amz-") != NULL)
      req_headers = curl_slist_append(req_headers, header_to_s(header));

    g_free(header->key);
    g_free(header->value);
    g_free(header);

    head = next;
  }

  sign_request("PUT", &req_headers, path);
  curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);    // HTTP PUT
  curl_easy_setopt(c, CURLOPT_INFILESIZE, 0); // Content-Length: 0
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, req_headers);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *) &body);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_memory_cb);

  curl_easy_perform(c);
  status = http_response_errno(c);

  if(body.memory)
    g_free(body.memory);

  g_free(url);
  destroy_curl_handle(c);
  curl_slist_free_all(req_headers);

  return status;
}

void
stormfs_curl_destroy()
{
  curl_global_cleanup();
}
