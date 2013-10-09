/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 1999,2000,2001,2002,2003,2006,2007,2008,2009,2010,2011  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config-util.h>
#include <config.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <grub/util/misc.h>

#include <grub/cryptodisk.h>
#include <grub/i18n.h>

#if !defined (__MINGW32__) && !defined (__CYGWIN__) && !defined (__AROS__)

#ifdef __linux__
#include <sys/ioctl.h>         /* ioctl */
#include <sys/mount.h>
#ifndef MAJOR
# ifndef MINORBITS
#  define MINORBITS	8
# endif /* ! MINORBITS */
# define MAJOR(dev)	((unsigned) ((dev) >> MINORBITS))
#endif /* ! MAJOR */
#ifndef FLOPPY_MAJOR
# define FLOPPY_MAJOR	2
#endif /* ! FLOPPY_MAJOR */
#endif

#include <sys/types.h>

#if defined(HAVE_LIBZFS) && defined(HAVE_LIBNVPAIR)
# include <grub/util/libzfs.h>
# include <grub/util/libnvpair.h>
#endif

#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/emu/misc.h>
#include <grub/emu/hostdisk.h>
#include <grub/emu/getroot.h>

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
# define MAJOR(dev) major(dev)
# define FLOPPY_MAJOR	2
#endif

#if defined (__FreeBSD__) || defined (__FreeBSD_kernel__)
#include <sys/mount.h>
#endif

#if defined(__NetBSD__) || defined(__OpenBSD__)
# include <sys/ioctl.h>
# include <sys/disklabel.h>    /* struct disklabel */
# include <sys/disk.h>    /* struct dkwedge_info */
#include <sys/param.h>
#include <sys/mount.h>
#endif /* defined(__NetBSD__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) */

#if defined(__NetBSD__) || defined(__OpenBSD__)
# define MAJOR(dev) major(dev)
# ifdef HAVE_GETRAWPARTITION
#  include <util.h>    /* getrawpartition */
# endif /* HAVE_GETRAWPARTITION */
#if defined(__NetBSD__)
# include <sys/fdio.h>
#endif
# ifndef FLOPPY_MAJOR
#  define FLOPPY_MAJOR	2
# endif /* ! FLOPPY_MAJOR */
# ifndef RAW_FLOPPY_MAJOR
#  define RAW_FLOPPY_MAJOR	9
# endif /* ! RAW_FLOPPY_MAJOR */
#endif /* defined(__NetBSD__) */

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#define LVM_DEV_MAPPER_STRING "/dev/linux_lvm/"
#else
#define LVM_DEV_MAPPER_STRING "/dev/mapper/"
#endif

#include <sys/types.h>
#include <sys/wait.h>

static void
strip_extra_slashes (char *dir)
{
  char *p = dir;

  while ((p = strchr (p, '/')) != 0)
    {
      if (p[1] == '/')
	{
	  memmove (p, p + 1, strlen (p));
	  continue;
	}
      else if (p[1] == '\0')
	{
	  if (p > dir)
	    p[0] = '\0';
	  break;
	}

      p++;
    }
}

static char *
xgetcwd (void)
{
  size_t size = 10;
  char *path;

  path = xmalloc (size);
  while (! getcwd (path, size))
    {
      size <<= 1;
      path = xrealloc (path, size);
    }

  return path;
}

pid_t
grub_util_exec_pipe (char **argv, int *fd)
{
  int mdadm_pipe[2];
  pid_t mdadm_pid;

  *fd = 0;

  if (pipe (mdadm_pipe) < 0)
    {
      grub_util_warn (_("Unable to create pipe: %s"),
		      strerror (errno));
      return 0;
    }
  mdadm_pid = fork ();
  if (mdadm_pid < 0)
    grub_util_error (_("Unable to fork: %s"), strerror (errno));
  else if (mdadm_pid == 0)
    {
      /* Child.  */

      /* Close fd's.  */
      grub_util_devmapper_cleanup ();
      grub_diskfilter_fini ();

      /* Ensure child is not localised.  */
      setenv ("LC_ALL", "C", 1);

      close (mdadm_pipe[0]);
      dup2 (mdadm_pipe[1], STDOUT_FILENO);
      close (mdadm_pipe[1]);

      execvp (argv[0], argv);
      exit (127);
    }
  else
    {
      close (mdadm_pipe[1]);
      *fd = mdadm_pipe[0];
      return mdadm_pid;
    }
}

