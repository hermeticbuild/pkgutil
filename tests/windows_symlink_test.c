#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <direct.h>
#include <process.h>
#include <windows.h>
#define chdir _chdir
#define getpid _getpid
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#endif

static const char contents[] = "a header reached through framework symlinks\n";

static void fail(const char *operation, const char *path) {
  fprintf(stderr, "%s: %s (errno %d)\n", operation, path, errno);
  exit(1);
}

static void require_archive(struct archive *a, int status,
                            const char *operation) {
  if (status != ARCHIVE_OK) {
    fprintf(stderr, "%s: %s (status %d)\n", operation,
            archive_error_string(a), status);
    exit(1);
  }
}

static struct archive *new_writer_with_flags(int flags) {
  struct archive *a = archive_write_disk_new();
  if (a == NULL) {
    fail("archive_write_disk_new", "");
  }
  require_archive(a,
                  archive_write_disk_set_options(
                      a, flags | ARCHIVE_EXTRACT_WINDOWS_SYMLINK_TYPE |
                             ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                             ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                             ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS),
                  "archive_write_disk_set_options");
  return a;
}

static struct archive *new_writer(void) { return new_writer_with_flags(0); }

static struct archive_entry *new_entry(const char *path, unsigned int type) {
  struct archive_entry *e = archive_entry_new();
  if (e == NULL) {
    fail("archive_entry_new", path);
  }
  archive_entry_set_pathname(e, path);
  archive_entry_set_filetype(e, type);
  archive_entry_set_perm(e, type == AE_IFREG ? 0644 : 0755);
  archive_entry_set_size(e, 0);
  return e;
}

static void add_link_at_time(struct archive *a, const char *path,
                             const char *target, int type, time_t seconds,
                             long nanoseconds) {
  struct archive_entry *e = new_entry(path, AE_IFLNK);
  archive_entry_set_symlink(e, target);
  archive_entry_set_symlink_type(e, type);
  archive_entry_set_mtime(e, seconds, nanoseconds);
  require_archive(a, archive_write_header(a, e), path);
  require_archive(a, archive_write_finish_entry(a), "finish symlink");
  archive_entry_free(e);
}

static void add_link(struct archive *a, const char *path, const char *target,
                     int type) {
  add_link_at_time(a, path, target, type, 0, 0);
}

static void add_directory(struct archive *a, const char *path) {
  struct archive_entry *e = new_entry(path, AE_IFDIR);
  require_archive(a, archive_write_header(a, e), path);
  require_archive(a, archive_write_finish_entry(a), "finish directory");
  archive_entry_free(e);
}

static void add_file_at_time(struct archive *a, const char *path, time_t seconds,
                             long nanoseconds) {
  struct archive_entry *e = new_entry(path, AE_IFREG);
  archive_entry_set_size(e, sizeof(contents) - 1);
  archive_entry_set_mtime(e, seconds, nanoseconds);
  require_archive(a, archive_write_header(a, e), path);
  if (archive_write_data(a, contents, sizeof(contents) - 1) !=
      sizeof(contents) - 1) {
    fail("archive_write_data", path);
  }
  require_archive(a, archive_write_finish_entry(a), "finish file");
  archive_entry_free(e);
}

static void add_file(struct archive *a, const char *path) {
  add_file_at_time(a, path, 0, 0);
}

static void close_writer(struct archive *a) {
  require_archive(a, archive_write_close(a), "archive_write_close");
  if (archive_write_free(a) != ARCHIVE_OK) {
    fail("archive_write_free", "");
  }
}

static void expect_contents(const char *path) {
  char buffer[sizeof(contents) + 1];
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fail("fopen", path);
  }
  size_t n = fread(buffer, 1, sizeof(buffer), f);
  if (ferror(f) || n != sizeof(contents) - 1 ||
      memcmp(buffer, contents, n) != 0) {
    fail("incorrect file contents", path);
  }
  if (fclose(f) != 0) {
    fail("fclose", path);
  }
}

