/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "task.h"

/* FIXME we shouldn't need to branch in this file */
#if defined(__unix__) || defined(__POSIX__) || \
	defined(__APPLE__) || defined(_AIX)
#include <unistd.h> /* unlink, rmdir, memset */
#else
# include <direct.h>
# include <io.h>
# define unlink _unlink
# define rmdir _rmdir
# define open _open
# define write _write
# define close _close
# ifndef stat
#  define stat _stati64
# endif
# ifndef lseek
#   define lseek _lseek
# endif
#endif

#define TOO_LONG_NAME_LENGTH 65536
#define PATHMAX 1024

#if defined(__sun) || defined(__linux__)
#include <string.h> /* memset */
#endif

#include <fcntl.h>
#include <sys/stat.h>

uv_fs_t readdir_req;
uv_dir_t dir_handle_sync;
uv_dir_t dir_handle_async;
uv_dirent_t dent;

static int empty_opendir_cb_count;
static int empty_readdir_cb_count;

static int empty_closedir_cb_count_sync;
static int empty_closedir_cb_count_async;

static void empty_closedir_cb_async(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_async);
  ++empty_closedir_cb_count_async;
}

static void empty_closedir_cb_sync(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_sync);
  ++empty_closedir_cb_count_sync;
}

static void empty_readdir_cb(uv_fs_t* req) {
  ASSERT(req == &readdir_req);
  ASSERT(req->fs_type == UV_FS_READDIR);

  // TODO jgilli: by default, uv_fs_readdir doesn't return UV_EOF when reading
  // an emptry dir. Instead, it returns "." and ".." entries in sequence.
  // Should this be fixed to mimic uv_fs_scandir's behavior?
  if (req->result == UV_EOF) {
    ASSERT(empty_readdir_cb_count == 2);
    uv_close((uv_handle_t*)req->dir_handle, empty_closedir_cb_async);
  } else {
    ASSERT(req->result == 1);
    ASSERT(req->ptr == &dent);
    ASSERT(req->dir_handle == &dir_handle_async);

#ifdef HAVE_DIRENT_TYPES
    // In an empty directory, all entries are directories ("." and "..")
    ASSERT(((uv_dirent_t*)req->ptr)->type == UV_DIRENT_DIR);
#else
    ASSERT(((uv_dirent_t*)req->ptr)->type == UV_DIRENT_UNKNOWN);
#endif /* HAVE_DIRENT_TYPES */

    ++empty_readdir_cb_count;

    uv_fs_readdir(uv_default_loop(),
                  req,
                  req->dir_handle,
                  &dent,
                  empty_readdir_cb);
  }

  uv_fs_req_cleanup(req);
}

static void empty_opendir_cb(uv_fs_t* req) {
  ASSERT(req == &readdir_req);
  ASSERT(req->fs_type == UV_FS_OPENDIR);
  ASSERT(req->result == 0);
  ASSERT(req->ptr == &dir_handle_async);
  ASSERT(req->dir_handle == &dir_handle_async);
  ASSERT(0 == uv_fs_readdir(uv_default_loop(),
                            req,
                            req->ptr,
                            &dent,
                            empty_readdir_cb));
  uv_fs_req_cleanup(req);
  ++empty_opendir_cb_count;
}

/*
 * This test makes sure that both synchronous and asynchronous flavors
 * of the uv_fs_opendir -> uv_fs_readdir -> uv_close sequence work
 * as expected when processing an empty directory.
 */
