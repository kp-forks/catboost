/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <sys/disk.h>
#elif defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/disklabel.h>
#error #include <sys/dkio.h>
#elif defined(__DragonFly__)
#error #include <sys/diskslice.h>
#endif

#include "archive.h"
#include "archive_private.h"
#include "archive_string.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC	0
#endif

struct read_file_data {
	int	 fd;
	size_t	 block_size;
	void	*buffer;
	mode_t	 st_mode;  /* Mode bits for opened file. */
	int64_t	 size;
	char	 use_lseek;
	enum fnt_e { FNT_STDIN, FNT_MBS, FNT_WCS } filename_type;
	union {
		char	 m[1];/* MBS filename. */
		wchar_t	 w[1];/* WCS filename. */
	} filename; /* Must be last! */
};

static int	file_open(struct archive *, void *);
static int	file_close(struct archive *, void *);
static int file_close2(struct archive *, void *);
static int file_switch(struct archive *, void *, void *);
static ssize_t	file_read(struct archive *, void *, const void **buff);
static int64_t	file_seek(struct archive *, void *, int64_t request, int);
static int64_t	file_skip(struct archive *, void *, int64_t request);
static int64_t	file_skip_lseek(struct archive *, void *, int64_t request);

int
archive_read_open_file(struct archive *a, const char *filename,
    size_t block_size)
{
	return (archive_read_open_filename(a, filename, block_size));
}

int
archive_read_open_filename(struct archive *a, const char *filename,
    size_t block_size)
{
	const char *filenames[2];
	filenames[0] = filename;
	filenames[1] = NULL;
	return archive_read_open_filenames(a, filenames, block_size);
}

int
archive_read_open_filenames(struct archive *a, const char **filenames,
    size_t block_size)
{
	struct read_file_data *mine;
	const char *filename = NULL;
	if (filenames)
		filename = *(filenames++);

	archive_clear_error(a);
	do
	{
		if (filename == NULL)
			filename = "";
		mine = calloc(1,
			sizeof(*mine) + strlen(filename));
		if (mine == NULL)
			goto no_memory;
		strcpy(mine->filename.m, filename);
		mine->block_size = block_size;
		mine->fd = -1;
		mine->buffer = NULL;
		mine->st_mode = mine->use_lseek = 0;
		if (filename == NULL || filename[0] == '\0') {
			mine->filename_type = FNT_STDIN;
		} else
			mine->filename_type = FNT_MBS;
		if (archive_read_append_callback_data(a, mine) != (ARCHIVE_OK))
			return (ARCHIVE_FATAL);
		if (filenames == NULL)
			break;
		filename = *(filenames++);
	} while (filename != NULL && filename[0] != '\0');
	archive_read_set_open_callback(a, file_open);
	archive_read_set_read_callback(a, file_read);
	archive_read_set_skip_callback(a, file_skip);
	archive_read_set_close_callback(a, file_close);
	archive_read_set_switch_callback(a, file_switch);
	archive_read_set_seek_callback(a, file_seek);

	return (archive_read_open1(a));
no_memory:
	archive_set_error(a, ENOMEM, "No memory");
	return (ARCHIVE_FATAL);
}

/*
 * This function is an implementation detail of archive_read_open_filename_w,
 * which is exposed as a separate API on Windows.
 */
