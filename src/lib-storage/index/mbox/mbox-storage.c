/* Copyright (C) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "istream.h"
#include "home-expand.h"
#include "mkdir-parents.h"
#include "unlink-directory.h"
#include "subscription-file/subscription-file.h"
#include "mbox-storage.h"
#include "mbox-lock.h"
#include "mbox-file.h"
#include "mbox-sync-private.h"
#include "mail-copy.h"
#include "index-mail.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define CREATE_MODE 0770 /* umask() should limit it more */

/* NOTE: must be sorted for istream-header-filter. */
const char *mbox_hide_headers[] = {
	"Content-Length",
	"Status",
	"X-IMAP",
	"X-IMAPbase",
	"X-Keywords",
	"X-Status",
	"X-UID",
};
size_t mbox_hide_headers_count = 7;

extern struct mail_storage mbox_storage;
extern struct mailbox mbox_mailbox;

int mbox_set_syscall_error(struct index_mailbox *ibox, const char *function)
{
	i_assert(function != NULL);

	mail_storage_set_critical(ibox->box.storage,
		"%s failed with mbox file %s: %m", function, ibox->path);
	return -1;
}

static int mbox_handle_errors(struct index_storage *istorage)
{
	struct mail_storage *storage = &istorage->storage;

	if (ENOACCESS(errno))
		mail_storage_set_error(storage, "Permission denied");
	else if (ENOSPACE(errno))
		mail_storage_set_error(storage, "Not enough disk space");
	else if (ENOTFOUND(errno))
		mail_storage_set_error(storage, "Directory structure is broken");
	else
		return FALSE;
	return TRUE;
}

static int mbox_is_file(const char *path, const char *name, int debug)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: stat(%s) failed: %m",
			       name, path);
		}
		return FALSE;
	}
	if (S_ISDIR(st.st_mode)) {
		if (debug) {
			i_info("mbox autodetect: %s: is a directory (%s)",
			       name, path);
		}
		return FALSE;
	}
	if (access(path, R_OK|W_OK) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: no R/W access (%s)",
			       name, path);
		}
		return FALSE;
	}

	if (debug)
		i_info("mbox autodetect: %s: yes (%s)", name, path);
	return TRUE;
}

static int mbox_is_dir(const char *path, const char *name, int debug)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: stat(%s) failed: %m",
			       name, path);
		}
		return FALSE;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (debug) {
			i_info("mbox autodetect: %s: is not a directory (%s)",
			       name, path);
		}
		return FALSE;
	}
	if (access(path, R_OK|W_OK|X_OK) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: no R/W/X access (%s)",
			       name, path);
		}
		return FALSE;
	}

	if (debug)
		i_info("mbox autodetect: %s: yes (%s)", name, path);
	return TRUE;
}

static int mbox_autodetect(const char *data, enum mail_storage_flags flags)
{
	int debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;
	const char *path;

	path = t_strcut(data, ':');

	if (debug) {
		if (strchr(data, ':') != NULL) {
			i_info("mbox autodetect: data=%s, splitting ':' -> %s",
			       data, path);
		} else {
			i_info("mbox autodetect: data=%s", data);
		}
	}

	if (*path != '\0' && mbox_is_file(path, "INBOX file", debug))
		return TRUE;

	if (mbox_is_dir(t_strconcat(path, "/.imap", NULL), "has .imap/", debug))
		return TRUE;
	if (mbox_is_file(t_strconcat(path, "/inbox", NULL), "has inbox", debug))
		return TRUE;
	if (mbox_is_file(t_strconcat(path, "/mbox", NULL), "has mbox", debug))
		return TRUE;

	return FALSE;
}

