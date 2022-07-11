/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "worktree.h"

#include "buf.h"
#include "repository.h"
#include "path.h"

#include "git2/branch.h"
#include "git2/commit.h"
#include "git2/worktree.h"

static bool is_worktree_private_directory(git_str *path) {
	return git_fs_path_contains_file(path, "commondir")
		&& git_fs_path_contains_file(path, "gitdir")
		&& git_fs_path_contains_file(path, "HEAD");

}

/* Returns the absolute path to the worktree's working directory */
static int get_worktree_gitdir_from_private_directory(git_str *out, git_str *directory) {
	git_str buf = GIT_STR_INIT;
	int error;

	GIT_ASSERT_ARG(directory);

	if (!is_worktree_private_directory(directory)) {
		error = -1;
		goto out;
	}
	if ((error = git_str_sets(&buf, git_worktree__read_link(directory->ptr, "gitdir")) < 0))
		goto out;

	if ((error = git_fs_path_apply_relative(&buf, "..")) < 0)
		goto out;

	if ((error = git_str_sets(out, buf.ptr)) < 0)
		goto out;

out:
	git_str_dispose(&buf);

	return error;
}

int git_worktree_list(git_strarray *wts, git_repository *repo)
{
	git_vector worktrees = GIT_VECTOR_INIT;
	git_str path = GIT_STR_INIT, worktree_dir = GIT_STR_INIT;
	char *worktree;
	size_t i, len;
	int error;

	GIT_ASSERT_ARG(wts);
	GIT_ASSERT_ARG(repo);

	wts->count = 0;
	wts->strings = NULL;

	if ((error = git_str_joinpath(&path, repo->commondir, "worktrees/")) < 0)
		goto exit;
	if (!git_fs_path_exists(path.ptr) || git_fs_path_is_empty_dir(path.ptr))
		goto exit;
	if ((error = git_fs_path_dirload(&worktrees, path.ptr, path.size, 0x0)) < 0)
		goto exit;

	len = path.size;

	/* TODO: instead of returning the path in .git/worktrees, we have to
	 * parse gitdir inside .git/worktrees/<worktree>/ which points to the
	 * correct path
	 *
	 * It is the absolute path to the worktree path, including .git as the
	 * last component
	 *
	 * git_worktree__read_link() should help with that.
	 */
	git_vector_foreach(&worktrees, i, worktree) {
		git_str_truncate(&path, len);
		git_str_puts(&path, worktree);

		if (!is_worktree_private_directory(&path)) {
			git_vector_remove(&worktrees, i);
			git__free(worktree);
		} else {
			if ((error = get_worktree_gitdir_from_private_directory(&worktree_dir, &path)) < 0)
				goto exit;
			if ((error = git_vector_set(NULL, &worktrees, i, worktree)) < 0)
				goto exit;
		}
	}

	wts->strings = (char **)git_vector_detach(&wts->count, NULL, &worktrees);

exit:
	git_str_dispose(&path);

	return error;
}

char *git_worktree__read_link(const char *base, const char *file)
{
	git_str path = GIT_STR_INIT, buf = GIT_STR_INIT;

	GIT_ASSERT_ARG_WITH_RETVAL(base, NULL);
	GIT_ASSERT_ARG_WITH_RETVAL(file, NULL);

	if (git_str_joinpath(&path, base, file) < 0)
		goto err;
	if (git_futils_readbuffer(&buf, path.ptr) < 0)
		goto err;
	git_str_dispose(&path);

	git_str_rtrim(&buf);

	if (!git_fs_path_is_relative(buf.ptr))
		return git_str_detach(&buf);

	if (git_str_sets(&path, base) < 0)
		goto err;
	if (git_fs_path_apply_relative(&path, buf.ptr) < 0)
		goto err;
	git_str_dispose(&buf);

	return git_str_detach(&path);

err:
	git_str_dispose(&buf);
	git_str_dispose(&path);

	return NULL;
}

static int write_wtfile(const char *base, const char *file, const git_str *buf)
{
	git_str path = GIT_STR_INIT;
	int err;

	GIT_ASSERT_ARG(base);
	GIT_ASSERT_ARG(file);
	GIT_ASSERT_ARG(buf);

	if ((err = git_str_joinpath(&path, base, file)) < 0)
		goto out;

	if ((err = git_futils_writebuffer(buf, path.ptr, O_CREAT|O_EXCL|O_WRONLY, 0644)) < 0)
		goto out;

out:
	git_str_dispose(&path);

	return err;
}

