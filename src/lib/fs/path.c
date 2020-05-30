/* Copyright (c) 2003, Roger Dingledine
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2020, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file path.c
 *
 * \brief Manipulate strings that contain filesystem paths.
 **/

#include "lib/fs/path.h"
#include "lib/malloc/malloc.h"
#include "lib/log/log.h"
#include "lib/log/util_bug.h"
#include "lib/container/smartlist.h"
#include "lib/sandbox/sandbox.h"
#include "lib/string/printf.h"
#include "lib/string/util_string.h"
#include "lib/string/compat_ctype.h"
#include "lib/fs/files.h"
#include "lib/fs/dir.h"
#include "lib/fs/userdb.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#else /* !(defined(_WIN32)) */
#include <dirent.h>
#include <glob.h>
#endif /* defined(_WIN32) */

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#define IS_GLOB_CHAR(s,i) (((s)[(i)]) == '*' || ((s)[(i)]) == '?')
#else
#define IS_GLOB_CHAR(s,i) ((((s)[(i)]) == '*' || ((s)[(i)]) == '?') &&\
                           ((i) == 0 || (s)[(i)-1] != '\\')) /* check escape */
#endif

/** Removes enclosing quotes from <b>path</b> and unescapes quotes between the
 * enclosing quotes. Backslashes are not unescaped. Return the unquoted
 * <b>path</b> on success or 0 if <b>path</b> is not quoted correctly. */
char *
get_unquoted_path(const char *path)
{
  size_t len = strlen(path);

  if (len == 0) {
    return tor_strdup("");
  }

  int has_start_quote = (path[0] == '\"');
  int has_end_quote = (len > 0 && path[len-1] == '\"');
  if (has_start_quote != has_end_quote || (len == 1 && has_start_quote)) {
    return NULL;
  }

  char *unquoted_path = tor_malloc(len - has_start_quote - has_end_quote + 1);
  char *s = unquoted_path;
  size_t i;
  for (i = has_start_quote; i < len - has_end_quote; i++) {
    if (path[i] == '\"' && (i > 0 && path[i-1] == '\\')) {
      *(s-1) = path[i];
    } else if (path[i] != '\"') {
      *s++ = path[i];
    } else {  /* unescaped quote */
      tor_free(unquoted_path);
      return NULL;
    }
  }
  *s = '\0';
  return unquoted_path;
}

/** Expand any homedir prefix on <b>filename</b>; return a newly allocated
 * string. */
char *
expand_filename(const char *filename)
{
  tor_assert(filename);
#ifdef _WIN32
  /* Might consider using GetFullPathName() as described here:
   * http://etutorials.org/Programming/secure+programming/
   *     Chapter+3.+Input+Validation/3.7+Validating+Filenames+and+Paths/
   */
  return tor_strdup(filename);
#else /* !defined(_WIN32) */
  if (*filename == '~') {
    char *home, *result=NULL;
    const char *rest;

    if (filename[1] == '/' || filename[1] == '\0') {
      home = getenv("HOME");
      if (!home) {
        log_warn(LD_CONFIG, "Couldn't find $HOME environment variable while "
                 "expanding \"%s\"; defaulting to \"\".", filename);
        home = tor_strdup("");
      } else {
        home = tor_strdup(home);
      }
      rest = strlen(filename)>=2?(filename+2):"";
    } else {
#ifdef HAVE_PWD_H
      char *username, *slash;
      slash = strchr(filename, '/');
      if (slash)
        username = tor_strndup(filename+1,slash-filename-1);
      else
        username = tor_strdup(filename+1);
      if (!(home = get_user_homedir(username))) {
        log_warn(LD_CONFIG,"Couldn't get homedir for \"%s\"",username);
        tor_free(username);
        return NULL;
      }
      tor_free(username);
      rest = slash ? (slash+1) : "";
#else /* !defined(HAVE_PWD_H) */
      log_warn(LD_CONFIG, "Couldn't expand homedir on system without pwd.h");
      return tor_strdup(filename);
#endif /* defined(HAVE_PWD_H) */
    }
    tor_assert(home);
    /* Remove trailing slash. */
    if (strlen(home)>1 && !strcmpend(home,PATH_SEPARATOR)) {
      home[strlen(home)-1] = '\0';
    }
    tor_asprintf(&result,"%s"PATH_SEPARATOR"%s",home,rest);
    tor_free(home);
    return result;
  } else {
    return tor_strdup(filename);
  }
#endif /* defined(_WIN32) */
}