static void expect_absent(const char *path) {
#if defined(_WIN32) && !defined(__CYGWIN__)
  if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
    fail("unexpected path", path);
  }
  DWORD error = GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    fail("cannot check missing path", path);
  }
#else
  struct stat st;
  if (lstat(path, &st) == 0 || (errno != ENOENT && errno != ENOTDIR)) {
    fail("unexpected path", path);
  }
#endif
}

/* Check the link itself: even dangling links must retain their Windows type. */
static void expect_link(const char *path, int directory) {
#if defined(_WIN32) && !defined(__CYGWIN__)
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !!(attrs & FILE_ATTRIBUTE_DIRECTORY) != directory) {
    fail("incorrect Windows symlink type", path);
  }
#else
  struct stat st;
  (void)directory;
  if (lstat(path, &st) != 0 || !S_ISLNK(st.st_mode)) {
    fail("expected symlink", path);
  }
#endif
}

static void expect_regular_file(const char *path) {
#if defined(_WIN32) && !defined(__CYGWIN__)
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
    fail("expected regular file", path);
  }
#else
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    fail("expected regular file", path);
  }
#endif
}

static void test_framework_chain(void) {
  struct archive *a = new_writer();
  /* All links precede their targets, including a link in a target's prefix. */
  add_link(a, "sdk/Headers", "Versions/Current/Headers",
           AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "sdk/Config.h", "Headers/config.h", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "sdk/FileAlias", "Config.h", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "sdk/DirectoryAlias", "Headers", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "sdk/Versions/Current", "A", AE_SYMLINK_TYPE_UNDEFINED);
  add_directory(a, "sdk/Versions/A/Headers");
  add_file(a, "sdk/Versions/A/Headers/config.h");
#if defined(_WIN32) && !defined(__CYGWIN__)
  expect_absent("sdk/Headers");
  expect_absent("sdk/Config.h");
  expect_absent("sdk/FileAlias");
  expect_absent("sdk/DirectoryAlias");
  expect_absent("sdk/Versions/Current");
#endif
  close_writer(a);

  expect_contents("sdk/Headers/config.h");
  expect_contents("sdk/FileAlias");
  expect_contents("sdk/DirectoryAlias/config.h");
  expect_link("sdk/Headers", 1);
  expect_link("sdk/Versions/Current", 1);
  expect_link("sdk/DirectoryAlias", 1);
  expect_link("sdk/Config.h", 0);
  expect_link("sdk/FileAlias", 0);

  if (rename("sdk", "moved-sdk") != 0) {
    fail("rename", "sdk");
  }
  /* Read before inspecting attributes: directory enumeration masked #580. */
  expect_contents("moved-sdk/Headers/config.h");
  expect_contents("moved-sdk/FileAlias");
  expect_contents("moved-sdk/DirectoryAlias/config.h");
  expect_link("moved-sdk/Headers", 1);
  expect_link("moved-sdk/Versions/Current", 1);
}

static void test_explicit_and_unresolved_types(void) {
  struct archive *a = new_writer();
  add_directory(a, "types");
  add_link(a, "types/explicit-directory", "missing-directory",
           AE_SYMLINK_TYPE_DIRECTORY);
  add_link(a, "types/explicit-file", "missing-file", AE_SYMLINK_TYPE_FILE);
  /* Explicit type hints need no inference and remain immediately visible. */
  expect_link("types/explicit-directory", 1);
  expect_link("types/explicit-file", 0);
  add_link(a, "types/dangling-file", "missing-file", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "types/dangling-directory", "missing-directory/",
           AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "types/cycle-a", "cycle-b", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "types/cycle-b", "cycle-a", AE_SYMLINK_TYPE_UNDEFINED);
  close_writer(a);
  /* With no concrete target, retain libarchive's existing native heuristic. */
  expect_link("types/dangling-file", 0);
  expect_link("types/dangling-directory", 1);
  expect_link("types/cycle-a", 0);
  expect_link("types/cycle-b", 0);
}