#if !defined (__GNU__)
char **
grub_util_find_root_devices_from_poolname (char *poolname)
{
  char **devices = 0;
  size_t ndevices = 0;
  size_t devices_allocated = 0;

#if defined(HAVE_LIBZFS) && defined(HAVE_LIBNVPAIR)
  zpool_handle_t *zpool;
  libzfs_handle_t *libzfs;
  nvlist_t *config, *vdev_tree;
  nvlist_t **children;
  unsigned int nvlist_count;
  unsigned int i;
  char *device = 0;

  libzfs = grub_get_libzfs_handle ();
  if (! libzfs)
    return NULL;

  zpool = zpool_open (libzfs, poolname);
  config = zpool_get_config (zpool, NULL);

  if (nvlist_lookup_nvlist (config, "vdev_tree", &vdev_tree) != 0)
    error (1, errno, "nvlist_lookup_nvlist (\"vdev_tree\")");

  if (nvlist_lookup_nvlist_array (vdev_tree, "children", &children, &nvlist_count) != 0)
    error (1, errno, "nvlist_lookup_nvlist_array (\"children\")");
  assert (nvlist_count > 0);

  while (nvlist_lookup_nvlist_array (children[0], "children",
				     &children, &nvlist_count) == 0)
    assert (nvlist_count > 0);

  for (i = 0; i < nvlist_count; i++)
    {
      if (nvlist_lookup_string (children[i], "path", &device) != 0)
	error (1, errno, "nvlist_lookup_string (\"path\")");

      struct stat st;
      if (stat (device, &st) == 0)
	{
#ifdef __sun__
	  if (grub_memcmp (device, "/dev/dsk/", sizeof ("/dev/dsk/") - 1)
	      == 0)
	    device = xasprintf ("/dev/rdsk/%s",
				device + sizeof ("/dev/dsk/") - 1);
	  else if (grub_memcmp (device, "/devices", sizeof ("/devices") - 1)
		   == 0
		   && grub_memcmp (device + strlen (device) - 4,
				   ",raw", 4) != 0)
	    device = xasprintf ("%s,raw", device);
	  else
#endif
	    device = xstrdup (device);
	  if (ndevices >= devices_allocated)
	    {
	      devices_allocated = 2 * (devices_allocated + 8);
	      devices = xrealloc (devices, sizeof (devices[0])
				  * devices_allocated);
	    }
	  devices[ndevices++] = device;
	}

      device = NULL;
    }

  zpool_close (zpool);
#else
  FILE *fp;
  int ret;
  char *line;
  size_t len;
  int st;

  char name[PATH_MAX + 1], state[257], readlen[257], writelen[257];
  char cksum[257], notes[257];
  unsigned int dummy;
  char *argv[4];
  pid_t pid;
  int fd;

  /* execvp has inconvenient types, hence the casts.  None of these
     strings will actually be modified.  */
  argv[0] = (char *) "zpool";
  argv[1] = (char *) "status";
  argv[2] = (char *) poolname;
  argv[3] = NULL;

  pid = grub_util_exec_pipe (argv, &fd);
  if (!pid)
    return NULL;

  fp = fdopen (fd, "r");
  if (!fp)
    {
      grub_util_warn (_("Unable to open stream from %s: %s"),
		      "zpool", strerror (errno));
      goto out;
    }

  st = 0;
  while (1)
    {
      line = NULL;
      ret = getline (&line, &len, fp);
      if (ret == -1)
	break;
	
      if (sscanf (line, " %s %256s %256s %256s %256s %256s",
		  name, state, readlen, writelen, cksum, notes) >= 5)
	switch (st)
	  {
	  case 0:
	    if (!strcmp (name, "NAME")
		&& !strcmp (state, "STATE")
		&& !strcmp (readlen, "READ")
		&& !strcmp (writelen, "WRITE")
		&& !strcmp (cksum, "CKSUM"))
	      st++;
	    break;
	  case 1:
	    {
	      char *ptr = line;
	      while (1)
		{
		  if (strncmp (ptr, poolname, strlen (poolname)) == 0
		      && grub_isspace(ptr[strlen (poolname)]))
		    st++;
		  if (!grub_isspace (*ptr))
		    break;
		  ptr++;
		}
	    }
	    break;
	  case 2:
	    if (strcmp (name, "mirror") && !sscanf (name, "mirror-%u", &dummy)
		&& !sscanf (name, "raidz%u", &dummy)
		&& !sscanf (name, "raidz1%u", &dummy)
		&& !sscanf (name, "raidz2%u", &dummy)
		&& !sscanf (name, "raidz3%u", &dummy)
		&& !strcmp (state, "ONLINE"))
	      {
		if (ndevices >= devices_allocated)
		  {
		    devices_allocated = 2 * (devices_allocated + 8);
		    devices = xrealloc (devices, sizeof (devices[0])
					* devices_allocated);
		  }
		if (name[0] == '/')
		  devices[ndevices++] = xstrdup (name);
		else
		  devices[ndevices++] = xasprintf ("/dev/%s", name);
	      }
	    break;
	  }
	
      free (line);
    }

 out:
  close (fd);
  waitpid (pid, NULL, 0);
#endif
  if (devices)
    {
      if (ndevices >= devices_allocated)
	{
	  devices_allocated = 2 * (devices_allocated + 8);
	  devices = xrealloc (devices, sizeof (devices[0])
			      * devices_allocated);
	}
      devices[ndevices++] = 0;
    }
  return devices;
}

