

Ref https://github.com/libgit2/libgit2/issues/6327

It all became a bit more compliacted than I initially thought.

The main issue is that the `name` parameter to `git_add_worktree()` is used for
the directory name in `.git/worktrees` (which will be changed as discussed)
**and** for the name of the branch to check out. The second needs to be kept to
ensure backwards compatibility, the first will be changed to use basename plus
the suffix on conflicts.

As `name` is now only used for the branch name, it's actually redundant with
`opts.ref`.

Option 1: We change the behavior as follows:

* If `name` is given, we use the "old-style" functionality. This means that having
  a slash in name will still break.
* If `name` is `NULL`, we use the new automatic generation of the name under
  `.git/worktrees` using `basename(path)` and we require that `opts.ref` is set.

Option 2: We change the behavior as follows:

* We use the new automatic generation also when `name` is non-`NULL`. In this
  case, the `name` parameter only specifies the branch name. It's completely
  ignored when `opts.ref` is set.


---

In here, I will not refer to a "worktree name" any more, as there isn't really
such a thing. There is a path (i.e. whereever the working tree is), a private
subdirectory under `.git/worktrees`, and the name of the **branch** that is
checked out in the worktree. I'll refer to the directory in `.git/worktrees` as
the worktree's "private directory" (similar to `git-worktree(1)`. It's
effectively an opaquely named directory, the name does not really matter and
cannot be used to map back to the worktree's path.

This means that the "reverse mapping" from a worktree name to its path is no
longer trivial: Instead, we need to look through all subdirectories of
`.git/worktrees`, check whether there is one with a matching path in `gitdir`
and use that.

My first thought was to treat the `name` as the subdirectory of a worktree
inside its parent directory, but this is not valid, as a worktree can also live
outside of its parents tree.

## About git_worktree_lookup()

`git_worktree_lookup` is supposed to look up a worktree by its "name" and is
therefore no longer meaningful.

In the past, this just worked because the path/name was codified inside
`.git/worktrees`. This can unfortunately not work any more. Imagine the
following scenario:

* Create new worktree with path `foo/bar`. Its private directory will be in
  `.git/worktrees/foo`.
* Create new worktree with path `foo`. Its private directory will be in
  `.git/worktrees/foo1`, as `foo` already exists.
* `git_worktree_lookup("foo")` would have returned the `foo` worktree.

This scenario can only happen for users of the new functionality, as the
creation of `foo/bar` is currently failing. It *is* possible to create
worktrees with slashes using `git(1)`, but their private directories have the
same name as the directories now created using `git_worktree_add()`.

Option 1:

The function stays as it is.

Option 2:

Therefore, `git_worktree_lookup()` will be deprecated. The current
functionality will be kept, as it's still valid for repositories only handled
via libgit2, and not worse than before for repositories handled with `git(1)`.
Instead of using `git_worktree_lookup()`, users should look up worktrees using
their path, with a list of all paths obtained via `git_worktree_list()`.

Option 3:

Like Option 3, but a new method will be introduced that looks up a worktree
by path. I don't see the need, as such a path can also be directly opened via
`git_worktree_open_from_repository()`.

Option 4:

For backwards compatibility, the function now returns the first worktree that
has a path ending with the `name` parameter. Note that this does not work
with invalid worktrees: If `gitdir` was pointing to something invalid (i.e. not
a worktree), the call to `git_worktree_lookup()` still succeeded before, as the
`gitdir` file was never read. As it's now used to get the actual worktree,
such calls fail. What's the inpact on backwards compatibility? I'd even consider
the new behavior better. Before `git_worktree_lookup()` just returned an invalid
`git_worktree` struct that will fail as soon as it's passed to `git_repository_open_from_worktree()`.

---

`gitdir` inside `.git/worktrees` will point to the `.git` directory inside that
worktree. When doing a lookup, we could do a reverse-verify as well. This means
we see what worktree path `.git/worktree/{handle}/gitdir` links to, and then
verify that the worktree's `.git` properly links back to the handle. If
verification fails, we can consider the git repository corrupted and skip the
worktree. Q: Is there a way to return a warning?

The `name` member of the `git_worktree` struct is now no longer used, as it's
redundant with `gitdir_path`.

---


# Tests





This functionality is encapsulated in a new internal function:

`char *git_worktree__get_path_from_handle(const char *handle)`

It expects the handle of the worktree, looks it up under `.git/worktrees`,
finds the actual worktree paths, does the reverse verification as described
above. It internally uses `git_worktree__read_link()` to read `gitdir`.

This functionality is encaspulated in a new function that maps from a path to
its worktree path.

* `is_worktree_dir()` is now called `is_worktree_handle()` to better reflect
  its usage. As the function is only used internally, this does not affect
  existing usage.

* Throughout `worktree.c`, `name` is used for the path to the worktree. The
  word "name" will be kept in external interfaces, while the semantics are
  changed to mean "worktree path".

The change affects the following functions in `worktree.c` (effectively all
functions that look for worktrees using their "name"):

* `git_worktree_list()`: It returned all subdirectories of `.git/worktrees`
  while assuming that the filename is the same as the worktree. Now, it returns
  the paths of all worktrees found in `.git/worktrees` instead, after doing the
  proper mapping with `*git_worktree__get_path_from_handle()`.

* `git_worktree_lookup()`: Instead of just checking `.git/worktrees/{name}`, it
  now also iterates through all subdirectories of `.git/worktree/{name}` and
  does the lookup.

It's backwards compatible, but a few considerations:

* `git_worktree_lookup()` is no longer `O(1)`. It's now `O(n)` with the number
  of worktrees in a repository. I consider this acceptable as the number of
  worktrees is most likely quite low and this would only matter when going into
  the thousands of worktrees.

Documentation changes:

* The documentation now states that the `name` paramter to `git_worktree_add()`
  only specifies the name of the branch. It also semantically conflicts with
  the usage of a non-NULL `git_reference` member of a potential
  `git_worktree_add_options` struct passed as the `opts` parameter, as the
  latter would override `name` and make `name` completely meaningless.