static void test_later_entries_supersede_pending_links(void) {
  struct archive *a = new_writer();
  add_link(a, "overwritten", "missing", AE_SYMLINK_TYPE_UNDEFINED);
  add_file(a, "overwritten");
  add_link(a, "last-link", "missing", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "last-link", "last-target", AE_SYMLINK_TYPE_UNDEFINED);
  add_directory(a, "last-target");
  add_file(a, "last-target/file");
  close_writer(a);
  expect_regular_file("overwritten");
  expect_contents("overwritten");
  expect_link("last-link", 1);
  expect_contents("last-link/file");
}

static void test_secure_symlink_ancestor(void) {
  struct archive *a = new_writer();
  add_directory(a, "secure-target");
  add_link(a, "secure-alias", "secure-target", AE_SYMLINK_TYPE_UNDEFINED);
  struct archive_entry *e = new_entry("secure-alias/child", AE_IFREG);
  if (archive_write_header(a, e) >= ARCHIVE_OK) {
    fail("accepted a write through a symlink ancestor", "secure-alias/child");
  }
  archive_entry_free(e);
  close_writer(a);
  expect_absent("secure-target/child");
}

#if defined(_WIN32) && !defined(__CYGWIN__)
static void try_empty_file_at_time(struct archive *a, const char *path,
                                   time_t seconds, long nanoseconds) {
  struct archive_entry *e = new_entry(path, AE_IFREG);
  archive_entry_set_mtime(e, seconds, nanoseconds);
  require_archive(a, archive_write_header(a, e), path);
  require_archive(a, archive_write_finish_entry(a), "finish skipped file");
  archive_entry_free(e);
}

static void test_no_overwrite(void) {
  struct archive *a = new_writer();
  add_file(a, "existing-no-overwrite");
  close_writer(a);

  a = new_writer_with_flags(ARCHIVE_EXTRACT_NO_OVERWRITE);
  add_link(a, "keep-first", "keep-target", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "keep-first", "missing", AE_SYMLINK_TYPE_UNDEFINED);
  try_empty_file_at_time(a, "keep-first", 0, 0);
  add_link(a, "existing-no-overwrite", "keep-target", AE_SYMLINK_TYPE_UNDEFINED);
  add_directory(a, "keep-target");
  add_file(a, "keep-target/file");
  close_writer(a);
  expect_link("keep-first", 1);
  expect_contents("keep-first/file");
  expect_regular_file("existing-no-overwrite");
  expect_contents("existing-no-overwrite");
}

static void test_no_overwrite_newer(void) {
  struct archive *a = new_writer_with_flags(ARCHIVE_EXTRACT_TIME);
  add_file_at_time(a, "existing-newer", 300, 0);
  add_file_at_time(a, "existing-older", 100, 0);
  close_writer(a);

  a = new_writer_with_flags(ARCHIVE_EXTRACT_TIME |
                            ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER);
  add_link_at_time(a, "keep-newer", "newer-target", AE_SYMLINK_TYPE_UNDEFINED,
                   200, 1000000);
  try_empty_file_at_time(a, "keep-newer", 200, 0);
  add_link_at_time(a, "keep-equal", "newer-target", AE_SYMLINK_TYPE_UNDEFINED,
                   200, 1000000);
  add_link_at_time(a, "keep-equal", "missing", AE_SYMLINK_TYPE_UNDEFINED,
                   200, 1000000);
  add_link_at_time(a, "replace-older", "missing", AE_SYMLINK_TYPE_UNDEFINED,
                   100, 0);
  add_file_at_time(a, "replace-older", 200, 0);
  add_link_at_time(a, "existing-newer", "newer-target",
                   AE_SYMLINK_TYPE_UNDEFINED, 200, 0);
  add_link_at_time(a, "existing-older", "newer-target",
                   AE_SYMLINK_TYPE_UNDEFINED, 200, 0);
  add_directory(a, "newer-target");
  add_file(a, "newer-target/file");
  close_writer(a);
  expect_link("keep-newer", 1);
  expect_contents("keep-newer/file");
  expect_link("keep-equal", 1);
  expect_contents("keep-equal/file");
  expect_regular_file("replace-older");
  expect_contents("replace-older");
  expect_regular_file("existing-newer");
  expect_contents("existing-newer");
  expect_link("existing-older", 1);
  expect_contents("existing-older/file");
}