static char **
find_root_devices_from_libzfs (const char *dir)
{
  char **devices = NULL;
  char *poolname;
  char *poolfs;

  grub_find_zpool_from_dir (dir, &poolname, &poolfs);
  if (! poolname)
    return NULL;

  devices = grub_util_find_root_devices_from_poolname (poolname);

  free (poolname);
  if (poolfs)
    free (poolfs);

  return devices;
}

char *
grub_find_device (const char *dir, dev_t dev)
{
  DIR *dp;
  char *saved_cwd;
  struct dirent *ent;

  if (! dir)
    dir = "/dev";

  dp = opendir (dir);
  if (! dp)
    return 0;

  saved_cwd = xgetcwd ();

  grub_util_info ("changing current directory to %s", dir);
  if (chdir (dir) < 0)
    {
      free (saved_cwd);
      closedir (dp);
      return 0;
    }

  while ((ent = readdir (dp)) != 0)
    {
      struct stat st;

      /* Avoid:
	 - dotfiles (like "/dev/.tmp.md0") since they could be duplicates.
	 - dotdirs (like "/dev/.static") since they could contain duplicates.  */
      if (ent->d_name[0] == '.')
	continue;

      if (lstat (ent->d_name, &st) < 0)
	/* Ignore any error.  */
	continue;

      if (S_ISLNK (st.st_mode)) {
#ifdef __linux__
	if (strcmp (dir, "mapper") == 0 || strcmp (dir, "/dev/mapper") == 0) {
	  /* Follow symbolic links under /dev/mapper/; the canonical name
	     may be something like /dev/dm-0, but the names under
	     /dev/mapper/ are more human-readable and so we prefer them if
	     we can get them.  */
	  if (stat (ent->d_name, &st) < 0)
	    continue;
	} else
#endif /* __linux__ */
	/* Don't follow other symbolic links.  */
	continue;
      }

      if (S_ISDIR (st.st_mode))
	{
	  /* Find it recursively.  */
	  char *res;

	  res = grub_find_device (ent->d_name, dev);

	  if (res)
	    {
	      if (chdir (saved_cwd) < 0)
		grub_util_error ("%s",
				 _("cannot restore the original directory"));

	      free (saved_cwd);
	      closedir (dp);
	      return res;
	    }
	}

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__APPLE__)
      if (S_ISCHR (st.st_mode) && st.st_rdev == dev)
#else
      if (S_ISBLK (st.st_mode) && st.st_rdev == dev)
#endif
	{
#ifdef __linux__
	  /* Skip device names like /dev/dm-0, which are short-hand aliases
	     to more descriptive device names, e.g. those under /dev/mapper */
	  if (ent->d_name[0] == 'd' &&
	      ent->d_name[1] == 'm' &&
	      ent->d_name[2] == '-' &&
	      ent->d_name[3] >= '0' &&
	      ent->d_name[3] <= '9')
	    continue;
#endif

	  /* Found!  */
	  char *res;
	  char *cwd;
#if defined(__NetBSD__) || defined(__OpenBSD__)
	  /* Convert this block device to its character (raw) device.  */
	  const char *template = "%s/r%s";
#else
	  /* Keep the device name as it is.  */
	  const char *template = "%s/%s";
#endif

	  cwd = xgetcwd ();
	  res = xmalloc (strlen (cwd) + strlen (ent->d_name) + 3);
	  sprintf (res, template, cwd, ent->d_name);
	  strip_extra_slashes (res);
	  free (cwd);

	  /* /dev/root is not a real block device keep looking, takes care
	     of situation where root filesystem is on the same partition as
	     grub files */

	  if (strcmp(res, "/dev/root") == 0)
	    {
	      free (res);
	      continue;
	    }

	  if (chdir (saved_cwd) < 0)
	    grub_util_error ("%s", _("cannot restore the original directory"));

	  free (saved_cwd);
	  closedir (dp);
	  return res;
	}
    }

  if (chdir (saved_cwd) < 0)
    grub_util_error ("%s", _("cannot restore the original directory"));

  free (saved_cwd);
  closedir (dp);
  return 0;
}