/*
 * parent: the root of the "actual" git repository
 * dir: the working directory of the worktree
 * dir: name: nothing. meaningless. just loop it through to the struct
*/
static int open_worktree_dir(git_worktree **out, const char *parent, const char *dir, const char *name)
{
	git_str gitdir = GIT_STR_INIT;
	git_worktree *wt = NULL;
	int error = 0;

	if ((error = git_str_puts(&gitdir, dir)) < 0)
		goto out;

	if (!is_worktree_private_directory(&gitdir)) {
		error = -1;
		goto out;
	}

	if ((error = git_path_validate_length(NULL, dir)) < 0)
		goto out;

	if ((wt = git__calloc(1, sizeof(*wt))) == NULL) {
		error = -1;
		goto out;
	}

	if ((wt->name = git__strdup(name)) == NULL) {
		error = -1;
		goto out;
	}
	if ((wt->commondir_path = git_worktree__read_link(dir, "commondir")) == NULL) {
		error = -1;
		goto out;
	}
	if ((wt->gitlink_path = git_worktree__read_link(dir, "gitdir")) == NULL) {
		error = -1;
		goto out;
	}
	if ((parent && (wt->parent_path = git__strdup(parent)) == NULL)) {
		error = -1;
		goto out;
	}
	if ((wt->worktree_path = git_fs_path_dirname(wt->gitlink_path)) == NULL) {
		error = -1;
		goto out;
	}

	if ((error = git_fs_path_prettify_dir(&gitdir, dir, NULL)) < 0)
		goto out;
	wt->gitdir_path = git_str_detach(&gitdir);

	if ((error = git_worktree_is_locked(NULL, wt)) < 0)
		goto out;
	wt->locked = !!error;
	error = 0;

	*out = wt;

out:
	if (error)
		git_worktree_free(wt);
	git_str_dispose(&gitdir);

	return error;
}

int git_worktree_lookup(git_worktree **out, git_repository *repo, const char *name)
{
	int error, found;

	git_vector worktrees = GIT_VECTOR_INIT;
	git_str path = GIT_STR_INIT, worktree_dir = GIT_STR_INIT;
	char *basename1, *basename2;
	char *worktree;
	size_t i, len;

	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(name);

	*out = NULL;

	if ((error = git_str_joinpath(&path, repo->commondir, "worktrees/")) < 0)
		goto out;
	if (!git_fs_path_exists(path.ptr) || git_fs_path_is_empty_dir(path.ptr))
		goto out;
	if ((error = git_fs_path_dirload(&worktrees, path.ptr, path.size, 0x0)) < 0)
		goto out;

	len = path.size;

	found = 0;

	git_vector_foreach(&worktrees, i, worktree) {
		/* printf("found worktree: %s\n", worktree); */
		git_str_truncate(&path, len);
		git_str_puts(&path, worktree);

		if (is_worktree_private_directory(&path)) {
			/* printf("getting worktree_gitdir\n"); */
			if ((error = get_worktree_gitdir_from_private_directory(&worktree_dir, &path)) < 0) {
				/* printf("something went wrong\n"); */
				goto out;
			}
			/* printf("found worktree dir: %s\n", worktree_dir.ptr); */
			/* printf("basename1\n"); */
			basename1 = git_fs_path_basename(worktree_dir.ptr);
			/* printf("basename2\n"); */
			basename2 = git_fs_path_basename(name);
			/* printf("comparing: \n%s\n%s\n", basename1, basename2); */

			if (strcmp(basename1, basename2) == 0) {
				/* printf("MATCHES"); */
				if ((error = (open_worktree_dir(out, git_repository_workdir(repo), path.ptr, name))) < 0) {
					/* printf("something went wrong\n"); */
					goto out;
				}
				found = 1;
				break;
			}
			/* printf("NOT MATCHED\n"); */
		}
	}
out:

	/* printf("git_worktree_lookup() found %d\n", found); */
	/* printf("git_worktree_lookup() error %d\n", error); */
	if (error < 0) {
		/* printf("git_worktree_lookup() exists with %d\n", error); */
		return error;
	}
	/* printf("git_worktree_lookup() exists with %d\n", (found == 0) ? -1 : 0); */
	return (found == 0) ? -1 : 0;
}