static const char *get_root_dir(enum mail_storage_flags flags)
{
	const char *home, *path;
	int debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;

	home = getenv("HOME");
	if (home != NULL) {
		path = t_strconcat(home, "/mail", NULL);
		if (access(path, R_OK|W_OK|X_OK) == 0) {
			if (debug)
				i_info("mbox: root exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: root: access(%s, rwx) failed: %m", path);

		path = t_strconcat(home, "/Mail", NULL);
		if (access(path, R_OK|W_OK|X_OK) == 0) {
			if (debug)
				i_info("mbox: root exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: root: access(%s, rwx) failed: %m", path);
	}

	if (debug)
		i_info("mbox: checking if we are chrooted:");
	if (mbox_autodetect("", flags))
		return "/";

	if (debug)
		i_info("mbox: root directory not found");

	return NULL;
}

static const char *
get_inbox_file(const char *root_dir, int only_root, int debug)
{
	const char *user, *path;

	if (!only_root && (user = getenv("USER")) != NULL) {
		path = t_strconcat("/var/mail/", user, NULL);
		if (access(path, R_OK|W_OK) == 0) {
			if (debug)
				i_info("mbox: INBOX exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: INBOX: access(%s, rw) failed: %m", path);

		path = t_strconcat("/var/spool/mail/", user, NULL);
		if (access(path, R_OK|W_OK) == 0) {
			if (debug)
				i_info("mbox: INBOX exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: INBOX: access(%s, rw) failed: %m", path);
	}

	path = t_strconcat(root_dir, "/inbox", NULL);
	if (debug)
		i_info("mbox: INBOX defaulted to %s", path);
	return path;
}

static const char *create_root_dir(int debug)
{
	const char *home, *path;

	home = getenv("HOME");
	if (home == NULL) {
		i_error("mbox: We need root IMAP folder, "
			"but can't find it or HOME environment");
		return NULL;
	}

	path = t_strconcat(home, "/mail", NULL);
	if (mkdir_parents(path, CREATE_MODE) < 0) {
		i_error("mbox: Can't create root IMAP folder %s: %m", path);
		return NULL;
	}

	if (debug)
		i_info("mbox: root directory created: %s", path);
	return path;
}

static struct mail_storage *
mbox_create(const char *data, const char *user, enum mail_storage_flags flags)
{
	int debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;
	struct index_storage *storage;
	const char *root_dir, *inbox_file, *index_dir, *p;
	struct stat st;
	int autodetect;
	pool_t pool;

	root_dir = inbox_file = index_dir = NULL;

	autodetect = data == NULL || *data == '\0';
	if (autodetect) {
		/* we'll need to figure out the mail location ourself.
		   it's root dir if we've already chroot()ed, otherwise
		   either $HOME/mail or $HOME/Mail */
		root_dir = get_root_dir(flags);
	} else {
		/* <root folder> | <INBOX path>
		   [:INBOX=<path>] [:INDEX=<dir>] */
		if (debug)
			i_info("mbox: data=%s", data);
		p = strchr(data, ':');
		if (p == NULL) {
			if (stat(data, &st) < 0) {
				i_error("Invalid mbox path %s: %m", data);
				return NULL;
			}

			if (S_ISDIR(st.st_mode))
				root_dir = data;
			else {
				root_dir = get_root_dir(flags);
				inbox_file = data;
			}
		} else {
			root_dir = t_strdup_until(data, p);
			do {
				p++;
				if (strncmp(p, "INBOX=", 6) == 0)
					inbox_file = t_strcut(p+6, ':');
				else if (strncmp(p, "INDEX=", 6) == 0)
					index_dir = t_strcut(p+6, ':');
				p = strchr(p, ':');
			} while (p != NULL);
		}
	}

	if (root_dir == NULL) {
		root_dir = create_root_dir(debug);
		if (root_dir == NULL)
			return NULL;
	} else {
		/* strip trailing '/' */
		size_t len = strlen(root_dir);
		if (root_dir[len-1] == '/')
			root_dir = t_strndup(root_dir, len-1);

		/* make sure the directory exists */
		if (lstat(root_dir, &st) == 0) {
			/* yep, go ahead */
		} else if (errno != ENOENT && errno != ENOTDIR) {
			i_error("lstat(%s) failed: %m", root_dir);
			return NULL;
		} else if (mkdir_parents(root_dir, CREATE_MODE) < 0 &&
			   errno != EEXIST) {
			i_error("mkdir_parents(%s) failed: %m", root_dir);
			return NULL;
		}
	}

	if (inbox_file == NULL)
		inbox_file = get_inbox_file(root_dir, !autodetect, debug);

	if (index_dir == NULL)
		index_dir = root_dir;
	else if (strcmp(index_dir, "MEMORY") == 0)
		index_dir = NULL;

	if (debug) {
		i_info("mbox: root=%s, index=%s, inbox=%s",
		       root_dir, index_dir == NULL ? "" : index_dir,
		       inbox_file == NULL ? "" : inbox_file);
	}

	pool = pool_alloconly_create("storage", 256);
	storage = p_new(pool, struct index_storage, 1);
	storage->storage = mbox_storage;
	storage->storage.pool = pool;

	storage->dir = p_strdup(pool, home_expand(root_dir));
	storage->inbox_path = p_strdup(pool, home_expand(inbox_file));
	storage->index_dir = p_strdup(pool, home_expand(index_dir));
	storage->user = p_strdup(pool, user);
	storage->callbacks = p_new(pool, struct mail_storage_callbacks, 1);
	index_storage_init(storage, flags);
	return &storage->storage;
}

static void mbox_free(struct mail_storage *_storage)
{
	struct index_storage *storage = (struct index_storage *)_storage;

	index_storage_deinit(storage);
	pool_unref(storage->storage.pool);
}

int mbox_is_valid_mask(struct mail_storage *storage, const char *mask)
{
	const char *p;
	int newdir;

	if ((storage->flags & MAIL_STORAGE_FLAG_FULL_FS_ACCESS) != 0)
		return TRUE;

	/* make sure it's not absolute path */
	if (*mask == '/' || *mask == '~')
		return FALSE;

	/* make sure there's no "../" stuff */
	newdir = TRUE;
	for (p = mask; *p != '\0'; p++) {
		if (newdir && p[0] == '.' && p[1] == '.' && p[2] == '/')
			return FALSE;
		newdir = p[0] == '/';
	}

	return TRUE;
}

static int mbox_is_valid_create_name(struct mail_storage *storage,
				     const char *name)
{
	size_t len;

	len = strlen(name);
	if (name[0] == '\0' || name[len-1] == '/' ||
	    strchr(name, '*') != NULL || strchr(name, '%') != NULL)
		return FALSE;

	return mbox_is_valid_mask(storage, name);
}

static int mbox_is_valid_existing_name(struct mail_storage *storage,
				       const char *name)
{
	size_t len;

	len = strlen(name);
	if (name[0] == '\0' || name[len-1] == '/')
		return FALSE;

	return mbox_is_valid_mask(storage, name);
}

static const char *mbox_get_index_dir(struct index_storage *storage,
				      const char *name)
{
	const char *p;

	if (storage->index_dir == NULL)
		return NULL;

	if ((storage->storage.flags & MAIL_STORAGE_FLAG_FULL_FS_ACCESS) != 0 &&
	    (*name == '/' || *name == '~')) {
		name = home_expand(name);
		p = strrchr(name, '/');
		return t_strconcat(t_strdup_until(name, p),
				   "/.imap/", p+1, NULL);
	}

	p = strrchr(name, '/');
	if (p == NULL)
		return t_strconcat(storage->index_dir, "/.imap/", name, NULL);
	else {
		return t_strconcat(storage->index_dir, "/",
				   t_strdup_until(name, p),
				   "/.imap/", p+1, NULL);
	}
}

static int create_mbox_index_dirs(struct index_storage *storage,
				  const char *name)
{
	const char *index_dir;

	index_dir = mbox_get_index_dir(storage, name);
	if (index_dir == NULL)
		return 0;

	if (mkdir_parents(index_dir, CREATE_MODE) < 0) {
		mail_storage_set_critical(&storage->storage,
			"mkdir_parents(%s) failed: %m", index_dir);
		return -1;
	}

	return 0;
}

static int verify_inbox(struct index_storage *storage)
{
	int fd;

	/* make sure inbox file itself exists */
	fd = open(storage->inbox_path, O_RDWR | O_CREAT | O_EXCL, 0660);
	if (fd != -1)
		(void)close(fd);
	else if (errno != EEXIST) {
		mail_storage_set_critical(&storage->storage,
			"open(%s, O_CREAT) failed: %m", storage->inbox_path);
	}

	/* make sure the index directories exist */
	if (create_mbox_index_dirs(storage, "INBOX") < 0)
		return -1;

	return 0;
}

static const char *
mbox_get_path(struct index_storage *storage, const char *name)
{
	if (strcmp(name, "INBOX") == 0)
		return storage->inbox_path;
	if ((storage->storage.flags & MAIL_STORAGE_FLAG_FULL_FS_ACCESS) != 0 &&
	    (*name == '/' || *name == '~'))
		return home_expand(name);
	return t_strconcat(storage->dir, "/", name, NULL);
}

static int mbox_mail_is_recent(struct index_mailbox *ibox __attr_unused__,
			       uint32_t uid __attr_unused__)
{
	return FALSE;
}

static struct index_mailbox *
mbox_alloc(struct index_storage *storage, struct mail_index *index,
	   const char *name, enum mailbox_open_flags flags)
{
	struct index_mailbox *ibox;
	pool_t pool;

	pool = pool_alloconly_create("mailbox", 256);
	ibox = p_new(pool, struct index_mailbox, 1);
	ibox->box = mbox_mailbox;
	ibox->box.pool = pool;
	ibox->storage = storage;

	if (index_storage_mailbox_init(ibox, index, name, flags) < 0) {
		/* the memory is already freed here, no need to deinit */
		return NULL;
	}

	ibox->mbox_fd = -1;
	ibox->mbox_lock_type = F_UNLCK;
	ibox->mbox_ext_idx =
		mail_index_ext_register(index, "mbox", 0,
					sizeof(uint64_t), sizeof(uint64_t));

	ibox->is_recent = mbox_mail_is_recent;
	ibox->mail_vfuncs = &mbox_mail_vfuncs;
        ibox->mbox_very_dirty_syncs = getenv("MBOX_VERY_DIRTY_SYNCS") != NULL;
	ibox->mbox_do_dirty_syncs = ibox->mbox_very_dirty_syncs ||
		getenv("MBOX_DIRTY_SYNCS") != NULL;

	ibox->md5hdr_ext_idx =
		mail_index_ext_register(ibox->index, "header-md5", 0, 16, 1);
	if ((flags & MAILBOX_OPEN_KEEP_HEADER_MD5) != 0)
		ibox->mbox_save_md5 = TRUE;
	return ibox;
}

static struct mailbox *
mbox_open(struct index_storage *storage, const char *name,
	  enum mailbox_open_flags flags)
{
	struct index_mailbox *ibox;
	struct mail_index *index;
	const char *path, *index_dir;

	if (strcmp(name, "INBOX") == 0) {
		/* name = "INBOX"
		   path = "<inbox_file>/INBOX"
		   index_dir = "/mail/.imap/INBOX" */
		path = storage->inbox_path;
		index_dir = mbox_get_index_dir(storage, "INBOX");
	} else {
		/* name = "foo/bar"
		   path = "/mail/foo/bar"
		   index_dir = "/mail/foo/.imap/bar" */
		path = mbox_get_path(storage, name);
		index_dir = mbox_get_index_dir(storage, name);
	}

	index = index_storage_alloc(index_dir, path, MBOX_INDEX_PREFIX);
	ibox = mbox_alloc(storage, index, name, flags);
	if (ibox == NULL)
		return NULL;

	ibox->path = p_strdup(ibox->box.pool, path);

	if (access(path, R_OK|W_OK) < 0) {
		if (errno < EACCES)
			mbox_set_syscall_error(ibox, "access()");
		else {
			ibox->readonly = TRUE;
			ibox->mbox_readonly = TRUE;
		}
	}

	return &ibox->box;
}

static struct mailbox *
mbox_mailbox_open_stream(struct index_storage *storage, const char *name,
			 struct istream *input, enum mailbox_open_flags flags)
{
	struct mail_index *index;
	struct index_mailbox *ibox;

	flags |= MAILBOX_OPEN_READONLY;

	index = mail_index_alloc(NULL, NULL);
	ibox = mbox_alloc(storage, index, name, flags);
	if (ibox == NULL)
		return NULL;

	i_stream_ref(input);
	ibox->mbox_file_stream = input;
	ibox->mbox_readonly = TRUE;

	ibox->path = "(read-only mbox stream)";
	return &ibox->box;
}

static struct mailbox *
mbox_mailbox_open(struct mail_storage *_storage, const char *name,
		  struct istream *input, enum mailbox_open_flags flags)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *path;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (input != NULL)
		return mbox_mailbox_open_stream(storage, name, input, flags);

	if (strcmp(name, "INBOX") == 0) {
		/* make sure INBOX exists */
		if (verify_inbox(storage) < 0)
			return NULL;
		return mbox_open(storage, "INBOX", flags);
	}

	if (!mbox_is_valid_existing_name(_storage, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return NULL;
	}

	path = mbox_get_path(storage, name);
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			mail_storage_set_error(_storage,
				"Mailbox isn't selectable: %s", name);
			return NULL;
		}

		/* exists - make sure the required directories are also there */
		if (create_mbox_index_dirs(storage, name) < 0)
			return NULL;

		return mbox_open(storage, name, flags);
	}

	if (ENOTFOUND(errno)) {
		mail_storage_set_error(_storage, "Mailbox doesn't exist: %s",
				       name);
	} else if (!mbox_handle_errors(storage)) {
		mail_storage_set_critical(_storage, "stat(%s) failed: %m",
					  path);
	}

	return NULL;
}

static int mbox_mailbox_create(struct mail_storage *_storage, const char *name,
			       int directory)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *path, *p;
	struct stat st;
	int fd;

	mail_storage_clear_error(_storage);

	if (!mbox_is_valid_create_name(_storage, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	if (strncasecmp(name, "INBOX/", 6) == 0) {
		/* We might actually be able to create mailboxes under INBOX
		   because the real INBOX file isn't usually named as INBOX
		   in the root mail directory. that would anyway be a special
		   case which would require special handling elsewhere, so just
		   don't allow it. */
		mail_storage_set_error(_storage,
				"Mailbox doesn't allow inferior mailboxes");
		return -1;
	}

	/* make sure it doesn't exist already */
	path = mbox_get_path(storage, name);
	if (stat(path, &st) == 0) {
		mail_storage_set_error(_storage, "Mailbox already exists");
		return -1;
	}

	if (errno != ENOENT && errno != ELOOP && errno != EACCES) {
		if (errno == ENOTDIR) {
			mail_storage_set_error(_storage,
				"Mailbox doesn't allow inferior mailboxes");
		} else {
			mail_storage_set_critical(_storage,
				"stat() failed for mbox file %s: %m", path);
		}
		return -1;
	}

	/* create the hierarchy if needed */
	p = directory ? path + strlen(path) : strrchr(path, '/');
	if (p != NULL) {
		p = t_strdup_until(path, p);
		if (mkdir_parents(p, CREATE_MODE) < 0) {
			if (mbox_handle_errors(storage))
				return -1;

			mail_storage_set_critical(_storage,
				"mkdir_parents(%s) failed: %m", p);
			return -1;
		}

		if (directory) {
			/* wanted to create only the directory */
			return 0;
		}
	}

	/* create the mailbox file */
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0660);
	if (fd != -1) {
		(void)close(fd);
		return 0;
	}

	if (errno == EEXIST) {
		/* mailbox was just created between stat() and open() call.. */
		mail_storage_set_error(_storage, "Mailbox already exists");
	} else if (!mbox_handle_errors(storage)) {
		mail_storage_set_critical(_storage,
			"Can't create mailbox %s: %m", name);
	}
	return -1;
}

static int mbox_mailbox_delete(struct mail_storage *_storage, const char *name)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *index_dir, *path;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (strcmp(name, "INBOX") == 0) {
		mail_storage_set_error(_storage, "INBOX can't be deleted.");
		return -1;
	}

	if (!mbox_is_valid_existing_name(_storage, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	path = mbox_get_path(storage, name);
	if (lstat(path, &st) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				"Mailbox doesn't exist: %s", name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"lstat() failed for %s: %m", path);
		}
		return -1;
	}

	if (S_ISDIR(st.st_mode)) {
		/* deleting a folder, only allow it if it's empty.
		   Delete .imap folder before to make sure it goes empty. */
		index_dir = t_strconcat(storage->index_dir, "/", name,
					"/.imap", NULL);

		if (index_dir != NULL && rmdir(index_dir) < 0 &&
		    !ENOTFOUND(errno) && errno != ENOTEMPTY) {
			if (!mbox_handle_errors(storage) < 0) {
				mail_storage_set_critical(_storage,
					"rmdir() failed for %s: %m", index_dir);
				return -1;
			}
		}

		if (rmdir(path) == 0)
			return 0;

		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				"Mailbox doesn't exist: %s", name);
		} else if (errno == ENOTEMPTY) {
			mail_storage_set_error(_storage,
				"Folder %s isn't empty, can't delete it.",
				name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"rmdir() failed for %s: %m", path);
		}
		return -1;
	}

	/* first unlink the mbox file */
	if (unlink(path) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				"Mailbox doesn't exist: %s", name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"unlink() failed for %s: %m", path);
		}
		return -1;
	}

	/* next delete the index directory */
	index_dir = mbox_get_index_dir(storage, name);
	if (index_dir != NULL) {
		index_storage_destroy_unrefed();

		if (unlink_directory(index_dir, TRUE) < 0 && errno != ENOENT) {
			mail_storage_set_critical(_storage,
				"unlink_directory(%s) failed: %m", index_dir);

			/* mailbox itself is deleted, so return success
			   anyway */
		}
	}

	return 0;
}