static void test_pending_descendant_blocks_non_directory(void) {
  struct archive *a = new_writer();
  add_link(a, "pending-parent/child", "missing", AE_SYMLINK_TYPE_UNDEFINED);
  struct archive_entry *e = new_entry("pending-parent", AE_IFREG);
  if (archive_write_header(a, e) >= ARCHIVE_OK) {
    fail("accepted a non-directory over a pending descendant", "pending-parent");
  }
  archive_entry_free(e);
  close_writer(a);
  expect_link("pending-parent/child", 0);
}

static void test_pending_target_replaces_stale_file(void) {
  struct archive *a = new_writer();
  add_file(a, "stale-target");
  add_directory(a, "stale-real-directory");
  add_file(a, "stale-real-directory/file");
  close_writer(a);

  a = new_writer();
  add_link(a, "stale-target", "stale-real-directory", AE_SYMLINK_TYPE_UNDEFINED);
  add_link(a, "stale-alias", "stale-target", AE_SYMLINK_TYPE_UNDEFINED);
  close_writer(a);
  /* The old regular file must not make the alias a file symlink. */
  expect_link("stale-target", 1);
  expect_link("stale-alias", 1);
  expect_contents("stale-alias/file");
}

static void test_close_reports_creation_failure(void) {
  struct archive *a = new_writer();
  add_link(a, "blocked", "block-target", AE_SYMLINK_TYPE_UNDEFINED);
  add_directory(a, "block-target");
  /* A nonempty directory introduced before close cannot become a symlink. */
  if (mkdir("blocked", 0755) != 0) {
    fail("mkdir", "blocked");
  }
  FILE *f = fopen("blocked/child", "wb");
  if (f == NULL || fclose(f) != 0) {
    fail("create blocker", "blocked/child");
  }
  if (archive_write_close(a) >= ARCHIVE_OK) {
    fail("close ignored deferred creation failure", "blocked");
  }
  archive_write_free(a);
}
#endif

int main(void) {
  const char *tmpdir = getenv("TEST_TMPDIR");
  if (tmpdir == NULL) {
    tmpdir = getenv("TMPDIR");
  }
  if (tmpdir != NULL && chdir(tmpdir) != 0) {
    fail("chdir", tmpdir);
  }
  char directory[80];
  int i;
  for (i = 0; i < 100; i++) {
    snprintf(directory, sizeof(directory), "pkgutil-symlink-test-%lu-%d",
             (unsigned long)getpid(), i);
    if (mkdir(directory, 0755) == 0) {
      break;
    }
    if (errno != EEXIST) {
      fail("mkdir", directory);
    }
  }
  if (i == 100 || chdir(directory) != 0) {
    fail("create test directory", directory);
  }

  test_framework_chain();
  test_explicit_and_unresolved_types();
  test_later_entries_supersede_pending_links();
  test_secure_symlink_ancestor();
#if defined(_WIN32) && !defined(__CYGWIN__)
  test_no_overwrite();
  test_no_overwrite_newer();
  test_pending_descendant_blocks_non_directory();
  test_pending_target_replaces_stale_file();
  test_close_reports_creation_failure();
#endif
  puts("Symlink extraction tests passed.");
  return 0;
}