int git_worktree_open_from_repository(git_worktree **out, git_repository *repo)
{
	git_str parent = GIT_STR_INIT;
	const char *gitdir, *commondir;
	char *name = NULL;
	int error = 0;

	if (!git_repository_is_worktree(repo)) {
		git_error_set(GIT_ERROR_WORKTREE, "cannot open worktree of a non-worktree repo");
		error = -1;
		goto out;
	}

	gitdir = git_repository_path(repo);
	commondir = git_repository_commondir(repo);

	if ((error = git_fs_path_prettify_dir(&parent, "..", commondir)) < 0)
		goto out;

	/* The name is defined by the last component in '.git/worktree/%s' */
	/* TODO this is actually wrong, it needs to use the matching logic as
	 * well */
	name = git_fs_path_basename(gitdir);

	if ((error = open_worktree_dir(out, parent.ptr, gitdir, name)) < 0)
		goto out;

out:
	git__free(name);
	git_str_dispose(&parent);

	return error;
}

void git_worktree_free(git_worktree *wt)
{
	if (!wt)
		return;

	git__free(wt->commondir_path);
	git__free(wt->worktree_path);
	git__free(wt->gitlink_path);
	git__free(wt->gitdir_path);
	git__free(wt->parent_path);
	git__free(wt->name);
	git__free(wt);
}

int git_worktree_validate(const git_worktree *wt)
{
	int ret;
	git_str gitdir = GIT_STR_INIT;

	GIT_ASSERT_ARG(wt);


	if ((ret = git_str_puts(&gitdir, wt->gitdir_path)) < 0)
		goto out;

	if (!is_worktree_private_directory(&gitdir)) {
		git_error_set(GIT_ERROR_WORKTREE,
			"worktree gitdir ('%s') is not valid",
			wt->gitlink_path);
		ret = GIT_ERROR;
		goto out;
	}

	if (wt->parent_path && !git_fs_path_exists(wt->parent_path)) {
		git_error_set(GIT_ERROR_WORKTREE,
			"worktree parent directory ('%s') does not exist ",
			wt->parent_path);
		ret = GIT_ERROR;
		goto out;
	}

	if (!git_fs_path_exists(wt->commondir_path)) {
		git_error_set(GIT_ERROR_WORKTREE,
			"worktree common directory ('%s') does not exist ",
			wt->commondir_path);
		ret = GIT_ERROR;
		goto out;
	}

	if (!git_fs_path_exists(wt->worktree_path)) {
		git_error_set(GIT_ERROR_WORKTREE,
			"worktree directory '%s' does not exist",
			wt->worktree_path);
		ret = GIT_ERROR;
		goto out;
	}

out:
	git_str_dispose(&gitdir);

	return ret;
}

int git_worktree_add_options_init(git_worktree_add_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(opts, version,
		git_worktree_add_options, GIT_WORKTREE_ADD_OPTIONS_INIT);
	return 0;
}

#ifndef GIT_DEPRECATE_HARD
int git_worktree_add_init_options(git_worktree_add_options *opts,
	unsigned int version)
{
	return git_worktree_add_options_init(opts, version);
}
#endif