static int mbox_mailbox_rename(struct mail_storage *_storage,
			       const char *oldname, const char *newname)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *oldpath, *newpath, *old_indexdir, *new_indexdir, *p;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (!mbox_is_valid_existing_name(_storage, oldname) ||
	    !mbox_is_valid_create_name(_storage, newname)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	if (strncasecmp(newname, "INBOX/", 6) == 0) {
		/* Not allowed - see explanation in mbox_mailbox_create */
		mail_storage_set_error(_storage,
			"Target mailbox doesn't allow inferior mailboxes");
		return -1;
	}

	oldpath = mbox_get_path(storage, oldname);
	newpath = mbox_get_path(storage, newname);

	/* create the hierarchy */
	p = strrchr(newpath, '/');
	if (p != NULL) {
		p = t_strdup_until(newpath, p);
		if (mkdir_parents(p, CREATE_MODE) < 0) {
			if (mbox_handle_errors(storage))
				return -1;

			mail_storage_set_critical(_storage,
				"mkdir_parents(%s) failed: %m", p);
			return -1;
		}
	}

	/* first check that the destination mailbox doesn't exist.
	   this is racy, but we need to be atomic and there's hardly any
	   possibility that someone actually tries to rename two mailboxes
	   to same new one */
	if (lstat(newpath, &st) == 0) {
		mail_storage_set_error(_storage,
				       "Target mailbox already exists");
		return -1;
	} else if (errno == ENOTDIR) {
		mail_storage_set_error(_storage,
			"Target mailbox doesn't allow inferior mailboxes");
		return -1;
	} else if (errno != ENOENT && errno != EACCES) {
		mail_storage_set_critical(_storage, "lstat(%s) failed: %m",
					  newpath);
		return -1;
	}

	/* NOTE: renaming INBOX works just fine with us, it's simply recreated
	   the next time it's needed. */
	if (rename(oldpath, newpath) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				"Mailbox doesn't exist: %s", oldname);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"rename(%s, %s) failed: %m", oldpath, newpath);
		}
		return -1;
	}

	/* we need to rename the index directory as well */
	old_indexdir = mbox_get_index_dir(storage, oldname);
	new_indexdir = mbox_get_index_dir(storage, newname);
	if (old_indexdir != NULL) {
		if (rename(old_indexdir, new_indexdir) < 0 &&
		    errno != ENOENT) {
			mail_storage_set_critical(_storage,
						  "rename(%s, %s) failed: %m",
						  old_indexdir, new_indexdir);
		}
	}

	return 0;
}