char **
grub_guess_root_devices (const char *dir)
{
  char **os_dev = NULL;
  struct stat st;
  dev_t dev;

#ifdef __linux__
  if (!os_dev)
    os_dev = grub_find_root_devices_from_mountinfo (dir, NULL);
#endif /* __linux__ */

  if (!os_dev)
    os_dev = find_root_devices_from_libzfs (dir);

  if (os_dev)
    {
      char **cur;
      for (cur = os_dev; *cur; cur++)
	{
	  char *tmp = *cur;
	  int root, dm;
	  if (strcmp (*cur, "/dev/root") == 0
	      || strncmp (*cur, "/dev/dm-", sizeof ("/dev/dm-") - 1) == 0)
	    *cur = tmp;
	  else
	    {
	      *cur = canonicalize_file_name (tmp);
	      if (*cur == NULL)
		grub_util_error (_("failed to get canonical path of `%s'"), tmp);
	      free (tmp);
	    }
	  root = (strcmp (*cur, "/dev/root") == 0);
	  dm = (strncmp (*cur, "/dev/dm-", sizeof ("/dev/dm-") - 1) == 0);
	  if (!dm && !root)
	    continue;
	  if (stat (*cur, &st) < 0)
	    break;
	  free (*cur);
	  dev = st.st_rdev;
	  *cur = grub_find_device (dm ? "/dev/mapper" : "/dev", dev);
	}
      if (!*cur)
	return os_dev;
      for (cur = os_dev; *cur; cur++)
	free (*cur);
      free (os_dev);
      os_dev = 0;
    }

  if (stat (dir, &st) < 0)
    grub_util_error (_("cannot stat `%s': %s"), dir, strerror (errno));

  dev = st.st_dev;

  os_dev = xmalloc (2 * sizeof (os_dev[0]));

  /* This might be truly slow, but is there any better way?  */
  os_dev[0] = grub_find_device ("/dev", dev);

  if (!os_dev[0])
    {
      free (os_dev);
      return 0;
    }

  os_dev[1] = 0;

  return os_dev;
}

#endif