/* Name is used:
// * As the dir under `.git/worktrees`
// * As the name of the branch if nothing explicit is given
*/
int git_worktree_add(git_worktree **out, git_repository *repo,
	const char *name, const char *worktree,
	const git_worktree_add_options *opts)
{
	git_str gitdir = GIT_STR_INIT, wddir = GIT_STR_INIT, buf = GIT_STR_INIT;
	git_reference *ref = NULL, *head = NULL;
	git_commit *commit = NULL;
	git_repository *wt = NULL;
	git_checkout_options coopts;
	git_worktree_add_options wtopts = GIT_WORKTREE_ADD_OPTIONS_INIT;
	int err;

	git_str private_subdir_tmp = GIT_STR_INIT;
	git_str private_subdir_name = GIT_STR_INIT;

	/* Make sure to adapt the length of `suffixstrbuf` below if you change
	 * the type here */
	int8_t suffix = 1;
	/* uint8 maximum value is 255, which is 3 characters, plus one for the
	   null bytes */
	char suffixstrbuf[4];

	GIT_ERROR_CHECK_VERSION(
		opts, GIT_WORKTREE_ADD_OPTIONS_VERSION, "git_worktree_add_options");

	GIT_ASSERT_ARG(out);
	GIT_ASSERT_ARG(repo);
	GIT_ASSERT_ARG(name);
	GIT_ASSERT_ARG(worktree);

	*out = NULL;

	if (opts)
		memcpy(&wtopts, opts, sizeof(wtopts));

	memcpy(&coopts, &wtopts.checkout_options, sizeof(coopts));

	if (wtopts.ref) {
		/* printf("we actually have a ref to check out!\n"); */
		if (!git_reference_is_branch(wtopts.ref)) {
			git_error_set(GIT_ERROR_WORKTREE, "reference is not a branch");
			err = -1;
			goto out;
		}

		if (git_branch_is_checked_out(wtopts.ref)) {
			git_error_set(GIT_ERROR_WORKTREE, "reference is already checked out");
			err = -1;
			goto out;
		}
		/* printf("and the ref looks valid!\n"); */
	}

	/* Create gitdir directory ".git/worktrees/$(basename <name>)", with
	 * additional numbered suffixes in case of conflicts
	 * */
	if ((err = git_str_joinpath(&gitdir, repo->commondir, "worktrees")) < 0)
		goto out;
	if (!git_fs_path_exists(gitdir.ptr))
		if ((err = git_futils_mkdir(gitdir.ptr, 0755, GIT_MKDIR_EXCL)) < 0)
			goto out;

	if ((err = git_fs_path_basename_r(&private_subdir_name, worktree)) < 0)
		goto out;

	if ((err = git_str_puts(&private_subdir_tmp, gitdir.ptr)) < 0)
		goto out;

	while (true) {
		if ((err = git_str_joinpath(&private_subdir_tmp, gitdir.ptr, private_subdir_name.ptr)) < 0)
			goto out;
		if (git_fs_path_exists(private_subdir_tmp.ptr)) {
			if ((err = sprintf(suffixstrbuf, "%d", suffix)) < 0)
				goto out;
			if ((err = git_str_puts(&private_subdir_name, suffixstrbuf)) < 0)
				goto out;
			suffix++;
		} else {
			break;
		}
	}

	if ((err = git_str_sets(&gitdir, private_subdir_tmp.ptr)) < 0)
		goto out;

	if ((err = git_futils_mkdir(gitdir.ptr, 0755, GIT_MKDIR_EXCL)) < 0)
		goto out;
	if ((err = git_fs_path_prettify_dir(&gitdir, gitdir.ptr, NULL)) < 0)
		goto out;

	/* Create worktree work dir */
	/* not sure about just yoloing the creation here with _PATH. maybe
	 * iterate?
	 *
	 * Note that EXCL is not possilbe here, as a subdirectory may be shared
	 * by multiple worktrees and therefore already exist
	 */
	if ((err = git_futils_mkdir(worktree, 0755, GIT_MKDIR_EXCL | GIT_MKDIR_PATH)) < 0)
		goto out;
	if ((err = git_fs_path_prettify_dir(&wddir, worktree, NULL)) < 0)
		goto out;

	if (wtopts.lock) {
		int fd;

		if ((err = git_str_joinpath(&buf, gitdir.ptr, "locked")) < 0)
			goto out;

		if ((fd = p_creat(buf.ptr, 0644)) < 0) {
			err = fd;
			goto out;
		}

		p_close(fd);
		git_str_clear(&buf);
	}

	/* Create worktree .git file */
	if ((err = git_str_printf(&buf, "gitdir: %s\n", gitdir.ptr)) < 0)
		goto out;
	if ((err = write_wtfile(wddir.ptr, ".git", &buf)) < 0)
		goto out;


	/* Create gitdir files */
	if ((err = git_fs_path_prettify_dir(&buf, repo->commondir, NULL) < 0)
	    || (err = git_str_putc(&buf, '\n')) < 0
	    || (err = write_wtfile(gitdir.ptr, "commondir", &buf)) < 0)
		goto out;
	if ((err = git_str_joinpath(&buf, wddir.ptr, ".git")) < 0
	    || (err = git_str_putc(&buf, '\n')) < 0
	    || (err = write_wtfile(gitdir.ptr, "gitdir", &buf)) < 0)
		goto out;


	/* Set up worktree reference */
	if (wtopts.ref) {
		/* printf("copying the given ref\n"); */
		if ((err = git_reference_dup(&ref, wtopts.ref)) < 0)
			goto out;
		/* printf("done copying the given ref\n"); */
	} else {
		if ((err = git_repository_head(&head, repo)) < 0)
			goto out;
		if ((err = git_commit_lookup(&commit, repo, &head->target.oid)) < 0)
			goto out;
		if ((err = git_branch_create(&ref, repo, name, commit, false)) < 0)
			goto out;
	}

	/* Set worktree's HEAD */
	/* printf("we are now making sure the head points to the given ref\n"); */
	if ((err = git_repository_create_head(gitdir.ptr, git_reference_name(ref))) < 0)
		goto out;
	if ((err = git_repository_open(&wt, wddir.ptr)) < 0)
		goto out;

	/* printf("git_worktree_add() | gitdir path:    %s\n", wt->gitdir); */
	/* printf("git_worktree_add() | commondir path: %s\n", wt->commondir); */
	/* printf("git_worktree_add() | workdir path:   %s\n", wt->workdir); */

	/* Checkout worktree's HEAD */
	if ((err = git_checkout_head(wt, &coopts)) < 0)
		goto out;
	/* printf("done with pointing the head to the given ref\n"); */

	/* Load result */
	if ((err = git_worktree_lookup(out, repo, worktree)) < 0)
		goto out;

out:
	git_str_dispose(&gitdir);
	git_str_dispose(&wddir);
	git_str_dispose(&buf);
	git_str_dispose(&private_subdir_name);
	git_str_dispose(&private_subdir_tmp);
	git_reference_free(ref);
	git_reference_free(head);
	git_commit_free(commit);
	git_repository_free(wt);

	/* printf("git_add_worktree() exists with %d\n", err); */

	return err;
}