/** Return true iff <b>filename</b> is a relative path. */
int
path_is_relative(const char *filename)
{
  if (filename && filename[0] == '/')
    return 0;
#ifdef _WIN32
  else if (filename && filename[0] == '\\')
    return 0;
  else if (filename && strlen(filename)>3 && TOR_ISALPHA(filename[0]) &&
           filename[1] == ':' && filename[2] == '\\')
    return 0;
#endif /* defined(_WIN32) */
  else
    return 1;
}

/** Clean up <b>name</b> so that we can use it in a call to "stat".  On Unix,
 * we do nothing.  On Windows, we remove a trailing slash, unless the path is
 * the root of a disk. */
void
clean_fname_for_stat(char *name)
{
#ifdef _WIN32
  size_t len = strlen(name);
  if (!len)
    return;
  if (name[len-1]=='\\' || name[len-1]=='/') {
    if (len == 1 || (len==3 && name[1]==':'))
      return;
    name[len-1]='\0';
  }
#else /* !defined(_WIN32) */
  (void)name;
#endif /* defined(_WIN32) */
}

/** Modify <b>fname</b> to contain the name of its parent directory.  Doesn't
 * actually examine the filesystem; does a purely syntactic modification.
 *
 * The parent of the root director is considered to be iteself.
 *
 * Path separators are the forward slash (/) everywhere and additionally
 * the backslash (\) on Win32.
 *
 * Cuts off any number of trailing path separators but otherwise ignores
 * them for purposes of finding the parent directory.
 *
 * Returns 0 if a parent directory was successfully found, -1 otherwise (fname
 * did not have any path separators or only had them at the end).
 * */
int
get_parent_directory(char *fname)
{
  char *cp;
  int at_end = 1;
  tor_assert(fname);
#ifdef _WIN32
  /* If we start with, say, c:, then don't consider that the start of the path
   */
  if (fname[0] && fname[1] == ':') {
    fname += 2;
  }
#endif /* defined(_WIN32) */
  /* Now we want to remove all path-separators at the end of the string,
   * and to remove the end of the string starting with the path separator
   * before the last non-path-separator.  In perl, this would be
   *   s#[/]*$##; s#/[^/]*$##;
   * on a unixy platform.
   */
  cp = fname + strlen(fname);
  at_end = 1;
  while (--cp >= fname) {
    int is_sep = (*cp == '/'
#ifdef _WIN32
                  || *cp == '\\'
#endif
                  );
    if (is_sep) {
      if (cp == fname) {
        /* This is the first separator in the file name; don't remove it! */
        cp[1] = '\0';
        return 0;
      }
      *cp = '\0';
      if (! at_end)
        return 0;
    } else {
      at_end = 0;
    }
  }
  return -1;
}

#ifndef _WIN32
/** Return a newly allocated string containing the output of getcwd(). Return
 * NULL on failure. (We can't just use getcwd() into a PATH_MAX buffer, since
 * Hurd hasn't got a PATH_MAX.)
 */
static char *
alloc_getcwd(void)
{
#ifdef HAVE_GET_CURRENT_DIR_NAME
  /* Glibc makes this nice and simple for us. */
  char *cwd = get_current_dir_name();
  char *result = NULL;
  if (cwd) {
    /* We make a copy here, in case tor_malloc() is not malloc(). */
    result = tor_strdup(cwd);
    raw_free(cwd); // alias for free to avoid tripping check-spaces.
  }
  return result;
#else /* !defined(HAVE_GET_CURRENT_DIR_NAME) */
  size_t size = 1024;
  char *buf = NULL;
  char *ptr = NULL;

  while (ptr == NULL) {
    buf = tor_realloc(buf, size);
    ptr = getcwd(buf, size);

    if (ptr == NULL && errno != ERANGE) {
      tor_free(buf);
      return NULL;
    }

    size *= 2;
  }
  return buf;
#endif /* defined(HAVE_GET_CURRENT_DIR_NAME) */
}
#endif /* !defined(_WIN32) */