TEST_IMPL(fs_readdir_empty_dir) {
  uv_loop_t* loop;
  const char* path;
  uv_fs_t mkdir_req;
  int r;
  size_t entries_count;

  path = "./empty_dir/";
  loop = uv_default_loop();

  uv_fs_mkdir(loop, &mkdir_req, path, 0777, NULL);
  uv_fs_req_cleanup(&mkdir_req);

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  /* Testing the synchronous flavor */
  r = uv_fs_opendir(loop,
                    &readdir_req,
                    &dir_handle_sync,
                    path,
                    UV_DIR_FLAGS_NONE,
                    NULL);
  ASSERT(r == 0);
  ASSERT(readdir_req.fs_type == UV_FS_OPENDIR);
  ASSERT(readdir_req.result == 0);
  ASSERT(readdir_req.ptr == &dir_handle_sync);
  ASSERT(readdir_req.dir_handle == &dir_handle_sync);
  /*
   * TODO jgilli: by default, uv_fs_readdir doesn't return UV_EOF when reading
   * an emptry dir. Instead, it returns "." and ".." entries in sequence.
   * Should this be fixed to mimic uv_fs_scandir's behavior?
   */
  entries_count = 0;
  while (UV_EOF != uv_fs_readdir(loop,
                                 &readdir_req,
                                 readdir_req.dir_handle,
                                 &dent,
                                 NULL)) {
#ifdef HAVE_DIRENT_TYPES
    // In an empty directory, all entries are directories ("." and "..")
    ASSERT(((uv_dirent_t*)readdir_req.ptr)->type == UV_DIRENT_DIR);
#else
    ASSERT(((uv_dirent_t*)readdir_req.ptr)->type == UV_DIRENT_UNKNOWN);
#endif /* HAVE_DIRENT_TYPES */

    ++entries_count;
  }

  ASSERT(entries_count == 2);

  uv_fs_req_cleanup(&readdir_req);

  ASSERT(empty_closedir_cb_count_sync == 0);

  uv_close((uv_handle_t*)&dir_handle_sync, empty_closedir_cb_sync);
  uv_run(loop, UV_RUN_ONCE);

  ASSERT(empty_closedir_cb_count_sync == 1);

  /* Testing the asynchronous flavor */

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  r = uv_fs_opendir(loop,
    &readdir_req,
    &dir_handle_async,
    path,
    UV_DIR_FLAGS_NONE,
    empty_opendir_cb);
  ASSERT(r == 0);

  ASSERT(empty_opendir_cb_count == 0);
  ASSERT(empty_closedir_cb_count_async == 0);

  uv_run(loop, UV_RUN_DEFAULT);

  ASSERT(empty_opendir_cb_count == 1);
  ASSERT(empty_closedir_cb_count_async == 1);

  uv_fs_rmdir(loop, &readdir_req, path, NULL);
  uv_fs_req_cleanup(&readdir_req);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

/*
 * This test makes sure that reading a non-existing directory with
 * uv_fs_{open,read}_dir returns proper error codes.
 */

static int non_existing_opendir_cb_count;
static int non_existing_dir_close_cb_count_async;
static int non_existing_dir_close_cb_count_sync;

static void non_existing_dir_close_cb_sync(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_sync);
  ++non_existing_dir_close_cb_count_sync;
}

static void non_existing_dir_close_cb_async(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_async);
  ++non_existing_dir_close_cb_count_async;
}

static void non_existing_opendir_cb(uv_fs_t* req) {
  ASSERT(req == &readdir_req);
  ASSERT(req->fs_type == UV_FS_OPENDIR);
  ASSERT(req->result == UV_ENOENT);
  ASSERT(req->ptr == NULL);
  ASSERT(req->dir_handle == &dir_handle_async);
  uv_fs_req_cleanup(req);
  uv_close((uv_handle_t*)req->dir_handle, non_existing_dir_close_cb_async);
  ++non_existing_opendir_cb_count;
}

TEST_IMPL(fs_readdir_non_existing_dir) {
  uv_loop_t* loop;
  const char* path;
  int r;

  path = "./non-existing-dir/";
  loop = uv_default_loop();

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  /* Testing the synchronous flavor */
  r = uv_fs_opendir(loop,
    &readdir_req,
    &dir_handle_sync,
    path,
    UV_DIR_FLAGS_NONE,
    NULL);
  ASSERT(r == UV_ENOENT);
  ASSERT(readdir_req.fs_type == UV_FS_OPENDIR);
  ASSERT(readdir_req.result == UV_ENOENT);
  ASSERT(readdir_req.ptr == NULL);
  ASSERT(readdir_req.dir_handle == &dir_handle_sync);

  uv_fs_req_cleanup(&readdir_req);

  ASSERT(non_existing_dir_close_cb_count_sync == 0);

  uv_close((uv_handle_t*)&dir_handle_sync, non_existing_dir_close_cb_sync);
  uv_run(loop, UV_RUN_ONCE);

  ASSERT(non_existing_dir_close_cb_count_sync == 1);

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  /* Testing the async flavor */
  r = uv_fs_opendir(loop,
    &readdir_req,
    &dir_handle_async,
    path,
    UV_DIR_FLAGS_NONE,
    non_existing_opendir_cb);
  ASSERT(r == 0);

  ASSERT(non_existing_opendir_cb_count == 0);
  ASSERT(non_existing_dir_close_cb_count_async == 0);

  uv_run(loop, UV_RUN_DEFAULT);

  ASSERT(non_existing_opendir_cb_count == 1);
  ASSERT(non_existing_dir_close_cb_count_async == 1);

  uv_fs_req_cleanup(&readdir_req);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

/*
 * This test makes sure that reading a file as a directory reports correct
 * error codes.
 */

static int file_opendir_cb_count;
static int file_opendir_close_handle_cb_count_async;
static int file_opendir_close_handle_cb_count_sync;

static void file_opendir_close_handle_cb_sync(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_sync);
  ++file_opendir_close_handle_cb_count_sync;
}

static void file_opendir_close_handle_cb_async(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_async);
  ++file_opendir_close_handle_cb_count_async;
}

