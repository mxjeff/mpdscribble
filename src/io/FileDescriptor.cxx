// SPDX-License-Identifier: BSD-2-Clause
// author: Max Kellermann <max.kellermann@gmail.com>

#include "FileDescriptor.hxx"
#include "UniqueFileDescriptor.hxx"
#include "system/Error.hxx"

#include <cassert>
#include <stdexcept>

#include <sys/stat.h>
#include <fcntl.h>

#ifndef _WIN32
#include <poll.h>
#endif

#ifdef __linux__
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#endif

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef _WIN32

bool
FileDescriptor::IsValid() const noexcept
{
	return IsDefined() && fcntl(fd, F_GETFL) >= 0;
}

bool
FileDescriptor::IsRegularFile() const noexcept
{
	struct stat st;
	return IsDefined() && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

bool
FileDescriptor::IsPipe() const noexcept
{
	struct stat st;
	return IsDefined() && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

bool
FileDescriptor::IsSocket() const noexcept
{
	struct stat st;
	return IsDefined() && fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode);
}

#endif

#ifdef __linux__

bool
FileDescriptor::Open(FileDescriptor dir, const char *pathname,
		     int flags, mode_t mode) noexcept
{
	fd = ::openat(dir.Get(), pathname, flags | O_NOCTTY | O_CLOEXEC, mode);
	return IsDefined();
}

#endif

bool
FileDescriptor::Open(const char *pathname, int flags, mode_t mode) noexcept
{
	fd = ::open(pathname, flags | O_NOCTTY | O_CLOEXEC, mode);
	return IsDefined();
}

#ifdef _WIN32

bool
FileDescriptor::Open(const wchar_t *pathname, int flags, mode_t mode) noexcept
{
	fd = ::_wopen(pathname, flags | O_NOCTTY | O_CLOEXEC, mode);
	return IsDefined();
}

#endif

bool
FileDescriptor::OpenReadOnly(const char *pathname) noexcept
{
	return Open(pathname, O_RDONLY);
}

#ifdef __linux__

bool
FileDescriptor::OpenReadOnly(FileDescriptor dir, const char *pathname) noexcept
{
	return Open(dir, pathname, O_RDONLY);
}

#endif // __linux__

#ifndef _WIN32

bool
FileDescriptor::OpenNonBlocking(const char *pathname) noexcept
{
	return Open(pathname, O_RDWR | O_NONBLOCK);
}

#endif

#ifdef __linux__

bool
FileDescriptor::CreatePipe(FileDescriptor &r, FileDescriptor &w,
			   int flags) noexcept
{
	int fds[2];
	const int result = pipe2(fds, flags);
	if (result < 0)
		return false;

	r = FileDescriptor(fds[0]);
	w = FileDescriptor(fds[1]);
	return true;
}

#endif

bool
FileDescriptor::CreatePipe(FileDescriptor &r, FileDescriptor &w) noexcept
{
#ifdef __linux__
	return CreatePipe(r, w, O_CLOEXEC);
#else
	int fds[2];

#ifdef _WIN32
	const int result = _pipe(fds, 512, _O_BINARY);
#else
	const int result = pipe(fds);
#endif

	if (result < 0)
		return false;

	r = FileDescriptor(fds[0]);
	w = FileDescriptor(fds[1]);
	return true;
#endif
}

#ifndef _WIN32

bool
FileDescriptor::CreatePipeNonBlock(FileDescriptor &r,
				   FileDescriptor &w) noexcept
{
#ifdef __linux__
	return CreatePipe(r, w, O_CLOEXEC|O_NONBLOCK);
#else
	if (!CreatePipe(r, w))
		return false;

	r.SetNonBlocking();
	w.SetNonBlocking();
	return true;
#endif
}

void
FileDescriptor::SetNonBlocking() noexcept
{
	assert(IsDefined());

	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void
FileDescriptor::SetBlocking() noexcept
{
	assert(IsDefined());

	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

void
FileDescriptor::EnableCloseOnExec() noexcept
{
	assert(IsDefined());

	const int old_flags = fcntl(fd, F_GETFD, 0);
	fcntl(fd, F_SETFD, old_flags | FD_CLOEXEC);
}

void
FileDescriptor::DisableCloseOnExec() noexcept
{
	assert(IsDefined());

	const int old_flags = fcntl(fd, F_GETFD, 0);
	fcntl(fd, F_SETFD, old_flags & ~FD_CLOEXEC);
}

UniqueFileDescriptor
FileDescriptor::Duplicate() const noexcept
{
	return UniqueFileDescriptor{::dup(Get())};
}

bool
FileDescriptor::CheckDuplicate(FileDescriptor new_fd) noexcept
{
	if (*this == new_fd) {
		DisableCloseOnExec();
		return true;
	} else
		return Duplicate(new_fd);
}

#endif

bool
FileDescriptor::Rewind() noexcept
{
	assert(IsDefined());

	return lseek(fd, 0, SEEK_SET) == 0;
}

off_t
FileDescriptor::GetSize() const noexcept
{
	struct stat st;
	return ::fstat(fd, &st) >= 0
		? (long)st.st_size
		: -1;
}

void
FileDescriptor::FullRead(void *_buffer, std::size_t length)
{
	auto buffer = (std::byte *)_buffer;

	while (length > 0) {
		ssize_t nbytes = Read(buffer, length);
		if (nbytes <= 0) {
			if (nbytes < 0)
				throw MakeErrno("Failed to read");
			throw std::runtime_error("Unexpected end of file");
		}

		buffer += nbytes;
		length -= nbytes;
	}
}

void
FileDescriptor::FullWrite(const void *_buffer, std::size_t length)
{
	auto buffer = (const std::byte *)_buffer;

	while (length > 0) {
		ssize_t nbytes = Write(buffer, length);
		if (nbytes <= 0) {
			if (nbytes < 0)
				throw MakeErrno("Failed to write");
			throw std::runtime_error("Failed to write");
		}

		buffer += nbytes;
		length -= nbytes;
	}
}

#ifndef _WIN32

int
FileDescriptor::Poll(short events, int timeout) const noexcept
{
	assert(IsDefined());

	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	int result = poll(&pfd, 1, timeout);
	return result > 0
		? pfd.revents
		: result;
}

int
FileDescriptor::WaitReadable(int timeout) const noexcept
{
	return Poll(POLLIN, timeout);
}

int
FileDescriptor::WaitWritable(int timeout) const noexcept
{
	return Poll(POLLOUT, timeout);
}

bool
FileDescriptor::IsReadyForWriting() const noexcept
{
	return WaitWritable(0) > 0;
}

#endif
