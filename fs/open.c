// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/string.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fsnotify.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/personality.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>
#include <linux/rcupdate.h>
#include <linux/audit.h>
#include <linux/falloc.h>
#include <linux/fs_struct.h>
#include <linux/dnotify.h>
#include <linux/compat.h>
#include <linux/mnt_idmapping.h>
#include <linux/filelock.h>

#include "internal.h"

int do_truncate(struct mnt_idmap *idmap, struct dentry *dentry,
		loff_t length, unsigned int time_attrs, struct file *filp)
{
	int ret;
	struct iattr newattrs;

	/* Not pretty: "inode->i_size" shouldn't really be signed. But it is. */
	if (length < 0)
		return -EINVAL;

	newattrs.ia_size = length;
	newattrs.ia_valid = ATTR_SIZE | time_attrs;
	if (filp) {
		newattrs.ia_file = filp;
		newattrs.ia_valid |= ATTR_FILE;
	}

	/* Remove suid, sgid, and file capabilities on truncate too */
	ret = dentry_needs_remove_privs(idmap, dentry);
	if (ret < 0)
		return ret;
	if (ret)
		newattrs.ia_valid |= ret | ATTR_FORCE;

	ret = inode_lock_killable(dentry->d_inode);
	if (ret)
		return ret;

	/* Note any delegations or leases have already been broken: */
	ret = notify_change(idmap, dentry, &newattrs, NULL);
	inode_unlock(dentry->d_inode);
	return ret;
}

int vfs_truncate(const struct path *path, loff_t length)
{
	struct mnt_idmap *idmap;
	struct inode *inode;
	int error;

	inode = path->dentry->d_inode;

	/* For directories it's -EISDIR, for other non-regulars - -EINVAL */
	if (S_ISDIR(inode->i_mode))
		return -EISDIR;
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	idmap = mnt_idmap(path->mnt);
	error = inode_permission(idmap, inode, MAY_WRITE);
	if (error)
		return error;

	error = fsnotify_truncate_perm(path, length);
	if (error)
		return error;

	error = mnt_want_write(path->mnt);
	if (error)
		return error;

	error = -EPERM;
	if (IS_APPEND(inode))
		goto mnt_drop_write_and_out;

	error = get_write_access(inode);
	if (error)
		goto mnt_drop_write_and_out;

	/*
	 * Make sure that there are no leases.  get_write_access() protects
	 * against the truncate racing with a lease-granting setlease().
	 */
	error = break_lease(inode, O_WRONLY);
	if (error)
		goto put_write_and_out;

	error = security_path_truncate(path);
	if (!error)
		error = do_truncate(idmap, path->dentry, length, 0, NULL);

put_write_and_out:
	put_write_access(inode);
mnt_drop_write_and_out:
	mnt_drop_write(path->mnt);

	return error;
}
EXPORT_SYMBOL_GPL(vfs_truncate);