#if !defined(_WIN32) || defined(__CYGWIN__)
static
#endif
int
archive_read_open_filenames_w(struct archive *a, const wchar_t **wfilenames,
    size_t block_size)
{
	struct read_file_data *mine;
	const wchar_t *wfilename = NULL;
	if (wfilenames)
		wfilename = *(wfilenames++);

	archive_clear_error(a);
	do
	{
		if (wfilename == NULL)
			wfilename = L"";
		mine = calloc(1,
			sizeof(*mine) + wcslen(wfilename) * sizeof(wchar_t));
		if (mine == NULL)
			goto no_memory;
		mine->block_size = block_size;
		mine->fd = -1;

		if (wfilename == NULL || wfilename[0] == L'\0') {
			mine->filename_type = FNT_STDIN;
		} else {
#if defined(_WIN32) && !defined(__CYGWIN__)
			mine->filename_type = FNT_WCS;
			wcscpy(mine->filename.w, wfilename);
#else
			/*
			 * POSIX system does not support a wchar_t interface for
			 * open() system call, so we have to translate a wchar_t
			 * filename to multi-byte one and use it.
			 */
			struct archive_string fn;

			archive_string_init(&fn);
			if (archive_string_append_from_wcs(&fn, wfilename,
			    wcslen(wfilename)) != 0) {
				if (errno == ENOMEM)
					archive_set_error(a, errno,
					    "Can't allocate memory");
				else
					archive_set_error(a, EINVAL,
					    "Failed to convert a wide-character"
					    " filename to a multi-byte filename");
				archive_string_free(&fn);
				free(mine);
				return (ARCHIVE_FATAL);
			}
			mine->filename_type = FNT_MBS;
			strcpy(mine->filename.m, fn.s);
			archive_string_free(&fn);
#endif
		}
		if (archive_read_append_callback_data(a, mine) != (ARCHIVE_OK))
			return (ARCHIVE_FATAL);
		if (wfilenames == NULL)
			break;
		wfilename = *(wfilenames++);
	} while (wfilename != NULL && wfilename[0] != '\0');
	archive_read_set_open_callback(a, file_open);
	archive_read_set_read_callback(a, file_read);
	archive_read_set_skip_callback(a, file_skip);
	archive_read_set_close_callback(a, file_close);
	archive_read_set_switch_callback(a, file_switch);
	archive_read_set_seek_callback(a, file_seek);

	return (archive_read_open1(a));
no_memory:
	archive_set_error(a, ENOMEM, "No memory");
	return (ARCHIVE_FATAL);
}

int
archive_read_open_filename_w(struct archive *a, const wchar_t *wfilename,
    size_t block_size)
{
	const wchar_t *wfilenames[2];
	wfilenames[0] = wfilename;
	wfilenames[1] = NULL;
	return archive_read_open_filenames_w(a, wfilenames, block_size);
}