static int mbox_set_subscribed(struct mail_storage *_storage,
			       const char *name, int set)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *path;

	path = t_strconcat(storage->dir, "/" SUBSCRIPTION_FILE_NAME, NULL);
	return subsfile_set_subscribed(_storage, path, storage->temp_prefix,
				       name, set);
}

static int mbox_get_mailbox_name_status(struct mail_storage *_storage,
					const char *name,
					enum mailbox_name_status *status)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	struct stat st;
	const char *path;

	mail_storage_clear_error(_storage);

	if (!mbox_is_valid_existing_name(_storage, name)) {
		*status = MAILBOX_NAME_INVALID;
		return 0;
	}

	path = mbox_get_path(storage, name);
	if (strcmp(name, "INBOX") == 0 || stat(path, &st) == 0) {
		*status = MAILBOX_NAME_EXISTS;
		return 0;
	}

	if (!mbox_is_valid_create_name(_storage, name)) {
		*status = MAILBOX_NAME_INVALID;
		return 0;
	}

	if (ENOTFOUND(errno) || errno == EACCES) {
		*status = MAILBOX_NAME_VALID;
		return 0;
	} else if (errno == ENOTDIR) {
		*status = MAILBOX_NAME_NOINFERIORS;
		return 0;
	} else {
		mail_storage_set_critical(_storage, "mailbox name status: "
					  "stat(%s) failed: %m", path);
		return -1;
	}
}