int do_sys_truncate(const char __user *pathname, loff_t length)
{
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	struct path path;
	int error;

	if (length < 0)	/* sorry, but loff_t says... */
		return -EINVAL;

retry:
	error = user_path_at(AT_FDCWD, pathname, lookup_flags, &path);
	if (!error) {
		error = vfs_truncate(&path, length);
		path_put(&path);
	}
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE2(truncate, const char __user *, path, long, length)
{
	return do_sys_truncate(path, length);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE2(truncate, const char __user *, path, compat_off_t, length)
{
	return do_sys_truncate(path, length);
}
#endif

int do_ftruncate(struct file *file, loff_t length, int small)
{
	struct inode *inode;
	struct dentry *dentry;
	int error;

	/* explicitly opened as large or we are on 64-bit box */
	if (file->f_flags & O_LARGEFILE)
		small = 0;

	dentry = file->f_path.dentry;
	inode = dentry->d_inode;
	if (!S_ISREG(inode->i_mode) || !(file->f_mode & FMODE_WRITE))
		return -EINVAL;

	/* Cannot ftruncate over 2^31 bytes without large file support */
	if (small && length > MAX_NON_LFS)
		return -EINVAL;

	/* Check IS_APPEND on real upper inode */
	if (IS_APPEND(file_inode(file)))
		return -EPERM;

	error = security_file_truncate(file);
	if (error)
		return error;

	error = fsnotify_truncate_perm(&file->f_path, length);
	if (error)
		return error;

	sb_start_write(inode->i_sb);
	error = do_truncate(file_mnt_idmap(file), dentry, length,
			    ATTR_MTIME | ATTR_CTIME, file);
	sb_end_write(inode->i_sb);

	return error;
}

int do_sys_ftruncate(unsigned int fd, loff_t length, int small)
{
	if (length < 0)
		return -EINVAL;
	CLASS(fd, f)(fd);
	if (fd_empty(f))
		return -EBADF;

	return do_ftruncate(fd_file(f), length, small);
}

SYSCALL_DEFINE2(ftruncate, unsigned int, fd, off_t, length)
{
	return do_sys_ftruncate(fd, length, 1);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE2(ftruncate, unsigned int, fd, compat_off_t, length)
{
	return do_sys_ftruncate(fd, length, 1);
}
#endif

/* LFS versions of truncate are only needed on 32 bit machines */
#if BITS_PER_LONG == 32
SYSCALL_DEFINE2(truncate64, const char __user *, path, loff_t, length)
{
	return do_sys_truncate(path, length);
}

SYSCALL_DEFINE2(ftruncate64, unsigned int, fd, loff_t, length)
{
	return do_sys_ftruncate(fd, length, 0);
}
#endif /* BITS_PER_LONG == 32 */

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_TRUNCATE64)
COMPAT_SYSCALL_DEFINE3(truncate64, const char __user *, pathname,
		       compat_arg_u64_dual(length))
{
	return ksys_truncate(pathname, compat_arg_u64_glue(length));
}
#endif

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_FTRUNCATE64)
COMPAT_SYSCALL_DEFINE3(ftruncate64, unsigned int, fd,
		       compat_arg_u64_dual(length))
{
	return ksys_ftruncate(fd, compat_arg_u64_glue(length));
}
#endif

int vfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	int ret;
	loff_t sum;

	if (offset < 0 || len <= 0)
		return -EINVAL;

	if (mode & ~(FALLOC_FL_MODE_MASK | FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;

	/*
	 * Modes are exclusive, even if that is not obvious from the encoding
	 * as bit masks and the mix with the flag in the same namespace.
	 *
	 * To make things even more complicated, FALLOC_FL_ALLOCATE_RANGE is
	 * encoded as no bit set.
	 */
	switch (mode & FALLOC_FL_MODE_MASK) {
	case FALLOC_FL_ALLOCATE_RANGE:
	case FALLOC_FL_UNSHARE_RANGE:
	case FALLOC_FL_ZERO_RANGE:
		break;
	case FALLOC_FL_PUNCH_HOLE:
		if (!(mode & FALLOC_FL_KEEP_SIZE))
			return -EOPNOTSUPP;
		break;
	case FALLOC_FL_COLLAPSE_RANGE:
	case FALLOC_FL_INSERT_RANGE:
	case FALLOC_FL_WRITE_ZEROES:
		if (mode & FALLOC_FL_KEEP_SIZE)
			return -EOPNOTSUPP;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;

	/*
	 * On append-only files only space preallocation is supported.
	 */
	if ((mode & ~FALLOC_FL_KEEP_SIZE) && IS_APPEND(inode))
		return -EPERM;

	if (IS_IMMUTABLE(inode))
		return -EPERM;

	/*
	 * We cannot allow any fallocate operation on an active swapfile
	 */
	if (IS_SWAPFILE(inode))
		return -ETXTBSY;

	/*
	 * Revalidate the write permissions, in case security policy has
	 * changed since the files were opened.
	 */
	ret = security_file_permission(file, MAY_WRITE);
	if (ret)
		return ret;

	ret = fsnotify_file_area_perm(file, MAY_WRITE, &offset, len);
	if (ret)
		return ret;

	if (S_ISFIFO(inode->i_mode))
		return -ESPIPE;

	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		return -ENODEV;

	/* Check for wraparound */
	if (check_add_overflow(offset, len, &sum))
		return -EFBIG;

	if (sum > inode->i_sb->s_maxbytes)
		return -EFBIG;

	if (!file->f_op->fallocate)
		return -EOPNOTSUPP;

	file_start_write(file);
	ret = file->f_op->fallocate(file, mode, offset, len);

	/*
	 * Create inotify and fanotify events.
	 *
	 * To keep the logic simple always create events if fallocate succeeds.
	 * This implies that events are even created if the file size remains
	 * unchanged, e.g. when using flag FALLOC_FL_KEEP_SIZE.
	 */
	if (ret == 0)
		fsnotify_modify(file);

	file_end_write(file);
	return ret;
}
EXPORT_SYMBOL_GPL(vfs_fallocate);

int ksys_fallocate(int fd, int mode, loff_t offset, loff_t len)
{
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return -EBADF;

	return vfs_fallocate(fd_file(f), mode, offset, len);
}

SYSCALL_DEFINE4(fallocate, int, fd, int, mode, loff_t, offset, loff_t, len)
{
	return ksys_fallocate(fd, mode, offset, len);
}

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_FALLOCATE)
COMPAT_SYSCALL_DEFINE6(fallocate, int, fd, int, mode, compat_arg_u64_dual(offset),
		       compat_arg_u64_dual(len))
{
	return ksys_fallocate(fd, mode, compat_arg_u64_glue(offset),
			      compat_arg_u64_glue(len));
}
#endif

/*
 * access() needs to use the real uid/gid, not the effective uid/gid.
 * We do this by temporarily clearing all FS-related capabilities and
 * switching the fsuid/fsgid around to the real ones.
 *
 * Creating new credentials is expensive, so we try to skip doing it,
 * which we can if the result would match what we already got.
 */
static bool access_need_override_creds(int flags)
{
	const struct cred *cred;

	if (flags & AT_EACCESS)
		return false;

	cred = current_cred();
	if (!uid_eq(cred->fsuid, cred->uid) ||
	    !gid_eq(cred->fsgid, cred->gid))
		return true;

	if (!issecure(SECURE_NO_SETUID_FIXUP)) {
		kuid_t root_uid = make_kuid(cred->user_ns, 0);
		if (!uid_eq(cred->uid, root_uid)) {
			if (!cap_isclear(cred->cap_effective))
				return true;
		} else {
			if (!cap_isidentical(cred->cap_effective,
			    cred->cap_permitted))
				return true;
		}
	}

	return false;
}

static const struct cred *access_override_creds(void)
{
	struct cred *override_cred;

	override_cred = prepare_creds();
	if (!override_cred)
		return NULL;

	/*
	 * XXX access_need_override_creds performs checks in hopes of skipping
	 * this work. Make sure it stays in sync if making any changes in this
	 * routine.
	 */

	override_cred->fsuid = override_cred->uid;
	override_cred->fsgid = override_cred->gid;

	if (!issecure(SECURE_NO_SETUID_FIXUP)) {
		/* Clear the capabilities if we switch to a non-root user */
		kuid_t root_uid = make_kuid(override_cred->user_ns, 0);
		if (!uid_eq(override_cred->uid, root_uid))
			cap_clear(override_cred->cap_effective);
		else
			override_cred->cap_effective =
				override_cred->cap_permitted;
	}

	/*
	 * The new set of credentials can *only* be used in
	 * task-synchronous circumstances, and does not need
	 * RCU freeing, unless somebody then takes a separate
	 * reference to it.
	 *
	 * NOTE! This is _only_ true because this credential
	 * is used purely for override_creds() that installs
	 * it as the subjective cred. Other threads will be
	 * accessing ->real_cred, not the subjective cred.
	 *
	 * If somebody _does_ make a copy of this (using the
	 * 'get_current_cred()' function), that will clear the
	 * non_rcu field, because now that other user may be
	 * expecting RCU freeing. But normal thread-synchronous
	 * cred accesses will keep things non-racy to avoid RCU
	 * freeing.
	 */
	override_cred->non_rcu = 1;
	return override_creds(override_cred);
}

static int do_faccessat(int dfd, const char __user *filename, int mode, int flags)
{
	struct path path;
	struct inode *inode;
	int res;
	unsigned int lookup_flags = LOOKUP_FOLLOW;
	const struct cred *old_cred = NULL;

	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;

	if (flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
		return -EINVAL;

	if (flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags &= ~LOOKUP_FOLLOW;
	if (flags & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;

	if (access_need_override_creds(flags)) {
		old_cred = access_override_creds();
		if (!old_cred)
			return -ENOMEM;
	}

retry:
	res = user_path_at(dfd, filename, lookup_flags, &path);
	if (res)
		goto out;

	inode = d_backing_inode(path.dentry);

	if ((mode & MAY_EXEC) && S_ISREG(inode->i_mode)) {
		/*
		 * MAY_EXEC on regular files is denied if the fs is mounted
		 * with the "noexec" flag.
		 */
		res = -EACCES;
		if (path_noexec(&path))
			goto out_path_release;
	}

	res = inode_permission(mnt_idmap(path.mnt), inode, mode | MAY_ACCESS);
	/* SuS v2 requires we report a read only fs too */
	if (res || !(mode & S_IWOTH) || special_file(inode->i_mode))
		goto out_path_release;
	/*
	 * This is a rare case where using __mnt_is_readonly()
	 * is OK without a mnt_want/drop_write() pair.  Since
	 * no actual write to the fs is performed here, we do
	 * not need to telegraph to that to anyone.
	 *
	 * By doing this, we accept that this access is
	 * inherently racy and know that the fs may change
	 * state before we even see this result.
	 */
	if (__mnt_is_readonly(path.mnt))
		res = -EROFS;

out_path_release:
	path_put(&path);
	if (retry_estale(res, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	if (old_cred)
		put_cred(revert_creds(old_cred));

	return res;
}

SYSCALL_DEFINE3(faccessat, int, dfd, const char __user *, filename, int, mode)
{
	return do_faccessat(dfd, filename, mode, 0);
}

SYSCALL_DEFINE4(faccessat2, int, dfd, const char __user *, filename, int, mode,
		int, flags)
{
	return do_faccessat(dfd, filename, mode, flags);
}

SYSCALL_DEFINE2(access, const char __user *, filename, int, mode)
{
	return do_faccessat(AT_FDCWD, filename, mode, 0);
}

SYSCALL_DEFINE1(chdir, const char __user *, filename)
{
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
retry:
	error = user_path_at(AT_FDCWD, filename, lookup_flags, &path);
	if (error)
		goto out;

	error = path_permission(&path, MAY_EXEC | MAY_CHDIR);
	if (error)
		goto dput_and_out;

	set_fs_pwd(current->fs, &path);

dput_and_out:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

SYSCALL_DEFINE1(fchdir, unsigned int, fd)
{
	CLASS(fd_raw, f)(fd);
	int error;

	if (fd_empty(f))
		return -EBADF;

	if (!d_can_lookup(fd_file(f)->f_path.dentry))
		return -ENOTDIR;

	error = file_permission(fd_file(f), MAY_EXEC | MAY_CHDIR);
	if (!error)
		set_fs_pwd(current->fs, &fd_file(f)->f_path);
	return error;
}

SYSCALL_DEFINE1(chroot, const char __user *, filename)
{
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_FOLLOW | LOOKUP_DIRECTORY;
retry:
	error = user_path_at(AT_FDCWD, filename, lookup_flags, &path);
	if (error)
		goto out;

	error = path_permission(&path, MAY_EXEC | MAY_CHDIR);
	if (error)
		goto dput_and_out;

	error = -EPERM;
	if (!ns_capable(current_user_ns(), CAP_SYS_CHROOT))
		goto dput_and_out;
	error = security_path_chroot(&path);
	if (error)
		goto dput_and_out;

	set_fs_root(current->fs, &path);
	error = 0;
dput_and_out:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

int chmod_common(const struct path *path, umode_t mode)
{
	struct inode *inode = path->dentry->d_inode;
	struct inode *delegated_inode = NULL;
	struct iattr newattrs;
	int error;

	error = mnt_want_write(path->mnt);
	if (error)
		return error;
retry_deleg:
	error = inode_lock_killable(inode);
	if (error)
		goto out_mnt_unlock;
	error = security_path_chmod(path, mode);
	if (error)
		goto out_unlock;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	error = notify_change(mnt_idmap(path->mnt), path->dentry,
			      &newattrs, &delegated_inode);
out_unlock:
	inode_unlock(inode);
	if (delegated_inode) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry_deleg;
	}
out_mnt_unlock:
	mnt_drop_write(path->mnt);
	return error;
}

int vfs_fchmod(struct file *file, umode_t mode)
{
	audit_file(file);
	return chmod_common(&file->f_path, mode);
}

SYSCALL_DEFINE2(fchmod, unsigned int, fd, umode_t, mode)
{
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return -EBADF;

	return vfs_fchmod(fd_file(f), mode);
}

static int do_fchmodat(int dfd, const char __user *filename, umode_t mode,
		       unsigned int flags)
{
	struct path path;
	int error;
	unsigned int lookup_flags;

	if (unlikely(flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)))
		return -EINVAL;

	lookup_flags = (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	if (flags & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;

retry:
	error = user_path_at(dfd, filename, lookup_flags, &path);
	if (!error) {
		error = chmod_common(&path, mode);
		path_put(&path);
		if (retry_estale(error, lookup_flags)) {
			lookup_flags |= LOOKUP_REVAL;
			goto retry;
		}
	}
	return error;
}

SYSCALL_DEFINE4(fchmodat2, int, dfd, const char __user *, filename,
		umode_t, mode, unsigned int, flags)
{
	return do_fchmodat(dfd, filename, mode, flags);
}

SYSCALL_DEFINE3(fchmodat, int, dfd, const char __user *, filename,
		umode_t, mode)
{
	return do_fchmodat(dfd, filename, mode, 0);
}

SYSCALL_DEFINE2(chmod, const char __user *, filename, umode_t, mode)
{
	return do_fchmodat(AT_FDCWD, filename, mode, 0);
}

/*
 * Check whether @kuid is valid and if so generate and set vfsuid_t in
 * ia_vfsuid.
 *
 * Return: true if @kuid is valid, false if not.
 */
static inline bool setattr_vfsuid(struct iattr *attr, kuid_t kuid)
{
	if (!uid_valid(kuid))
		return false;
	attr->ia_valid |= ATTR_UID;
	attr->ia_vfsuid = VFSUIDT_INIT(kuid);
	return true;
}

/*
 * Check whether @kgid is valid and if so generate and set vfsgid_t in
 * ia_vfsgid.
 *
 * Return: true if @kgid is valid, false if not.
 */
static inline bool setattr_vfsgid(struct iattr *attr, kgid_t kgid)
{
	if (!gid_valid(kgid))
		return false;
	attr->ia_valid |= ATTR_GID;
	attr->ia_vfsgid = VFSGIDT_INIT(kgid);
	return true;
}

int chown_common(const struct path *path, uid_t user, gid_t group)
{
	struct mnt_idmap *idmap;
	struct user_namespace *fs_userns;
	struct inode *inode = path->dentry->d_inode;
	struct inode *delegated_inode = NULL;
	int error;
	struct iattr newattrs;
	kuid_t uid;
	kgid_t gid;

	uid = make_kuid(current_user_ns(), user);
	gid = make_kgid(current_user_ns(), group);

	idmap = mnt_idmap(path->mnt);
	fs_userns = i_user_ns(inode);

retry_deleg:
	newattrs.ia_vfsuid = INVALID_VFSUID;
	newattrs.ia_vfsgid = INVALID_VFSGID;
	newattrs.ia_valid =  ATTR_CTIME;
	if ((user != (uid_t)-1) && !setattr_vfsuid(&newattrs, uid))
		return -EINVAL;
	if ((group != (gid_t)-1) && !setattr_vfsgid(&newattrs, gid))
		return -EINVAL;
	error = inode_lock_killable(inode);
	if (error)
		return error;
	if (!S_ISDIR(inode->i_mode))
		newattrs.ia_valid |= ATTR_KILL_SUID | ATTR_KILL_PRIV |
				     setattr_should_drop_sgid(idmap, inode);
	/* Continue to send actual fs values, not the mount values. */
	error = security_path_chown(
		path,
		from_vfsuid(idmap, fs_userns, newattrs.ia_vfsuid),
		from_vfsgid(idmap, fs_userns, newattrs.ia_vfsgid));
	if (!error)
		error = notify_change(idmap, path->dentry, &newattrs,
				      &delegated_inode);
	inode_unlock(inode);
	if (delegated_inode) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry_deleg;
	}
	return error;
}

int do_fchownat(int dfd, const char __user *filename, uid_t user, gid_t group,
		int flag)
{
	struct path path;
	int error = -EINVAL;
	int lookup_flags;

	if ((flag & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) != 0)
		goto out;

	lookup_flags = (flag & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW;
	if (flag & AT_EMPTY_PATH)
		lookup_flags |= LOOKUP_EMPTY;
retry:
	error = user_path_at(dfd, filename, lookup_flags, &path);
	if (error)
		goto out;
	error = mnt_want_write(path.mnt);
	if (error)
		goto out_release;
	error = chown_common(&path, user, group);
	mnt_drop_write(path.mnt);
out_release:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
out:
	return error;
}

SYSCALL_DEFINE5(fchownat, int, dfd, const char __user *, filename, uid_t, user,
		gid_t, group, int, flag)
{
	return do_fchownat(dfd, filename, user, group, flag);
}

SYSCALL_DEFINE3(chown, const char __user *, filename, uid_t, user, gid_t, group)
{
	return do_fchownat(AT_FDCWD, filename, user, group, 0);
}

SYSCALL_DEFINE3(lchown, const char __user *, filename, uid_t, user, gid_t, group)
{
	return do_fchownat(AT_FDCWD, filename, user, group,
			   AT_SYMLINK_NOFOLLOW);
}

int vfs_fchown(struct file *file, uid_t user, gid_t group)
{
	int error;

	error = mnt_want_write_file(file);
	if (error)
		return error;
	audit_file(file);
	error = chown_common(&file->f_path, user, group);
	mnt_drop_write_file(file);
	return error;
}

int ksys_fchown(unsigned int fd, uid_t user, gid_t group)
{
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return -EBADF;

	return vfs_fchown(fd_file(f), user, group);
}

SYSCALL_DEFINE3(fchown, unsigned int, fd, uid_t, user, gid_t, group)
{
	return ksys_fchown(fd, user, group);
}

static inline int file_get_write_access(struct file *f)
{
	int error;

	error = get_write_access(f->f_inode);
	if (unlikely(error))
		return error;
	error = mnt_get_write_access(f->f_path.mnt);
	if (unlikely(error))
		goto cleanup_inode;
	if (unlikely(f->f_mode & FMODE_BACKING)) {
		error = mnt_get_write_access(backing_file_user_path(f)->mnt);
		if (unlikely(error))
			goto cleanup_mnt;
	}
	return 0;

cleanup_mnt:
	mnt_put_write_access(f->f_path.mnt);
cleanup_inode:
	put_write_access(f->f_inode);
	return error;
}

static int do_dentry_open(struct file *f,
			  int (*open)(struct inode *, struct file *))
{
	static const struct file_operations empty_fops = {};
	struct inode *inode = f->f_path.dentry->d_inode;
	int error;

	path_get(&f->f_path);
	f->f_inode = inode;
	f->f_mapping = inode->i_mapping;
	f->f_wb_err = filemap_sample_wb_err(f->f_mapping);
	f->f_sb_err = file_sample_sb_err(f);

	if (unlikely(f->f_flags & O_PATH)) {
		f->f_mode = FMODE_PATH | FMODE_OPENED;
		file_set_fsnotify_mode(f, FMODE_NONOTIFY);
		f->f_op = &empty_fops;
		return 0;
	}

	if ((f->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ) {
		i_readcount_inc(inode);
	} else if (f->f_mode & FMODE_WRITE && !special_file(inode->i_mode)) {
		error = file_get_write_access(f);
		if (unlikely(error))
			goto cleanup_file;
		f->f_mode |= FMODE_WRITER;
	}

	/* POSIX.1-2008/SUSv4 Section XSI 2.9.7 */
	if (S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode))
		f->f_mode |= FMODE_ATOMIC_POS;

	f->f_op = fops_get(inode->i_fop);
	if (WARN_ON(!f->f_op)) {
		error = -ENODEV;
		goto cleanup_all;
	}

	error = security_file_open(f);
	if (error)
		goto cleanup_all;

	/*
	 * Call fsnotify open permission hook and set FMODE_NONOTIFY_* bits
	 * according to existing permission watches.
	 * If FMODE_NONOTIFY mode was already set for an fanotify fd or for a
	 * pseudo file, this call will not change the mode.
	 */
	error = fsnotify_open_perm_and_set_mode(f);
	if (error)
		goto cleanup_all;

	error = break_lease(file_inode(f), f->f_flags);
	if (error)
		goto cleanup_all;

	/* normally all 3 are set; ->open() can clear them if needed */
	f->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
	if (!open)
		open = f->f_op->open;
	if (open) {
		error = open(inode, f);
		if (error)
			goto cleanup_all;
	}
	f->f_mode |= FMODE_OPENED;
	if ((f->f_mode & FMODE_READ) &&
	     likely(f->f_op->read || f->f_op->read_iter))
		f->f_mode |= FMODE_CAN_READ;
	if ((f->f_mode & FMODE_WRITE) &&
	     likely(f->f_op->write || f->f_op->write_iter))
		f->f_mode |= FMODE_CAN_WRITE;
	if ((f->f_mode & FMODE_LSEEK) && !f->f_op->llseek)
		f->f_mode &= ~FMODE_LSEEK;
	if (f->f_mapping->a_ops && f->f_mapping->a_ops->direct_IO)
		f->f_mode |= FMODE_CAN_ODIRECT;

	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);
	f->f_iocb_flags = iocb_flags(f);

	file_ra_state_init(&f->f_ra, f->f_mapping->host->i_mapping);

	if ((f->f_flags & O_DIRECT) && !(f->f_mode & FMODE_CAN_ODIRECT))
		return -EINVAL;

	/*
	 * XXX: Huge page cache doesn't support writing yet. Drop all page
	 * cache for this file before processing writes.
	 */
	if (f->f_mode & FMODE_WRITE) {
		/*
		 * Depends on full fence from get_write_access() to synchronize
		 * against collapse_file() regarding i_writecount and nr_thps
		 * updates. Ensures subsequent insertion of THPs into the page
		 * cache will fail.
		 */
		if (filemap_nr_thps(inode->i_mapping)) {
			struct address_space *mapping = inode->i_mapping;

			filemap_invalidate_lock(inode->i_mapping);
			/*
			 * unmap_mapping_range just need to be called once
			 * here, because the private pages is not need to be
			 * unmapped mapping (e.g. data segment of dynamic
			 * shared libraries here).
			 */
			unmap_mapping_range(mapping, 0, 0, 0);
			truncate_inode_pages(mapping, 0);
			filemap_invalidate_unlock(inode->i_mapping);
		}
	}

	return 0;

cleanup_all:
	if (WARN_ON_ONCE(error > 0))
		error = -EINVAL;
	fops_put(f->f_op);
	put_file_access(f);
cleanup_file:
	path_put(&f->f_path);
	f->f_path.mnt = NULL;
	f->f_path.dentry = NULL;
	f->f_inode = NULL;
	return error;
}

/**
 * finish_open - finish opening a file
 * @file: file pointer
 * @dentry: pointer to dentry
 * @open: open callback
 *
 * This can be used to finish opening a file passed to i_op->atomic_open().
 *
 * If the open callback is set to NULL, then the standard f_op->open()
 * filesystem callback is substituted.
 *
 * NB: the dentry reference is _not_ consumed.  If, for example, the dentry is
 * the return value of d_splice_alias(), then the caller needs to perform dput()
 * on it after finish_open().
 *
 * Returns zero on success or -errno if the open failed.
 */
int finish_open(struct file *file, struct dentry *dentry,
		int (*open)(struct inode *, struct file *))
{
	BUG_ON(file->f_mode & FMODE_OPENED); /* once it's opened, it's opened */

	file->f_path.dentry = dentry;
	return do_dentry_open(file, open);
}
EXPORT_SYMBOL(finish_open);

/**
 * finish_no_open - finish ->atomic_open() without opening the file
 *
 * @file: file pointer
 * @dentry: dentry or NULL (as returned from ->lookup())
 *
 * This can be used to set the result of a successful lookup in ->atomic_open().
 *
 * NB: unlike finish_open() this function does consume the dentry reference and
 * the caller need not dput() it.
 *
 * Returns "0" which must be the return value of ->atomic_open() after having
 * called this function.
 */
int finish_no_open(struct file *file, struct dentry *dentry)
{
	file->f_path.dentry = dentry;
	return 0;
}
EXPORT_SYMBOL(finish_no_open);

char *file_path(struct file *filp, char *buf, int buflen)
{
	return d_path(&filp->f_path, buf, buflen);
}
EXPORT_SYMBOL(file_path);

/**
 * vfs_open - open the file at the given path
 * @path: path to open
 * @file: newly allocated file with f_flag initialized
 */
int vfs_open(const struct path *path, struct file *file)
{
	int ret;

	file->f_path = *path;
	ret = do_dentry_open(file, NULL);
	if (!ret) {
		/*
		 * Once we return a file with FMODE_OPENED, __fput() will call
		 * fsnotify_close(), so we need fsnotify_open() here for
		 * symmetry.
		 */
		fsnotify_open(file);
	}
	return ret;
}

struct file *dentry_open(const struct path *path, int flags,
			 const struct cred *cred)
{
	int error;
	struct file *f;

	/* We must always pass in a valid mount pointer. */
	BUG_ON(!path->mnt);

	f = alloc_empty_file(flags, cred);
	if (!IS_ERR(f)) {
		error = vfs_open(path, f);
		if (error) {
			fput(f);
			f = ERR_PTR(error);
		}
	}
	return f;
}
EXPORT_SYMBOL(dentry_open);

struct file *dentry_open_nonotify(const struct path *path, int flags,
				  const struct cred *cred)
{
	struct file *f = alloc_empty_file(flags, cred);
	if (!IS_ERR(f)) {
		int error;

		file_set_fsnotify_mode(f, FMODE_NONOTIFY);
		error = vfs_open(path, f);
		if (error) {
			fput(f);
			f = ERR_PTR(error);
		}
	}
	return f;
}

/**
 * dentry_create - Create and open a file
 * @path: path to create
 * @flags: O_ flags
 * @mode: mode bits for new file
 * @cred: credentials to use
 *
 * Caller must hold the parent directory's lock, and have prepared
 * a negative dentry, placed in @path->dentry, for the new file.
 *
 * Caller sets @path->mnt to the vfsmount of the filesystem where
 * the new file is to be created. The parent directory and the
 * negative dentry must reside on the same filesystem instance.
 *
 * On success, returns a "struct file *". Otherwise a ERR_PTR
 * is returned.
 */
struct file *dentry_create(const struct path *path, int flags, umode_t mode,
			   const struct cred *cred)
{
	struct file *f;
	int error;

	f = alloc_empty_file(flags, cred);
	if (IS_ERR(f))
		return f;

	error = vfs_create(mnt_idmap(path->mnt),
			   d_inode(path->dentry->d_parent),
			   path->dentry, mode, true);
	if (!error)
		error = vfs_open(path, f);

	if (unlikely(error)) {
		fput(f);
		return ERR_PTR(error);
	}
	return f;
}
EXPORT_SYMBOL(dentry_create);

/**
 * kernel_file_open - open a file for kernel internal use
 * @path:	path of the file to open
 * @flags:	open flags
 * @cred:	credentials for open
 *
 * Open a file for use by in-kernel consumers. The file is not accounted
 * against nr_files and must not be installed into the file descriptor
 * table.
 *
 * Return: Opened file on success, an error pointer on failure.
 */
struct file *kernel_file_open(const struct path *path, int flags,
				const struct cred *cred)
{
	struct file *f;
	int error;

	f = alloc_empty_file_noaccount(flags, cred);
	if (IS_ERR(f))
		return f;

	error = vfs_open(path, f);
	if (error) {
		fput(f);
		return ERR_PTR(error);
	}
	return f;
}
EXPORT_SYMBOL_GPL(kernel_file_open);

#define WILL_CREATE(flags)	(flags & (O_CREAT | __O_TMPFILE))
#define O_PATH_FLAGS		(O_DIRECTORY | O_NOFOLLOW | O_PATH | O_CLOEXEC)

inline struct open_how build_open_how(int flags, umode_t mode)
{
	struct open_how how = {
		.flags = flags & VALID_OPEN_FLAGS,
		.mode = mode & S_IALLUGO,
	};

	/* O_PATH beats everything else. */
	if (how.flags & O_PATH)
		how.flags &= O_PATH_FLAGS;
	/* Modes should only be set for create-like flags. */
	if (!WILL_CREATE(how.flags))
		how.mode = 0;
	return how;
}

inline int build_open_flags(const struct open_how *how, struct open_flags *op)
{
	u64 flags = how->flags;
	u64 strip = O_CLOEXEC;
	int lookup_flags = 0;
	int acc_mode = ACC_MODE(flags);

	BUILD_BUG_ON_MSG(upper_32_bits(VALID_OPEN_FLAGS),
			 "struct open_flags doesn't yet handle flags > 32 bits");

	/*
	 * Strip flags that aren't relevant in determining struct open_flags.
	 */
	flags &= ~strip;

	/*
	 * Older syscalls implicitly clear all of the invalid flags or argument
	 * values before calling build_open_flags(), but openat2(2) checks all
	 * of its arguments.
	 */
	if (flags & ~VALID_OPEN_FLAGS)
		return -EINVAL;
	if (how->resolve & ~VALID_RESOLVE_FLAGS)
		return -EINVAL;

	/* Scoping flags are mutually exclusive. */
	if ((how->resolve & RESOLVE_BENEATH) && (how->resolve & RESOLVE_IN_ROOT))
		return -EINVAL;

	/* Deal with the mode. */
	if (WILL_CREATE(flags)) {
		if (how->mode & ~S_IALLUGO)
			return -EINVAL;
		op->mode = how->mode | S_IFREG;
	} else {
		if (how->mode != 0)
			return -EINVAL;
		op->mode = 0;
	}

	/*
	 * Block bugs where O_DIRECTORY | O_CREAT created regular files.
	 * Note, that blocking O_DIRECTORY | O_CREAT here also protects
	 * O_TMPFILE below which requires O_DIRECTORY being raised.
	 */
	if ((flags & (O_DIRECTORY | O_CREAT)) == (O_DIRECTORY | O_CREAT))
		return -EINVAL;

	/* Now handle the creative implementation of O_TMPFILE. */
	if (flags & __O_TMPFILE) {
		/*
		 * In order to ensure programs get explicit errors when trying
		 * to use O_TMPFILE on old kernels we enforce that O_DIRECTORY
		 * is raised alongside __O_TMPFILE.
		 */
		if (!(flags & O_DIRECTORY))
			return -EINVAL;
		if (!(acc_mode & MAY_WRITE))
			return -EINVAL;
	}
	if (flags & O_PATH) {
		/* O_PATH only permits certain other flags to be set. */
		if (flags & ~O_PATH_FLAGS)
			return -EINVAL;
		acc_mode = 0;
	}

	/*
	 * O_SYNC is implemented as __O_SYNC|O_DSYNC.  As many places only
	 * check for O_DSYNC if the need any syncing at all we enforce it's
	 * always set instead of having to deal with possibly weird behaviour
	 * for malicious applications setting only __O_SYNC.
	 */
	if (flags & __O_SYNC)
		flags |= O_DSYNC;

	op->open_flag = flags;

	/* O_TRUNC implies we need access checks for write permissions */
	if (flags & O_TRUNC)
		acc_mode |= MAY_WRITE;

	/* Allow the LSM permission hook to distinguish append
	   access from general write access. */
	if (flags & O_APPEND)
		acc_mode |= MAY_APPEND;

	op->acc_mode = acc_mode;

	op->intent = flags & O_PATH ? 0 : LOOKUP_OPEN;

	if (flags & O_CREAT) {
		op->intent |= LOOKUP_CREATE;
		if (flags & O_EXCL) {
			op->intent |= LOOKUP_EXCL;
			flags |= O_NOFOLLOW;
		}
	}

	if (flags & O_DIRECTORY)
		lookup_flags |= LOOKUP_DIRECTORY;
	if (!(flags & O_NOFOLLOW))
		lookup_flags |= LOOKUP_FOLLOW;

	if (how->resolve & RESOLVE_NO_XDEV)
		lookup_flags |= LOOKUP_NO_XDEV;
	if (how->resolve & RESOLVE_NO_MAGICLINKS)
		lookup_flags |= LOOKUP_NO_MAGICLINKS;
	if (how->resolve & RESOLVE_NO_SYMLINKS)
		lookup_flags |= LOOKUP_NO_SYMLINKS;
	if (how->resolve & RESOLVE_BENEATH)
		lookup_flags |= LOOKUP_BENEATH;
	if (how->resolve & RESOLVE_IN_ROOT)
		lookup_flags |= LOOKUP_IN_ROOT;
	if (how->resolve & RESOLVE_CACHED) {
		/* Don't bother even trying for create/truncate/tmpfile open */
		if (flags & (O_TRUNC | O_CREAT | __O_TMPFILE))
			return -EAGAIN;
		lookup_flags |= LOOKUP_CACHED;
	}

	op->lookup_flags = lookup_flags;
	return 0;
}

/**
 * file_open_name - open file and return file pointer
 *
 * @name:	struct filename containing path to open
 * @flags:	open flags as per the open(2) second argument
 * @mode:	mode for the new file if O_CREAT is set, else ignored
 *
 * This is the helper to open a file from kernelspace if you really
 * have to.  But in generally you should not do this, so please move
 * along, nothing to see here..
 */
struct file *file_open_name(struct filename *name, int flags, umode_t mode)
{
	struct open_flags op;
	struct open_how how = build_open_how(flags, mode);
	int err = build_open_flags(&how, &op);
	if (err)
		return ERR_PTR(err);
	return do_filp_open(AT_FDCWD, name, &op);
}

/**
 * filp_open - open file and return file pointer
 *
 * @filename:	path to open
 * @flags:	open flags as per the open(2) second argument
 * @mode:	mode for the new file if O_CREAT is set, else ignored
 *
 * This is the helper to open a file from kernelspace if you really
 * have to.  But in generally you should not do this, so please move
 * along, nothing to see here..
 */
struct file *filp_open(const char *filename, int flags, umode_t mode)
{
	struct filename *name = getname_kernel(filename);
	struct file *file = ERR_CAST(name);

	if (!IS_ERR(name)) {
		file = file_open_name(name, flags, mode);
		putname(name);
	}
	return file;
}
EXPORT_SYMBOL(filp_open);

struct file *file_open_root(const struct path *root,
			    const char *filename, int flags, umode_t mode)
{
	struct open_flags op;
	struct open_how how = build_open_how(flags, mode);
	int err = build_open_flags(&how, &op);
	if (err)
		return ERR_PTR(err);
	return do_file_open_root(root, filename, &op);
}
EXPORT_SYMBOL(file_open_root);

static int do_sys_openat2(int dfd, const char __user *filename,
			  struct open_how *how)
{
	struct open_flags op;
	struct filename *tmp;
	int err, fd;

	err = build_open_flags(how, &op);
	if (unlikely(err))
		return err;

	tmp = getname(filename);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	fd = get_unused_fd_flags(how->flags);
	if (likely(fd >= 0)) {
		struct file *f = do_filp_open(dfd, tmp, &op);
		if (IS_ERR(f)) {
			put_unused_fd(fd);
			fd = PTR_ERR(f);
		} else {
			fd_install(fd, f);
		}
	}
	putname(tmp);
	return fd;
}

int do_sys_open(int dfd, const char __user *filename, int flags, umode_t mode)
{
	struct open_how how = build_open_how(flags, mode);
	return do_sys_openat2(dfd, filename, &how);
}


SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)
{
	if (force_o_largefile())
		flags |= O_LARGEFILE;
	return do_sys_open(AT_FDCWD, filename, flags, mode);
}

SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename, int, flags,
		umode_t, mode)
{
	if (force_o_largefile())
		flags |= O_LARGEFILE;
	return do_sys_open(dfd, filename, flags, mode);
}

SYSCALL_DEFINE4(openat2, int, dfd, const char __user *, filename,
		struct open_how __user *, how, size_t, usize)
{
	int err;
	struct open_how tmp;

	BUILD_BUG_ON(sizeof(struct open_how) < OPEN_HOW_SIZE_VER0);
	BUILD_BUG_ON(sizeof(struct open_how) != OPEN_HOW_SIZE_LATEST);

	if (unlikely(usize < OPEN_HOW_SIZE_VER0))
		return -EINVAL;
	if (unlikely(usize > PAGE_SIZE))
		return -E2BIG;

	err = copy_struct_from_user(&tmp, sizeof(tmp), how, usize);
	if (err)
		return err;

	audit_openat2_how(&tmp);

	/* O_LARGEFILE is only allowed for non-O_PATH. */
	if (!(tmp.flags & O_PATH) && force_o_largefile())
		tmp.flags |= O_LARGEFILE;

	return do_sys_openat2(dfd, filename, &tmp);
}

#ifdef CONFIG_COMPAT
/*
 * Exactly like sys_open(), except that it doesn't set the
 * O_LARGEFILE flag.
 */
COMPAT_SYSCALL_DEFINE3(open, const char __user *, filename, int, flags, umode_t, mode)
{
	return do_sys_open(AT_FDCWD, filename, flags, mode);
}

/*
 * Exactly like sys_openat(), except that it doesn't set the
 * O_LARGEFILE flag.
 */
COMPAT_SYSCALL_DEFINE4(openat, int, dfd, const char __user *, filename, int, flags, umode_t, mode)
{
	return do_sys_open(dfd, filename, flags, mode);
}
#endif

#ifndef __alpha__

/*
 * For backward compatibility?  Maybe this should be moved
 * into arch/i386 instead?
 */
SYSCALL_DEFINE2(creat, const char __user *, pathname, umode_t, mode)
{
	int flags = O_CREAT | O_WRONLY | O_TRUNC;

	if (force_o_largefile())
		flags |= O_LARGEFILE;
	return do_sys_open(AT_FDCWD, pathname, flags, mode);
}
#endif

/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
static int filp_flush(struct file *filp, fl_owner_t id)
{
	int retval = 0;

	if (CHECK_DATA_CORRUPTION(file_count(filp) == 0, filp,
			"VFS: Close: file count is 0 (f_op=%ps)",
			filp->f_op)) {
		return 0;
	}

	if (filp->f_op->flush)
		retval = filp->f_op->flush(filp, id);

	if (likely(!(filp->f_mode & FMODE_PATH))) {
		dnotify_flush(filp, id);
		locks_remove_posix(filp, id);
	}
	return retval;
}

int filp_close(struct file *filp, fl_owner_t id)
{
	int retval;

	retval = filp_flush(filp, id);
	fput_close(filp);

	return retval;
}
EXPORT_SYMBOL(filp_close);

/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
SYSCALL_DEFINE1(close, unsigned int, fd)
{
	int retval;
	struct file *file;

	file = file_close_fd(fd);
	if (!file)
		return -EBADF;

	retval = filp_flush(file, current->files);

	/*
	 * We're returning to user space. Don't bother
	 * with any delayed fput() cases.
	 */
	fput_close_sync(file);

	if (likely(retval == 0))
		return 0;

	/* can't restart close syscall because file table entry was cleared */
	if (retval == -ERESTARTSYS ||
	    retval == -ERESTARTNOINTR ||
	    retval == -ERESTARTNOHAND ||
	    retval == -ERESTART_RESTARTBLOCK)
		retval = -EINTR;

	return retval;
}

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */
SYSCALL_DEFINE0(vhangup)
{
	if (capable(CAP_SYS_TTY_CONFIG)) {
		tty_vhangup_self();
		return 0;
	}
	return -EPERM;
}

/*
 * Called when an inode is about to be open.
 * We use this to disallow opening large files on 32bit systems if
 * the caller didn't specify O_LARGEFILE.  On 64bit systems we force
 * on this flag in sys_open.
 */
int generic_file_open(struct inode * inode, struct file * filp)
{
	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;
	return 0;
}

EXPORT_SYMBOL(generic_file_open);

/*
 * This is used by subsystems that don't want seekable
 * file descriptors. The function is not supposed to ever fail, the only
 * reason it returns an 'int' and not 'void' is so that it can be plugged
 * directly into file_operations structure.
 */
int nonseekable_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return 0;
}

EXPORT_SYMBOL(nonseekable_open);

/*
 * stream_open is used by subsystems that want stream-like file descriptors.
 * Such file descriptors are not seekable and don't have notion of position
 * (file.f_pos is always 0 and ppos passed to .read()/.write() is always NULL).
 * Contrary to file descriptors of other regular files, .read() and .write()
 * can run simultaneously.
 *
 * stream_open never fails and is marked to return int so that it could be
 * directly used as file_operations.open .
 */
int stream_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE | FMODE_ATOMIC_POS);
	filp->f_mode |= FMODE_STREAM;
	return 0;
}

EXPORT_SYMBOL(stream_open);