static int
file_open(struct archive *a, void *client_data)
{
	struct stat st;
	struct read_file_data *mine = (struct read_file_data *)client_data;
	void *buffer;
	const char *filename = NULL;
#if defined(_WIN32) && !defined(__CYGWIN__)
	const wchar_t *wfilename = NULL;
#endif
	int fd = -1;
	int is_disk_like = 0;
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
	off_t mediasize = 0; /* FreeBSD-specific, so off_t okay here. */
#elif defined(__NetBSD__) || defined(__OpenBSD__)
	struct disklabel dl;
#elif defined(__DragonFly__)
	struct partinfo pi;
#endif

	archive_clear_error(a);
	if (mine->filename_type == FNT_STDIN) {
		/* We used to delegate stdin support by
		 * directly calling archive_read_open_fd(a,0,block_size)
		 * here, but that doesn't (and shouldn't) handle the
		 * end-of-file flush when reading stdout from a pipe.
		 * Basically, read_open_fd() is intended for folks who
		 * are willing to handle such details themselves.  This
		 * API is intended to be a little smarter for folks who
		 * want easy handling of the common case.
		 */
		fd = 0;
#if defined(__CYGWIN__) || defined(_WIN32)
		setmode(0, O_BINARY);
#endif
		filename = "";
	} else if (mine->filename_type == FNT_MBS) {
		filename = mine->filename.m;
		fd = open(filename, O_RDONLY | O_BINARY | O_CLOEXEC);
		__archive_ensure_cloexec_flag(fd);
		if (fd < 0) {
			archive_set_error(a, errno,
			    "Failed to open '%s'", filename);
			return (ARCHIVE_FATAL);
		}
	} else {
#if defined(_WIN32) && !defined(__CYGWIN__)
		wfilename = mine->filename.w;
		fd = _wopen(wfilename, O_RDONLY | O_BINARY);
		if (fd < 0 && errno == ENOENT) {
			wchar_t *fullpath;
			fullpath = __la_win_permissive_name_w(wfilename);
			if (fullpath != NULL) {
				fd = _wopen(fullpath, O_RDONLY | O_BINARY);
				free(fullpath);
			}
		}
		if (fd < 0) {
			archive_set_error(a, errno,
			    "Failed to open '%ls'", wfilename);
			return (ARCHIVE_FATAL);
		}
#else
		archive_set_error(a, ARCHIVE_ERRNO_MISC,
		    "Unexpedted operation in archive_read_open_filename");
		goto fail;
#endif
	}
	if (fstat(fd, &st) != 0) {
#if defined(_WIN32) && !defined(__CYGWIN__)
		if (mine->filename_type == FNT_WCS)
			archive_set_error(a, errno, "Can't stat '%ls'",
			    wfilename);
		else
#endif
			archive_set_error(a, errno, "Can't stat '%s'",
			    filename);
		goto fail;
	}

	/*
	 * Determine whether the input looks like a disk device or a
	 * tape device.  The results are used below to select an I/O
	 * strategy:
	 *  = "disk-like" devices support arbitrary lseek() and will
	 *    support I/O requests of any size.  So we get easy skipping
	 *    and can cheat on block sizes to get better performance.
	 *  = "tape-like" devices require strict blocking and use
	 *    specialized ioctls for seeking.
	 *  = "socket-like" devices cannot seek at all but can improve
	 *    performance by using nonblocking I/O to read "whatever is
	 *    available right now".
	 *
	 * Right now, we only specially recognize disk-like devices,
	 * but it should be straightforward to add probes and strategy
	 * here for tape-like and socket-like devices.
	 */
	if (S_ISREG(st.st_mode)) {
		/* Safety:  Tell the extractor not to overwrite the input. */
		archive_read_extract_set_skip_file(a, st.st_dev, st.st_ino);
		/* Regular files act like disks. */
		is_disk_like = 1;
	}
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
	/* FreeBSD: if it supports DIOCGMEDIASIZE ioctl, it's disk-like. */
	else if (S_ISCHR(st.st_mode) &&
	    ioctl(fd, DIOCGMEDIASIZE, &mediasize) == 0 &&
	    mediasize > 0) {
		is_disk_like = 1;
	}
#elif defined(__NetBSD__) || defined(__OpenBSD__)
	/* Net/OpenBSD: if it supports DIOCGDINFO ioctl, it's disk-like. */
	else if ((S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) &&
	    ioctl(fd, DIOCGDINFO, &dl) == 0 &&
	    dl.d_partitions[DISKPART(st.st_rdev)].p_size > 0) {
		is_disk_like = 1;
	}
#elif defined(__DragonFly__)
	/* DragonFly BSD:  if it supports DIOCGPART ioctl, it's disk-like. */
	else if (S_ISCHR(st.st_mode) &&
	    ioctl(fd, DIOCGPART, &pi) == 0 &&
	    pi.media_size > 0) {
		is_disk_like = 1;
	}
#elif defined(__linux__)
	/* Linux:  All block devices are disk-like. */
	else if (S_ISBLK(st.st_mode) &&
	    lseek(fd, 0, SEEK_CUR) == 0 &&
	    lseek(fd, 0, SEEK_SET) == 0 &&
	    lseek(fd, 0, SEEK_END) > 0 &&
	    lseek(fd, 0, SEEK_SET) == 0) {
		is_disk_like = 1;
	}
#endif
	/* TODO: Add an "is_tape_like" variable and appropriate tests. */

	/* Disk-like devices prefer power-of-two block sizes.  */
	/* Use provided block_size as a guide so users have some control. */
	if (is_disk_like) {
		size_t new_block_size = 64 * 1024;
		while (new_block_size < mine->block_size
		    && new_block_size < 64 * 1024 * 1024)
			new_block_size *= 2;
		mine->block_size = new_block_size;
	}
	buffer = malloc(mine->block_size);
	if (buffer == NULL) {
		archive_set_error(a, ENOMEM, "No memory");
		goto fail;
	}
	mine->buffer = buffer;
	mine->fd = fd;
	/* Remember mode so close can decide whether to flush. */
	mine->st_mode = st.st_mode;

	/* Disk-like inputs can use lseek(). */
	if (is_disk_like) {
		mine->use_lseek = 1;
		mine->size = st.st_size;
	}

	return (ARCHIVE_OK);
fail:
	/*
	 * Don't close file descriptors not opened or ones pointing referring
	 * to `FNT_STDIN`.
	 */
	if (fd != -1 && fd != 0)
		close(fd);
	return (ARCHIVE_FATAL);
}