int git_worktree_lock(git_worktree *wt, const char *reason)
{
	git_str buf = GIT_STR_INIT, path = GIT_STR_INIT;
	int error;

	GIT_ASSERT_ARG(wt);

	if ((error = git_worktree_is_locked(NULL, wt)) < 0)
		goto out;
	if (error) {
		error = GIT_ELOCKED;
		goto out;
	}

	if ((error = git_str_joinpath(&path, wt->gitdir_path, "locked")) < 0)
		goto out;

	if (reason)
		git_str_attach_notowned(&buf, reason, strlen(reason));

	if ((error = git_futils_writebuffer(&buf, path.ptr, O_CREAT|O_EXCL|O_WRONLY, 0644)) < 0)
		goto out;

	wt->locked = 1;

out:
	git_str_dispose(&path);

	return error;
}

int git_worktree_unlock(git_worktree *wt)
{
	git_str path = GIT_STR_INIT;
	int error;

	GIT_ASSERT_ARG(wt);

	if ((error = git_worktree_is_locked(NULL, wt)) < 0)
		return error;
	if (!error)
		return 1;

	if (git_str_joinpath(&path, wt->gitdir_path, "locked") < 0)
		return -1;

	if (p_unlink(path.ptr) != 0) {
		git_str_dispose(&path);
		return -1;
	}

	wt->locked = 0;

	git_str_dispose(&path);

	return 0;
}

static int git_worktree__is_locked(git_str *reason, const git_worktree *wt)
{
	git_str path = GIT_STR_INIT;
	int error, locked;

	GIT_ASSERT_ARG(wt);

	if (reason)
		git_str_clear(reason);

	if ((error = git_str_joinpath(&path, wt->gitdir_path, "locked")) < 0)
		goto out;
	locked = git_fs_path_exists(path.ptr);
	if (locked && reason &&
	    (error = git_futils_readbuffer(reason, path.ptr)) < 0)
		goto out;

	error = locked;
out:
	git_str_dispose(&path);

	return error;
}

int git_worktree_is_locked(git_buf *reason, const git_worktree *wt)
{
	git_str str = GIT_STR_INIT;
	int error = 0;

	if (reason && (error = git_buf_tostr(&str, reason)) < 0)
		return error;

	error = git_worktree__is_locked(reason ? &str : NULL, wt);

	if (error >= 0 && reason) {
		if (git_buf_fromstr(reason, &str) < 0)
			error = -1;
	}

	git_str_dispose(&str);
	return error;
}