static int mbox_storage_close(struct mailbox *box)
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;
	const struct mail_index_header *hdr;
	struct mail_index *free_index = NULL;
	int ret = 0;

	hdr = mail_index_get_header(ibox->view);
	if ((hdr->flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) != 0 &&
	    !ibox->readonly && !ibox->mbox_readonly) {
		/* we've done changes to mbox which haven't been written yet.
		   do it now. */
		if (mbox_sync(ibox, MBOX_SYNC_REWRITE) < 0)
			ret = -1;
	}

        mbox_file_close(ibox);
	if (ibox->mbox_file_stream != NULL) {
		i_stream_unref(ibox->mbox_file_stream);
		ibox->mbox_file_stream = NULL;

		/* it's not in storage's index cache, so free it manually */
		free_index = ibox->index;
		ibox->index = NULL;
	}

	index_storage_mailbox_free(box);
	if (free_index != NULL)
		mail_index_free(free_index);
	return ret;
}

static void
mbox_notify_changes(struct mailbox *box, unsigned int min_interval,
		    mailbox_notify_callback_t *callback, void *context)
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;

	ibox->min_notify_interval = min_interval;
	ibox->notify_callback = callback;
	ibox->notify_context = context;

	if (callback == NULL)
		index_mailbox_check_remove_all(ibox);
	else
		index_mailbox_check_add(ibox, ibox->path, FALSE);
}