/** Expand possibly relative path <b>fname</b> to an absolute path.
 * Return a newly allocated string, which may be a duplicate of <b>fname</b>.
 */
char *
make_path_absolute(const char *fname)
{
#ifdef _WIN32
  char *absfname_malloced = _fullpath(NULL, fname, 1);

  /* We don't want to assume that tor_free can free a string allocated
   * with malloc.  On failure, return fname (it's better than nothing). */
  char *absfname = tor_strdup(absfname_malloced ? absfname_malloced : fname);
  if (absfname_malloced) raw_free(absfname_malloced);

  return absfname;
#else /* !defined(_WIN32) */
  char *absfname = NULL, *path = NULL;

  tor_assert(fname);

  if (fname[0] == '/') {
    absfname = tor_strdup(fname);
  } else {
    path = alloc_getcwd();
    if (path) {
      tor_asprintf(&absfname, "%s/%s", path, fname);
      tor_free(path);
    } else {
      /* LCOV_EXCL_START Can't make getcwd fail. */
      /* If getcwd failed, the best we can do here is keep using the
       * relative path.  (Perhaps / isn't readable by this UID/GID.) */
      log_warn(LD_GENERAL, "Unable to find current working directory: %s",
               strerror(errno));
      absfname = tor_strdup(fname);
      /* LCOV_EXCL_STOP */
    }
  }
  return absfname;
#endif /* defined(_WIN32) */
}

#ifndef _WIN32
/** Same as opendir but calls sandbox_intern_string before */
static DIR *
prot_opendir(const char *name)
{
  return opendir(sandbox_intern_string(name));
}

/** Same as stat but calls sandbox_intern_string before */
static int
prot_stat(const char *pathname, struct stat *buf)
{
  return stat(sandbox_intern_string(pathname), buf);
}

/** Same as lstat but calls sandbox_intern_string before */
static int
prot_lstat(const char *pathname, struct stat *buf)
{
  return lstat(sandbox_intern_string(pathname), buf);
}
#endif /* !(defined(_WIN32)) */

#ifdef _WIN32
static struct smartlist_t *
unglob_win32(const char *pattern, int prev_sep, int curr_sep)
{
  smartlist_t *results = smartlist_new();
  int len = prev_sep < 1 ? prev_sep + 1 : prev_sep; // handle /*
  char *path_until_glob = tor_strndup(pattern, len);

  if (!is_file(file_status(path_until_glob))) {
    smartlist_t *filenames = tor_listdir(path_until_glob);
    if (!filenames) {
      smartlist_free(results);
      results = NULL;
    } else {
      SMARTLIST_FOREACH_BEGIN(filenames, char *, filename) {
        char *full_path;
        tor_asprintf(&full_path, "%s"PATH_SEPARATOR"%s",
                     path_until_glob, filename);
        char *path_curr_glob = tor_strndup(pattern, curr_sep + 1);
        if (is_dir(file_status(full_path))) {
          clean_fname_for_stat(path_curr_glob);
        }
        if (PathMatchSpec(full_path, path_curr_glob)) {
          smartlist_add(results, full_path);
        } else {
          tor_free(full_path);
        }
        tor_free(path_curr_glob);
      } SMARTLIST_FOREACH_END(filename);
      SMARTLIST_FOREACH(filenames, char *, p, tor_free(p));
      smartlist_free(filenames);
    }
  }
  tor_free(path_until_glob);
  return results;
}
#endif  /* defined(_WIN32) */

static bool
add_non_glob_path(const char *path, struct smartlist_t *results)
{
  file_status_t file_type = file_status(path);
  if (file_type == FN_ERROR) {
    return false;
  } else if (file_type != FN_NOENT) {
    char *to_add = tor_strdup(path);
    clean_fname_for_stat(to_add);
    smartlist_add(results, to_add);
  }
  return true;
}

