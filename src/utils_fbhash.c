/**
 * collectd - src/utils_fbhash.c
 * Copyright (C) 2009       Florian octo Forster
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 **/

#include "collectd.h"
#include "plugin.h"

#include <pthread.h>

#include "utils_fbhash.h"
#include "utils_avltree.h"

struct fbhash_s
{
  char *filename;
  time_t mtime;

  pthread_mutex_t lock;
  c_avl_tree_t *tree;
};

/*
 * Private functions
 */
static void fbh_free_tree (c_avl_tree_t *tree) /* {{{ */
{
  int status;

  if (tree == NULL)
    return;

  while (42)
  {
    char *key = NULL;
    char *value = NULL;

    status = c_avl_pick (tree, (void *) &key, (void *) &value);
    if (status != 0)
      break;

    free (key);
    free (value);
  }

  c_avl_destroy (tree);
} /* }}} void fbh_free_tree */

static int fbh_read_file (fbhash_t *h) /* {{{ */
{
  FILE *fh;
  char buffer[4096];
  struct flock fl;
  c_avl_tree_t *tree;
  int status;

  fh = fopen (h->filename, "r");
  if (fh == NULL)
    return (-1);

  memset (&fl, 0, sizeof (fl));
  fl.l_type = F_RDLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0; /* == entire file */
  /* TODO: Lock file? -> fcntl */

  status = fcntl (fileno (fh), F_SETLK, &fl);
  if (status != 0)
  {
    fclose (fh);
    return (-1);
  }

  tree = c_avl_create ((void *) strcmp);
  if (tree == NULL)
  {
    fclose (fh);
    return (-1);
  }

  /* Read `fh' into `tree' */
  while (fgets (buffer, sizeof (buffer), fh) != NULL) /* {{{ */
  {
    size_t len;
    char *key;
    char *value;

    char *key_copy;
    char *value_copy;

    buffer[sizeof (buffer) - 1] = 0;
    len = strlen (buffer);

    /* Remove trailing newline characters. */
    while ((len > 0)
        && ((buffer[len - 1] == '\n') || (buffer[len - 1] == '\r')))
    {
      len--;
      buffer[len] = 0;
    }

    /* Seek first non-space character */
    key = buffer;
    while ((*key != 0) && isspace ((int) *key))
      key++;

    /* Skip empty lines and comments */
    if ((key[0] == 0) || (key[0] == '#'))
      continue;

    /* Seek first colon */
    value = strchr (key, ':');
    if (value == NULL)
      continue;

    /* Null-terminate `key'. */
    *value = 0;
    value++;

    /* Skip leading whitespace */
    while ((*value != 0) && isspace ((int) *value))
      value++;

    /* Skip lines without value */
    if (value[0] == 0)
      continue;

    key_copy = strdup (key);
    value_copy = strdup (value);

    if ((key_copy == NULL) || (value_copy == NULL))
    {
      free (key_copy);
      free (value_copy);
      continue;
    }

    status = c_avl_insert (tree, key_copy, value_copy);
    if (status != 0)
    {
      free (key_copy);
      free (value_copy);
      continue;
    }

    DEBUG ("utils_fbhash: fbh_read_file: key = %s; value = %s;",
        key, value);
  } /* }}} while (fgets) */

  fclose (fh);

  fbh_free_tree (h->tree);
  h->tree = tree;

  return (0);
} /* }}} int fbh_read_file */

static int fbh_check_file (fbhash_t *h) /* {{{ */
{
  struct stat statbuf;
  int status;

  memset (&statbuf, 0, sizeof (statbuf));

  status = stat (h->filename, &statbuf);
  if (status != 0)
    return (-1);

  if (h->mtime >= statbuf.st_mtime)
    return (0);

  status = fbh_read_file (h);
  if (status == 0)
    h->mtime = statbuf.st_mtime;

  return (status);
} /* }}} int fbh_check_file */

/*
 * Public functions
 */
fbhash_t *fbh_create (const char *file) /* {{{ */
{
  fbhash_t *h;
  int status;

  if (file == NULL)
    return (NULL);

  h = malloc (sizeof (*h));
  if (h == NULL)
    return (NULL);
  memset (h, 0, sizeof (*h));

  h->filename = strdup (file);
  if (h->filename == NULL)
  {
    free (h);
    return (NULL);
  }

  h->mtime = 0;
  pthread_mutex_init (&h->lock, /* attr = */ NULL);

  status = fbh_check_file (h);
  if (status != 0)
  {
    fbh_destroy (h);
    free (h);
    return (NULL);
  }

  return (h);
} /* }}} fbhash_t *fbh_create */

void fbh_destroy (fbhash_t *h) /* {{{ */
{
  if (h == NULL)
    return;

  pthread_mutex_destroy (&h->lock);
  free (h->filename);
  fbh_free_tree (h->tree);
} /* }}} void fbh_destroy */

char *fbh_get (fbhash_t *h, const char *key) /* {{{ */
{
  char *value;
  char *value_copy;
  int status;

  if ((h == NULL) || (key == NULL))
    return (NULL);

  value = NULL;
  value_copy = NULL;

  pthread_mutex_lock (&h->lock);

  /* TODO: Checking this every time may be a bit much..? */
  fbh_check_file (h);

  status = c_avl_get (h->tree, key, (void *) &value);
  if (status == 0)
  {
    assert (value != NULL);
    value_copy = strdup (value);
  }

  pthread_mutex_unlock (&h->lock);

  return (value_copy);
} /* }}} char *fbh_get */

/* vim: set sw=2 sts=2 et fdm=marker : */