struct mail_storage mbox_storage = {
	MEMBER(name) "mbox",
	MEMBER(hierarchy_sep) '/',

	{
		mbox_create,
		mbox_free,
		mbox_autodetect,
		index_storage_set_callbacks,
		mbox_mailbox_open,
		mbox_mailbox_create,
		mbox_mailbox_delete,
		mbox_mailbox_rename,
		mbox_mailbox_list_init,
		mbox_mailbox_list_next,
		mbox_mailbox_list_deinit,
		mbox_set_subscribed,
		mbox_get_mailbox_name_status,
		index_storage_get_last_error
	},

	MEMBER(pool) NULL,
	MEMBER(error) NULL,
	MEMBER(flags) 0,
	MEMBER(module_contexts) ARRAY_INIT,
	MEMBER(syntax_error) 0
};

struct mailbox mbox_mailbox = {
	MEMBER(name) NULL, 
	MEMBER(storage) NULL, 

	{
		index_storage_is_readonly,
		index_storage_allow_new_keywords,
		mbox_storage_close,
		index_storage_get_status,
		mbox_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		mbox_notify_changes,
		mbox_transaction_begin,
		mbox_transaction_commit,
		mbox_transaction_rollback,
		index_keywords_create,
		index_keywords_free,
		index_storage_get_uids,
		index_mail_alloc,
		index_header_lookup_init,
		index_header_lookup_deinit,
		index_storage_search_get_sorting,
		index_storage_search_init,
		index_storage_search_deinit,
		index_storage_search_next,
		mbox_save_init,
		mbox_save_continue,
		mbox_save_finish,
		mbox_save_cancel,
		mail_storage_copy,
		index_storage_is_inconsistent
	},

	MEMBER(pool) NULL,
	MEMBER(module_contexts) ARRAY_INIT
};