#ifdef _WIN32
static struct smartlist_t *
tor_glob_win32(const char *pattern)
{
  smartlist_t *results = smartlist_new();
  int i, prev_sep = -1, curr_sep = -1;
  bool is_glob = false, error_found = false, is_sep = false, is_last = false;

  // search for first path fragment with globs
  for (i = 0; pattern[i]; i++) {
    is_glob = is_glob || IS_GLOB_CHAR(pattern, i);
    is_last = !pattern[i+1];
    is_sep = pattern[i] == *PATH_SEPARATOR || pattern[i] == '/';
    if (is_sep || is_last) {
      prev_sep = curr_sep;
      curr_sep = i;
      if (is_glob) {
        break;
      }
    }
  }

  if (!is_glob) {
    if (!add_non_glob_path(pattern, results)) {
      SMARTLIST_FOREACH(results, char *, p, tor_free(p));
      smartlist_free(results);
      results = NULL;
    }
    return results;
  }

  smartlist_t *unglobbed_paths = unglob_win32(pattern, prev_sep, curr_sep);
  if (!unglobbed_paths) {
    error_found = true;
  } else {
    // for each path for current fragment, add the rest of the pattern
    // and call recursively to get all opened paths
    SMARTLIST_FOREACH_BEGIN(unglobbed_paths, char *, current_path) {
      char *next_path;
      tor_asprintf(&next_path, "%s"PATH_SEPARATOR"%s", current_path,
                   &pattern[curr_sep+1]);
      smartlist_t *opened_next = tor_glob_win32(next_path);
      tor_free(next_path);
      if (!opened_next) {
        error_found = true;
        break;
      }
      smartlist_add_all(results, opened_next);
      smartlist_free(opened_next);
    } SMARTLIST_FOREACH_END(current_path);
    SMARTLIST_FOREACH(unglobbed_paths, char *, p, tor_free(p));
    smartlist_free(unglobbed_paths);
  }

  if (error_found) {
    SMARTLIST_FOREACH(results, char *, p, tor_free(p));
    smartlist_free(results);
    results = NULL;
  }

  return results;
}
#endif /* defined(_WIN32) */

/** Return a new list containing the paths that match the pattern
 * <b>pattern</b>. Return NULL on error.
 */
struct smartlist_t *
tor_glob(const char *pattern)
{
  smartlist_t *result;
#ifdef _WIN32
  // PathMatchSpec does not support forward slashes, change them to backslashes
  char *pattern_normalized = tor_strdup(pattern);
  tor_strreplacechar(pattern_normalized, '/', *PATH_SEPARATOR);
  result = tor_glob_win32(pattern_normalized);
  tor_free(pattern_normalized);
#else /* !(defined(_WIN32)) */
  glob_t matches;
  int flags = GLOB_ERR | GLOB_NOSORT;
#ifdef GLOB_ALTDIRFUNC
  /* use functions that call sandbox_intern_string */
  flags |= GLOB_ALTDIRFUNC;
  typedef void *(*gl_opendir)(const char * name);
  typedef struct dirent *(*gl_readdir)(void *);
  typedef void (*gl_closedir)(void *);
  matches.gl_opendir = (gl_opendir) &prot_opendir;
  matches.gl_readdir = (gl_readdir) &readdir;
  matches.gl_closedir = (gl_closedir) &closedir;
  matches.gl_stat = &prot_stat;
  matches.gl_lstat = &prot_lstat;
#endif /* defined(GLOB_ALTDIRFUNC) */
  int ret = glob(pattern, flags, NULL, &matches);
  if (ret == GLOB_NOMATCH) {
    return smartlist_new();
  } else if (ret != 0) {
    return NULL;
  }

  result = smartlist_new();
  size_t i;
  for (i = 0; i < matches.gl_pathc; i++) {
    char *match = tor_strdup(matches.gl_pathv[i]);
    size_t len = strlen(match);
    if (len > 0 && match[len-1] == *PATH_SEPARATOR) {
      match[len-1] = '\0';
    }
    smartlist_add(result, match);
  }
  globfree(&matches);
#endif /* defined(_WIN32) */
  return result;
}

/** Returns true if <b>s</b> contains characters that can be globbed.
 * Returns false otherwise. */
bool
has_glob(const char *s)
{
  int i;
  for (i = 0; s[i]; i++) {
    if (IS_GLOB_CHAR(s, i)) {
      return true;
    }
  }
  return false;
}