static void file_opendir_cb(uv_fs_t* req) {
  ASSERT(req == &readdir_req);
  ASSERT(req->fs_type == UV_FS_OPENDIR);
  ASSERT(req->result == UV_ENOTDIR);
  ASSERT(req->ptr == NULL);
  ASSERT(req->dir_handle == &dir_handle_async);

  uv_fs_req_cleanup(req);
  uv_close((uv_handle_t*)req->dir_handle, file_opendir_close_handle_cb_async);
  ++file_opendir_cb_count;
}

TEST_IMPL(fs_readdir_file) {
  uv_loop_t* loop;
  const char* path;
  int r;

  path = "test/fixtures/empty_file";
  loop = uv_default_loop();

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  /* Testing the synchronous flavor */
  r = uv_fs_opendir(loop,
    &readdir_req,
    &dir_handle_sync,
    path,
    UV_DIR_FLAGS_NONE,
    NULL);

  ASSERT(r == UV_ENOTDIR);
  ASSERT(readdir_req.fs_type == UV_FS_OPENDIR);
  ASSERT(readdir_req.result == UV_ENOTDIR);
  ASSERT(readdir_req.ptr == NULL);
  ASSERT(readdir_req.dir_handle == &dir_handle_sync);

  uv_fs_req_cleanup(&readdir_req);

  ASSERT(file_opendir_close_handle_cb_count_sync == 0);

  uv_close((uv_handle_t*)&dir_handle_sync, file_opendir_close_handle_cb_sync);
  uv_run(loop, UV_RUN_ONCE);

  ASSERT(file_opendir_close_handle_cb_count_sync == 1);

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  /* Testing the async flavor */
  r = uv_fs_opendir(loop,
    &readdir_req,
    &dir_handle_async,
    path,
    UV_DIR_FLAGS_NONE,
    file_opendir_cb);
  ASSERT(r == 0);

  ASSERT(file_opendir_cb_count == 0);
  ASSERT(file_opendir_close_handle_cb_count_async == 0);

  uv_run(loop, UV_RUN_DEFAULT);

  ASSERT(file_opendir_cb_count == 1);
  ASSERT(file_opendir_close_handle_cb_count_async == 1);

  uv_fs_req_cleanup(&readdir_req);

  MAKE_VALGRIND_HAPPY();
  return 0;
}

/*
 * This test makes sure that reading a non-empty directory with
 * uv_fs_{open,read}_dir returns proper directory entries, including the
 * correct entry types.
 */

static int non_empty_opendir_cb_count;
static int non_empty_readdir_cb_count;

static int non_empty_closedir_cb_count_sync;
static int non_empty_closedir_cb_count_async;

static void non_empty_closedir_cb_async(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_async);
  ++non_empty_closedir_cb_count_async;
}

static void non_empty_closedir_cb_sync(uv_handle_t* handle) {
  ASSERT(handle == (uv_handle_t*)&dir_handle_sync);
  ++non_empty_closedir_cb_count_sync;
}

static void non_empty_readdir_cb(uv_fs_t* req) {
  ASSERT(req == &readdir_req);
  ASSERT(req->fs_type == UV_FS_READDIR);

  // TODO jgilli: by default, uv_fs_readdir doesn't return UV_EOF when reading
  // an emptry dir. Instead, it returns "." and ".." entries in sequence.
  // Should this be fixed to mimic uv_fs_scandir's behavior?
  if (req->result == UV_EOF) {
    ASSERT(non_empty_readdir_cb_count == 5);
    uv_close((uv_handle_t*)req->dir_handle, non_empty_closedir_cb_async);
  } else {
    ASSERT(req->result == 1);
    ASSERT(req->ptr == &dent);
    ASSERT(req->dir_handle == &dir_handle_async);

#ifdef HAVE_DIRENT_TYPES
    if (!strcmp(dent.name, "test_subdir") ||
        !strcmp(dent.name, ".") ||
        !strcmp(dent.name, "..")) {
      ASSERT(dent.type == UV_DIRENT_DIR);
    } else {
      ASSERT(dent.type == UV_DIRENT_FILE);
    }
#else
    ASSERT(dent.type == UV_DIRENT_UNKNOWN);
#endif /* HAVE_DIRENT_TYPES */

    ++non_empty_readdir_cb_count;

    uv_fs_readdir(uv_default_loop(),
                  req,
                  req->dir_handle,
                  &dent,
                  non_empty_readdir_cb);
  }

  uv_fs_req_cleanup(req);
}

static void non_empty_opendir_cb(uv_fs_t* req) {
  ASSERT(req == &readdir_req);
  ASSERT(req->fs_type == UV_FS_OPENDIR);
  ASSERT(req->result == 0);
  ASSERT(req->ptr == &dir_handle_async);
  ASSERT(req->dir_handle == &dir_handle_async);
  ASSERT(0 == uv_fs_readdir(uv_default_loop(),
                            req,
                            req->ptr,
                            &dent,
                            non_empty_readdir_cb));
  uv_fs_req_cleanup(req);
  ++non_empty_opendir_cb_count;
}