static ssize_t
file_read(struct archive *a, void *client_data, const void **buff)
{
	struct read_file_data *mine = (struct read_file_data *)client_data;
	ssize_t bytes_read;

	/* TODO: If a recent lseek() operation has left us
	 * mis-aligned, read and return a short block to try to get
	 * us back in alignment. */

	/* TODO: Someday, try mmap() here; if that succeeds, give
	 * the entire file to libarchive as a single block.  That
	 * could be a lot faster than block-by-block manual I/O. */

	/* TODO: We might be able to improve performance on pipes and
	 * sockets by setting non-blocking I/O and just accepting
	 * whatever we get here instead of waiting for a full block
	 * worth of data. */

	*buff = mine->buffer;
	for (;;) {
		bytes_read = read(mine->fd, mine->buffer, mine->block_size);
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			else if (mine->filename_type == FNT_STDIN)
				archive_set_error(a, errno,
				    "Error reading stdin");
			else if (mine->filename_type == FNT_MBS)
				archive_set_error(a, errno,
				    "Error reading '%s'", mine->filename.m);
			else
				archive_set_error(a, errno,
				    "Error reading '%ls'", mine->filename.w);
		}
		return (bytes_read);
	}
}

/*
 * Regular files and disk-like block devices can use simple lseek
 * without needing to round the request to the block size.
 *
 * TODO: This can leave future reads mis-aligned.  Since we know the
 * offset here, we should store it and use it in file_read() above
 * to determine whether we should perform a short read to get back
 * into alignment.  Long series of mis-aligned reads can negatively
 * impact disk throughput.  (Of course, the performance impact should
 * be carefully tested; extra code complexity is only worthwhile if
 * it does provide measurable improvement.)
 *
 * TODO: Be lazy about the actual seek.  There are a few pathological
 * cases where libarchive makes a bunch of seek requests in a row
 * without any intervening reads.  This isn't a huge performance
 * problem, since the kernel handles seeks lazily already, but
 * it would be very slightly faster if we simply remembered the
 * seek request here and then actually performed the seek at the
 * top of the read callback above.
 */
static int64_t
file_skip_lseek(struct archive *a, void *client_data, int64_t request)
{
	struct read_file_data *mine = (struct read_file_data *)client_data;
#if defined(_WIN32) && !defined(__CYGWIN__)
	/* We use _lseeki64() on Windows. */
	int64_t old_offset, new_offset, skip = request;
#else
	off_t old_offset, new_offset, skip = (off_t)request;
#endif
	int skip_bits = sizeof(skip) * 8 - 1;

	/* We use off_t here because lseek() is declared that way. */

	/* Reduce a request that would overflow the 'skip' variable. */
	if (sizeof(request) > sizeof(skip)) {
		const int64_t max_skip =
		    (((int64_t)1 << (skip_bits - 1)) - 1) * 2 + 1;
		if (request > max_skip)
			skip = max_skip;
	}

	if ((old_offset = lseek(mine->fd, 0, SEEK_CUR)) >= 0) {
		if (old_offset >= mine->size ||
		    skip > mine->size - old_offset) {
			/* Do not seek past end of file. */
			errno = ESPIPE;
		} else if ((new_offset = lseek(mine->fd, skip, SEEK_CUR)) >= 0)
			return (new_offset - old_offset);
	}

	/* If lseek() fails, don't bother trying again. */
	mine->use_lseek = 0;

	/* Let libarchive recover with read+discard */
	if (errno == ESPIPE)
		return (0);

	/* If the input is corrupted or truncated, fail. */
	if (mine->filename_type == FNT_STDIN)
		archive_set_error(a, errno, "Error seeking in stdin");
	else if (mine->filename_type == FNT_MBS)
		archive_set_error(a, errno, "Error seeking in '%s'",
		    mine->filename.m);
	else
		archive_set_error(a, errno, "Error seeking in '%ls'",
		    mine->filename.w);
	return (-1);
}