void
grub_util_pull_lvm_by_command (const char *os_dev)
{
  char *argv[8];
  int fd;
  pid_t pid;
  FILE *mdadm;
  char *buf = NULL;
  size_t len = 0;
  char *vgname = NULL;
  const char *iptr;
  char *optr;
  char *vgid = NULL;
  grub_size_t vgidlen = 0;

  vgid = grub_util_get_vg_uuid (os_dev);
  if (vgid)
    vgidlen = grub_strlen (vgid);

  if (!vgid)
    {
      if (strncmp (os_dev, LVM_DEV_MAPPER_STRING,
		   sizeof (LVM_DEV_MAPPER_STRING) - 1)
	  != 0)
	return;

      vgname = xmalloc (strlen (os_dev + sizeof (LVM_DEV_MAPPER_STRING) - 1) + 1);
      for (iptr = os_dev + sizeof (LVM_DEV_MAPPER_STRING) - 1, optr = vgname; *iptr; )
	if (*iptr != '-')
	  *optr++ = *iptr++;
	else if (iptr[0] == '-' && iptr[1] == '-')
	  {
	    iptr += 2;
	    *optr++ = '-';
	  }
	else
	  break;
      *optr = '\0';
    }

  /* execvp has inconvenient types, hence the casts.  None of these
     strings will actually be modified.  */
  /* by default PV name is left aligned in 10 character field, meaning that
     we do not know where name ends. Using dummy --separator disables
     alignment. We have a single field, so separator itself is not output */
  argv[0] = (char *) "vgs";
  argv[1] = (char *) "--options";
  if (vgid)
    argv[2] = (char *) "vg_uuid,pv_name";
  else
    argv[2] = (char *) "pv_name";
  argv[3] = (char *) "--noheadings";
  argv[4] = (char *) "--separator";
  argv[5] = (char *) ":";
  argv[6] = vgname;
  argv[7] = NULL;

  pid = grub_util_exec_pipe (argv, &fd);
  free (vgname);

  if (!pid)
    return;

  /* Parent.  Read mdadm's output.  */
  mdadm = fdopen (fd, "r");
  if (! mdadm)
    {
      grub_util_warn (_("Unable to open stream from %s: %s"),
		      "vgs", strerror (errno));
      goto out;
    }

  while (getline (&buf, &len, mdadm) > 0)
    {
      char *ptr;
      /* LVM adds two spaces as standard prefix */
      for (ptr = buf; ptr < buf + 2 && *ptr == ' '; ptr++);

      if (vgid && (grub_strncmp (vgid, ptr, vgidlen) != 0
		   || ptr[vgidlen] != ':'))
	continue;
      if (vgid)
	ptr += vgidlen + 1;
      if (*ptr == '\0')
	continue;
      *(ptr + strlen (ptr) - 1) = '\0';
      grub_util_pull_device (ptr);
    }

out:
  close (fd);
  waitpid (pid, NULL, 0);
  free (buf);
}

/* ZFS has similar problems to those of btrfs (see above).  */
void
grub_find_zpool_from_dir (const char *dir, char **poolname, char **poolfs)
{
  char *slash;

  *poolname = *poolfs = NULL;

#if defined(HAVE_STRUCT_STATFS_F_FSTYPENAME) && defined(HAVE_STRUCT_STATFS_F_MNTFROMNAME)
  /* FreeBSD and GNU/kFreeBSD.  */
  {
    struct statfs mnt;

    if (statfs (dir, &mnt) != 0)
      return;

    if (strcmp (mnt.f_fstypename, "zfs") != 0)
      return;

    *poolname = xstrdup (mnt.f_mntfromname);
  }
#elif defined(HAVE_GETEXTMNTENT)
  /* Solaris.  */
  {
    struct stat st;
    struct extmnttab mnt;

    if (stat (dir, &st) != 0)
      return;

    FILE *mnttab = fopen ("/etc/mnttab", "r");
    if (! mnttab)
      return;

    while (getextmntent (mnttab, &mnt, sizeof (mnt)) == 0)
      {
	if (makedev (mnt.mnt_major, mnt.mnt_minor) == st.st_dev
	    && !strcmp (mnt.mnt_fstype, "zfs"))
	  {
	    *poolname = xstrdup (mnt.mnt_special);
	    break;
	  }
      }

    fclose (mnttab);
  }
#endif

  if (! *poolname)
    return;

  slash = strchr (*poolname, '/');
  if (slash)
    {
      *slash = '\0';
      *poolfs = xstrdup (slash + 1);
    }
  else
    *poolfs = xstrdup ("");
}

/* This function never prints trailing slashes (so that its output
   can be appended a slash unconditionally).  */