TEST_IMPL(fs_readdir_non_empty_dir) {
  uv_loop_t* loop;
  int r;
  size_t entries_count;
  uv_fs_t mkdir_req;
  uv_fs_t open_req1;
  uv_fs_t close_req;
  uv_fs_t rmdir_req;

  loop = uv_default_loop();

  /* Setup */
  unlink("test_dir/file1");
  unlink("test_dir/file2");
  rmdir("test_dir/test_subdir");
  rmdir("test_dir");

  loop = uv_default_loop();

  r = uv_fs_mkdir(loop, &mkdir_req, "test_dir", 0755, NULL);
  ASSERT(r == 0);

  /* Create 2 files synchronously. */
  r = uv_fs_open(loop, &open_req1, "test_dir/file1", O_WRONLY | O_CREAT,
      S_IWUSR | S_IRUSR, NULL);
  ASSERT(r >= 0);
  uv_fs_req_cleanup(&open_req1);
  r = uv_fs_close(loop, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&close_req);

  r = uv_fs_open(loop, &open_req1, "test_dir/file2", O_WRONLY | O_CREAT,
      S_IWUSR | S_IRUSR, NULL);
  ASSERT(r >= 0);
  uv_fs_req_cleanup(&open_req1);
  r = uv_fs_close(loop, &close_req, open_req1.result, NULL);
  ASSERT(r == 0);
  uv_fs_req_cleanup(&close_req);

    r = uv_fs_mkdir(loop, &mkdir_req, "test_dir/test_subdir", 0755, NULL);
  ASSERT(r == 0);

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  /* Testing the synchronous flavor */
  r = uv_fs_opendir(loop,
                    &readdir_req,
                    &dir_handle_sync,
                    "test_dir",
                    UV_DIR_FLAGS_NONE,
                    NULL);
  ASSERT(r == 0);
  ASSERT(readdir_req.fs_type == UV_FS_OPENDIR);
  ASSERT(readdir_req.result == 0);
  ASSERT(readdir_req.ptr == &dir_handle_sync);
  ASSERT(readdir_req.dir_handle == &dir_handle_sync);
  // TODO jgilli: by default, uv_fs_readdir doesn't return UV_EOF when reading
  // an emptry dir. Instead, it returns "." and ".." entries in sequence.
  // Should this be fixed to mimic uv_fs_scandir's behavior?
  entries_count = 0;
  while (uv_fs_readdir(loop,
                       &readdir_req,
                       readdir_req.dir_handle,
                       &dent,
                       NULL) != UV_EOF) {
#ifdef HAVE_DIRENT_TYPES
    if (!strcmp(dent.name, "test_subdir") ||
        !strcmp(dent.name, ".") ||
        !strcmp(dent.name, "..")) {
      ASSERT(dent.type == UV_DIRENT_DIR);
    } else {
      ASSERT(dent.type == UV_DIRENT_FILE);
    }
#else
    ASSERT(dent.type == UV_DIRENT_UNKNOWN);
#endif /* HAVE_DIRENT_TYPES */
    ++entries_count;
  }

  ASSERT(entries_count == 5);
  uv_fs_req_cleanup(&readdir_req);

  ASSERT(non_empty_closedir_cb_count_sync == 0);

  uv_close((uv_handle_t*)&dir_handle_sync, non_empty_closedir_cb_sync);
  uv_run(loop, UV_RUN_ONCE);

  ASSERT(non_empty_closedir_cb_count_sync == 1);

  /* Testing the asynchronous flavor */

  /* Fill the req to ensure that required fields are cleaned up */
  memset(&readdir_req, 0xdb, sizeof(readdir_req));

  r = uv_fs_opendir(loop,
    &readdir_req,
    &dir_handle_async,
    "test_dir",
    UV_DIR_FLAGS_NONE,
    non_empty_opendir_cb);
  ASSERT(r == 0);

  ASSERT(non_empty_opendir_cb_count == 0);
  ASSERT(non_empty_closedir_cb_count_async == 0);

  uv_run(loop, UV_RUN_DEFAULT);

  ASSERT(non_empty_opendir_cb_count == 1);
  ASSERT(non_empty_closedir_cb_count_async == 1);

  uv_fs_rmdir(loop, &rmdir_req, "test_subdir", NULL);
  uv_fs_req_cleanup(&rmdir_req);

  /* Cleanup */
  unlink("test_dir/file1");
  unlink("test_dir/file2");
  rmdir("test_dir/test_subdir");
  rmdir("test_dir");

  MAKE_VALGRIND_HAPPY();
  return 0;
 }