/*
 * TODO: Implement another file_skip_XXXX that uses MTIO ioctls to
 * accelerate operation on tape drives.
 */

static int64_t
file_skip(struct archive *a, void *client_data, int64_t request)
{
	struct read_file_data *mine = (struct read_file_data *)client_data;

	/* Delegate skip requests. */
	if (mine->use_lseek)
		return (file_skip_lseek(a, client_data, request));

	/* If we can't skip, return 0; libarchive will read+discard instead. */
	return (0);
}

/*
 * TODO: Store the offset and use it in the read callback.
 */
static int64_t
file_seek(struct archive *a, void *client_data, int64_t request, int whence)
{
	struct read_file_data *mine = (struct read_file_data *)client_data;
	off_t seek = (off_t)request;
	int64_t r;
	int seek_bits = sizeof(seek) * 8 - 1;

	/* We use off_t here because lseek() is declared that way. */

	/* Reduce a request that would overflow the 'seek' variable. */
	if (sizeof(request) > sizeof(seek)) {
		const int64_t max_seek =
		    (((int64_t)1 << (seek_bits - 1)) - 1) * 2 + 1;
		const int64_t min_seek = ~max_seek;
		if (request > max_seek)
			seek = (off_t)max_seek;
		else if (request < min_seek)
			seek = (off_t)min_seek;
	}

	r = lseek(mine->fd, seek, whence);
	if (r >= 0)
		return r;

	/* If the input is corrupted or truncated, fail. */
	if (mine->filename_type == FNT_STDIN)
		archive_set_error(a, errno, "Error seeking in stdin");
	else if (mine->filename_type == FNT_MBS)
		archive_set_error(a, errno, "Error seeking in '%s'",
		    mine->filename.m);
	else
		archive_set_error(a, errno, "Error seeking in '%ls'",
		    mine->filename.w);
	return (ARCHIVE_FATAL);
}

static int
file_close2(struct archive *a, void *client_data)
{
	struct read_file_data *mine = (struct read_file_data *)client_data;

	(void)a; /* UNUSED */

	/* Only flush and close if open succeeded. */
	if (mine->fd >= 0) {
		/*
		 * Sometimes, we should flush the input before closing.
		 *   Regular files: faster to just close without flush.
		 *   Disk-like devices:  Ditto.
		 *   Tapes: must not flush (user might need to
		 *      read the "next" item on a non-rewind device).
		 *   Pipes and sockets:  must flush (otherwise, the
		 *      program feeding the pipe or socket may complain).
		 * Here, I flush everything except for regular files and
		 * device nodes.
		 */
		if (!S_ISREG(mine->st_mode)
		    && !S_ISCHR(mine->st_mode)
		    && !S_ISBLK(mine->st_mode)) {
			ssize_t bytesRead;
			do {
				bytesRead = read(mine->fd, mine->buffer,
				    mine->block_size);
			} while (bytesRead > 0);
		}
		/* If a named file was opened, then it needs to be closed. */
		if (mine->filename_type != FNT_STDIN)
			close(mine->fd);
	}
	free(mine->buffer);
	mine->buffer = NULL;
	mine->fd = -1;
	return (ARCHIVE_OK);
}

static int
file_close(struct archive *a, void *client_data)
{
	struct read_file_data *mine = (struct read_file_data *)client_data;
	file_close2(a, client_data);
	free(mine);
	return (ARCHIVE_OK);
}

static int
file_switch(struct archive *a, void *client_data1, void *client_data2)
{
	file_close2(a, client_data1);
	return file_open(a, client_data2);
}