char *
grub_make_system_path_relative_to_its_root (const char *path)
{
  struct stat st;
  char *p, *buf, *buf2, *buf3, *ret;
  uintptr_t offset = 0;
  dev_t num;
  size_t len;
  char *poolfs = NULL;

  /* canonicalize.  */
  p = canonicalize_file_name (path);
  if (p == NULL)
    grub_util_error (_("failed to get canonical path of `%s'"), path);

  /* For ZFS sub-pool filesystems, could be extended to others (btrfs?).  */
  {
    char *dummy;
    grub_find_zpool_from_dir (p, &dummy, &poolfs);
  }

  len = strlen (p) + 1;
  buf = xstrdup (p);
  free (p);

  if (stat (buf, &st) < 0)
    grub_util_error (_("cannot stat `%s': %s"), buf, strerror (errno));

  buf2 = xstrdup (buf);
  num = st.st_dev;

  /* This loop sets offset to the number of chars of the root
     directory we're inspecting.  */
  while (1)
    {
      p = strrchr (buf, '/');
      if (p == NULL)
	/* This should never happen.  */
	grub_util_error ("%s",
			 /* TRANSLATORS: canonical pathname is the
			    complete one e.g. /etc/fstab. It has
			    to contain `/' normally, if it doesn't
			    we're in trouble and throw this error.  */
			 _("no `/' in canonical filename"));
      if (p != buf)
	*p = 0;
      else
	*++p = 0;

      if (stat (buf, &st) < 0)
	grub_util_error (_("cannot stat `%s': %s"), buf, strerror (errno));

      /* buf is another filesystem; we found it.  */
      if (st.st_dev != num)
	{
	  /* offset == 0 means path given is the mount point.
	     This works around special-casing of "/" in Un*x.  This function never
	     prints trailing slashes (so that its output can be appended a slash
	     unconditionally).  Each slash in is considered a preceding slash, and
	     therefore the root directory is an empty string.  */
	  if (offset == 0)
	    {
	      free (buf);
#ifdef __linux__
	      {
		char *bind;
		grub_free (grub_find_root_devices_from_mountinfo (buf2, &bind));
		if (bind && bind[0] && bind[1])
		  {
		    buf3 = bind;
		    goto parsedir;
		  }
		grub_free (bind);
	      }
#endif
	      free (buf2);
	      if (poolfs)
		return xasprintf ("/%s/@", poolfs);
	      return xstrdup ("");
	    }
	  else
	    break;
	}

      offset = p - buf;
      /* offset == 1 means root directory.  */
      if (offset == 1)
	{
	  /* Include leading slash.  */
	  offset = 0;
	  break;
	}
    }
  free (buf);
  buf3 = xstrdup (buf2 + offset);
  buf2[offset] = 0;
#ifdef __linux__
  {
    char *bind;
    grub_free (grub_find_root_devices_from_mountinfo (buf2, &bind));
    if (bind && bind[0] && bind[1])
      {
	char *temp = buf3;
	buf3 = grub_xasprintf ("%s%s%s", bind, buf3[0] == '/' ?"":"/", buf3);
	grub_free (temp);
      }
    grub_free (bind);
  }
#endif
  
  free (buf2);

#ifdef __linux__
 parsedir:
#endif
  /* Remove trailing slashes, return empty string if root directory.  */
  len = strlen (buf3);
  while (len > 0 && buf3[len - 1] == '/')
    {
      buf3[len - 1] = '\0';
      len--;
    }

  if (poolfs)
    {
      ret = xasprintf ("/%s/@%s", poolfs, buf3);
      free (buf3);
    }
  else
    ret = buf3;

  return ret;
}

int
grub_util_biosdisk_is_floppy (grub_disk_t disk)
{
  struct stat st;
  int fd;
  const char *dname;

  dname = grub_util_biosdisk_get_osdev (disk);

  if (!dname)
    return 0;

  fd = open (dname, O_RDONLY);
  /* Shouldn't happen.  */
  if (fd == -1)
    return 0;

  /* Shouldn't happen either.  */
  if (fstat (fd, &st) < 0)
    {
      close (fd);
      return 0;
    }

  close (fd);

#if defined(__NetBSD__)
  if (major(st.st_rdev) == RAW_FLOPPY_MAJOR)
    return 1;
#endif

#if defined(FLOPPY_MAJOR)
  if (major(st.st_rdev) == FLOPPY_MAJOR)
#else
  /* Some kernels (e.g. kFreeBSD) don't have a static major number
     for floppies, but they still use a "fd[0-9]" pathname.  */
  if (dname[5] == 'f'
      && dname[6] == 'd'
      && dname[7] >= '0'
      && dname[7] <= '9')
#endif
    return 1;

  return 0;
}

const char *
grub_util_check_block_device (const char *blk_dev)
{
  struct stat st;

  if (stat (blk_dev, &st) < 0)
    grub_util_error (_("cannot stat `%s': %s"), blk_dev,
		     strerror (errno));

  if (S_ISBLK (st.st_mode))
    return (blk_dev);
  else
    return 0;
}

const char *
grub_util_check_char_device (const char *blk_dev)
{
  struct stat st;

  if (stat (blk_dev, &st) < 0)
    grub_util_error (_("cannot stat `%s': %s"), blk_dev, strerror (errno));

  if (S_ISCHR (st.st_mode))
    return (blk_dev);
  else
    return 0;
}

#else

void
grub_util_pull_lvm_by_command (const char *os_dev __attribute__ ((unused)))
{
}

#endif