const char *git_worktree_name(const git_worktree *wt)
{
	/* TODO this should return the PATH */
	GIT_ASSERT_ARG_WITH_RETVAL(wt, NULL);
	return wt->name;
}

const char *git_worktree_path(const git_worktree *wt)
{
	GIT_ASSERT_ARG_WITH_RETVAL(wt, NULL);
	return wt->worktree_path;
}

int git_worktree_prune_options_init(
	git_worktree_prune_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(opts, version,
		git_worktree_prune_options, GIT_WORKTREE_PRUNE_OPTIONS_INIT);
	return 0;
}

#ifndef GIT_DEPRECATE_HARD
int git_worktree_prune_init_options(git_worktree_prune_options *opts,
	unsigned int version)
{
	return git_worktree_prune_options_init(opts, version);
}
#endif

int git_worktree_is_prunable(git_worktree *wt,
	git_worktree_prune_options *opts)
{
	git_worktree_prune_options popts = GIT_WORKTREE_PRUNE_OPTIONS_INIT;

	GIT_ERROR_CHECK_VERSION(
		opts, GIT_WORKTREE_PRUNE_OPTIONS_VERSION,
		"git_worktree_prune_options");

	if (opts)
		memcpy(&popts, opts, sizeof(popts));

	if ((popts.flags & GIT_WORKTREE_PRUNE_LOCKED) == 0) {
		git_str reason = GIT_STR_INIT;
		int error;

		if ((error = git_worktree__is_locked(&reason, wt)) < 0)
			return error;

		if (error) {
			if (!reason.size)
				git_str_attach_notowned(&reason, "no reason given", 15);
			git_error_set(GIT_ERROR_WORKTREE, "not pruning locked working tree: '%s'", reason.ptr);
			git_str_dispose(&reason);
			return 0;
		}
	}

	if ((popts.flags & GIT_WORKTREE_PRUNE_VALID) == 0 &&
	    git_worktree_validate(wt) == 0) {
		git_error_set(GIT_ERROR_WORKTREE, "not pruning valid working tree");
		return 0;
	}

	return 1;
}

int git_worktree_prune(git_worktree *wt,
	git_worktree_prune_options *opts)
{
	git_worktree_prune_options popts = GIT_WORKTREE_PRUNE_OPTIONS_INIT;
	git_str path = GIT_STR_INIT;
	char *wtpath;
	int err;

	GIT_ERROR_CHECK_VERSION(
		opts, GIT_WORKTREE_PRUNE_OPTIONS_VERSION,
		"git_worktree_prune_options");

	if (opts)
		memcpy(&popts, opts, sizeof(popts));

	if (!git_worktree_is_prunable(wt, &popts)) {
		err = -1;
		goto out;
	}

	/* Delete gitdir in parent repository */
	/* TODO wrong, this cannot just be the name. do the lookup! */
	if ((err = git_str_join3(&path, '/', wt->commondir_path, "worktrees", wt->name)) < 0)
		goto out;
	if (!git_fs_path_exists(path.ptr))
	{
		git_error_set(GIT_ERROR_WORKTREE, "worktree gitdir '%s' does not exist", path.ptr);
		err = -1;
		goto out;
	}
	if ((err = git_futils_rmdir_r(path.ptr, NULL, GIT_RMDIR_REMOVE_FILES)) < 0)
		goto out;

	/* Skip deletion of the actual working tree if it does
	 * not exist or deletion was not requested */
	if ((popts.flags & GIT_WORKTREE_PRUNE_WORKING_TREE) == 0 ||
		!git_fs_path_exists(wt->gitlink_path))
	{
		goto out;
	}

	if ((wtpath = git_fs_path_dirname(wt->gitlink_path)) == NULL)
		goto out;
	git_str_attach(&path, wtpath, 0);
	if (!git_fs_path_exists(path.ptr))
	{
		git_error_set(GIT_ERROR_WORKTREE, "working tree '%s' does not exist", path.ptr);
		err = -1;
		goto out;
	}
	if ((err = git_futils_rmdir_r(path.ptr, NULL, GIT_RMDIR_REMOVE_FILES)) < 0)
		goto out;

out:
	git_str_dispose(&path);

	return err;
}
