#include <libriscv/machine.hpp>

#define SYSCALL_VERBOSE 1
#ifdef SYSCALL_VERBOSE
#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
static constexpr bool verbose_syscalls = true;
#else
#define SYSPRINT(fmt, ...) /* fmt */
static constexpr bool verbose_syscalls = false;
#endif

#include <linux/limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>

namespace riscv {
	template <int W>
	void add_socket_syscalls(Machine<W>&);

template <int W>
struct guest_iovec {
	address_type<W> iov_base;
	address_type<W> iov_len;
};

template <int W>
static void syscall_stub_zero(Machine<W>& machine) {
	SYSPRINT("SYSCALL stubbed (zero): %d\n", (int)machine.cpu.reg(17));
	machine.set_result(0);
}

template <int W>
static void syscall_stub_nosys(Machine<W>& machine) {
	SYSPRINT("SYSCALL stubbed (nosys): %d\n", (int)machine.cpu.reg(17));
	machine.set_result(-ENOSYS);
}

template <int W>
static void syscall_exit(Machine<W>& machine)
{
	// Stop sets the max instruction counter to zero, allowing most
	// instruction loops to end. It is, however, not the only way
	// to exit a program. Tighter integrations with the library should
	// provide their own methods.
	machine.stop();
}

template <int W>
static void syscall_ebreak(riscv::Machine<W>& machine)
{
	printf("\n>>> EBREAK at %#lX\n", (long) machine.cpu.pc());
#ifdef RISCV_DEBUG
	machine.print_and_pause();
#else
	throw MachineException(UNHANDLED_SYSCALL, "EBREAK instruction");
#endif
}

static inline bool is_exception_signal(int sig) {
	// SIGILL, SIGABRT, SIGFPE, SIGSEGV
	return sig == 4 || sig == 6 || sig == 8 || sig == 11;
}

template <int W>
static void syscall_sigaction(Machine<W>& machine)
{
	const int signal = machine.template sysarg<address_type<W>>(0);
	const auto buffer = machine.template sysarg<address_type<W>>(1);
	struct sigaction sa;
	machine.copy_from_guest(&sa, buffer, sizeof(sa));

	if (is_exception_signal(signal)) {
		// There is typically only one relevant handler,
		// and languages use it to print backtraces.
		//printf("Signal %d handler: 0x%lX\n", signal, (uintptr_t)sa.sa_handler);
		machine.set_sighandler((address_type<W>)(uintptr_t)sa.sa_handler);
	}
	machine.set_result(0);
}

template <int W>
void syscall_lseek(Machine<W>& machine)
{
	const int fd      = machine.template sysarg<int>(0);
	const auto offset = machine.template sysarg<int64_t>(1);
	const int whence  = machine.template sysarg<int>(2);
	SYSPRINT("SYSCALL lseek, fd: %d, offset: 0x%lX, whence: %d\n",
		fd, (long)offset, whence);

	const int real_fd = machine.fds().get(fd);
	int64_t res = lseek(real_fd, offset, whence);
	if (res >= 0) {
		machine.set_result(res);
	} else {
		machine.set_result(-errno);
	}
}
template <int W>
static void syscall_read(Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<address_type<W>>(1);
	const size_t len   = machine.template sysarg<address_type<W>>(2);
	SYSPRINT("SYSCALL read, addr: 0x%lX, len: %zu\n", (long)address, len);
	// We have special stdin handling
	if (fd == 0) {
		// Gather up to 64kb in pages we can read into
		riscv::vBuffer buffers[16];
		size_t cnt =
			machine.memory.gather_buffers_from_range(16, buffers, address, len);
		for (size_t i = 0; i < cnt; i++) {
			machine.stdin(buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	} else if (machine.has_file_descriptors()) {
		const int real_fd = machine.fds().get(fd);
		// Gather up to 1MB of pages we can read into
		riscv::vBuffer buffers[256];
		size_t cnt =
			machine.memory.gather_buffers_from_range(256, buffers, address, len);
		// Could probably be a readv call, tbh
		for (size_t i = 0; i < cnt; i++) {
			read(real_fd, buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	}
	machine.set_result(-EBADF);
}
template <int W>
static void syscall_write(Machine<W>& machine)
{
	const int  fd      = machine.template sysarg<int>(0);
	const auto address = machine.template sysarg<address_type<W>>(1);
	const size_t len   = machine.template sysarg<address_type<W>>(2);
	SYSPRINT("SYSCALL write, addr: 0x%lX, len: %zu\n", (long)address, len);
	// We only accept standard output pipes, for now :)
	if (fd == 1 || fd == 2) {
		// Zero-copy retrieval of buffers (64kb)
		riscv::vBuffer buffers[16];
		size_t cnt =
			machine.memory.gather_buffers_from_range(16, buffers, address, len);
		for (size_t i = 0; i < cnt; i++) {
			machine.print(buffers[i].ptr, buffers[i].len);
		}
		machine.set_result(len);
		return;
	} else if (machine.has_file_descriptors() && machine.fds().permit_file_write) {
		int real_fd = machine.fds().get(fd);
		// Zero-copy retrieval of buffers (256kb)
		riscv::vBuffer buffers[64];
		size_t cnt =
			machine.memory.gather_buffers_from_range(64, buffers, address, len);
		size_t bytes = 0;
		// Could probably be a writev call, tbh
		for (size_t i = 0; i < cnt; i++) {
			ssize_t res = write(real_fd, buffers[i].ptr, buffers[i].len);
			if (res >= 0) {
				bytes += res;
				// Detect partial writes
				if ((size_t)res < buffers[i].len) break;
			} else {
				// Detect write errors
				machine.set_result_or_error(res);
				return;
			}
		}
		machine.set_result(bytes);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_writev(Machine<W>& machine)
{
	const int  fd     = machine.template sysarg<int>(0);
	const auto iov_g  = machine.template sysarg<address_type<W>>(1);
	const auto count  = machine.template sysarg<int>(2);
	if constexpr (false) {
		printf("SYSCALL writev, iov: %#X  cnt: %d\n", iov_g, count);
	}
	if (count < 0 || count > 256) {
		machine.set_result(-EINVAL);
		return;
	}
	// We only accept standard output pipes, for now :)
	if (fd == 1 || fd == 2) {
		const size_t size = sizeof(guest_iovec<W>) * count;

		std::vector<guest_iovec<W>> vec(count);
		machine.memory.memcpy_out(vec.data(), iov_g, size);

		ssize_t res = 0;
		for (const auto& iov : vec)
		{
			auto src_g = (address_type<W>) iov.iov_base;
			auto len_g = (size_t) iov.iov_len;
			/* Zero-copy retrieval of buffers */
			riscv::vBuffer buffers[4];
			size_t cnt =
				machine.memory.gather_buffers_from_range(4, buffers, src_g, len_g);
			for (size_t i = 0; i < cnt; i++) {
				machine.print(buffers[i].ptr, buffers[i].len);
			}
			res += len_g;
		}
		machine.set_result(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_openat(Machine<W>& machine)
{
	const int dir_fd = machine.template sysarg<int>(0);
	const auto g_path = machine.template sysarg<address_type<W>>(1);
	const int flags  = machine.template sysarg<int>(2);
	char path[PATH_MAX];
	machine.copy_from_guest(path, g_path, sizeof(path)-1);
	path[sizeof(path)-1] = 0;

	SYSPRINT("SYSCALL openat, dir_fd: %d path: %s flags: %X\n",
		dir_fd, path, flags);

	if (machine.has_file_descriptors() && machine.fds().permit_filesystem) {

		if (machine.fds().filter_open != nullptr) {
			if (!machine.fds().filter_open(machine.template get_userdata<void>(), path)) {
				machine.set_result(-EPERM);
				return;
			}
		}
		int real_fd = openat(machine.fds().translate(dir_fd), path, flags);
		if (real_fd > 0) {
			const int vfd = machine.fds().assign(real_fd);
			machine.set_result(vfd);
		} else {
			// Translate errno() into kernel API return value
			machine.set_result(-errno);
		}
		return;
	}

	machine.set_result(-EBADF);
}

template <int W>
static void syscall_close(riscv::Machine<W>& machine)
{
	const int fd = machine.template sysarg<int>(0);
	if constexpr (verbose_syscalls) {
		printf("SYSCALL close, fd: %d\n", fd);
	}
	if (fd <= 2) {
		// TODO: Do we really want to close them?
		machine.set_result(0);
		return;
	} else if (machine.has_file_descriptors()) {
		machine.set_result(
			machine.fds().close(fd) >= 0 ? 0 : -EBADF);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_dup(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	SYSPRINT("SYSCALL dup, fd: %d\n", vfd);

	if (machine.has_file_descriptors()) {
		int real_fd = machine.fds().translate(vfd);
		int res = dup(real_fd);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_fcntl(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	const auto cmd = machine.template sysarg<int>(1);
	const auto arg1 = machine.template sysarg<address_type<W>>(2);
	const auto arg2 = machine.template sysarg<address_type<W>>(3);
	const auto arg3 = machine.template sysarg<address_type<W>>(4);
	SYSPRINT("SYSCALL fcntl, fd: %d  cmd: 0x%X\n", vfd, cmd);

	if (machine.has_file_descriptors()) {
		int real_fd = machine.fds().translate(vfd);
		int res = fcntl(real_fd, cmd, arg1, arg2, arg3);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_ioctl(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	const auto req = machine.template sysarg<uint64_t>(1);
	const auto arg1 = machine.template sysarg<address_type<W>>(2);
	const auto arg2 = machine.template sysarg<address_type<W>>(3);
	const auto arg3 = machine.template sysarg<address_type<W>>(4);
	const auto arg4 = machine.template sysarg<address_type<W>>(5);
	SYSPRINT("SYSCALL ioctl, fd: %d  req: 0x%lX\n", vfd, req);

	if (machine.has_file_descriptors()) {
		if (machine.fds().filter_ioctl != nullptr) {
			if (!machine.fds().filter_ioctl(machine.template get_userdata<void>(), req)) {
				machine.set_result(-EPERM);
				return;
			}
		}

		int real_fd = machine.fds().translate(vfd);
		int res = ioctl(real_fd, req, arg1, arg2, arg3, arg4);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
void syscall_readlinkat(Machine<W>& machine)
{
	const int vfd = machine.template sysarg<int>(0);
	const auto g_path = machine.template sysarg<address_type<W>>(1);
	const auto g_buf = machine.template sysarg<address_type<W>>(2);
	const auto bufsize = machine.template sysarg<address_type<W>>(3);

	char path[PATH_MAX];
	machine.copy_from_guest(path, g_path, sizeof(path)-1);
	path[sizeof(path)-1] = 0;

	SYSPRINT("SYSCALL readlinkat, fd: %d path: %s buffer: 0x%lX size: %zu\n",
		vfd, path, (long)g_buf, (size_t)bufsize);

	char buffer[16384];
	if (bufsize > sizeof(buffer)) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors()) {

		if (machine.fds().filter_open != nullptr) {
			if (!machine.fds().filter_open(machine.template get_userdata<void>(), path)) {
				machine.set_result(-EPERM);
				return;
			}
		}
		const int real_fd = machine.fds().translate(vfd);

		const int res = readlinkat(real_fd, path, buffer, bufsize);
		if (res > 0) {
			// TODO: Only necessary if g_buf is not sequential.
			machine.copy_to_guest(g_buf, buffer, res);
		}

		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}

// The RISC-V stat structure is different from x86
struct riscv_stat {
	uint64_t st_dev;		/* Device.  */
	uint64_t st_ino;		/* File serial number.  */
	uint32_t st_mode;	/* File mode.  */
	uint32_t st_nlink;	/* Link count.  */
	uint32_t st_uid;		/* User ID of the file's owner.  */
	uint32_t st_gid;		/* Group ID of the file's group. */
	uint64_t st_rdev;	/* Device number, if device.  */
	uint64_t __pad1;
	int64_t  st_size;	/* Size of file, in bytes.  */
	int32_t  st_blksize;	/* Optimal block size for I/O.  */
	int32_t  __pad2;
	int64_t  st_blocks;	/* Number 512-byte blocks allocated. */
	int64_t  rv_atime;	/* Time of last access.  */
	uint64_t rv_atime_nsec;
	int64_t  rv_mtime;	/* Time of last modification.  */
	uint64_t rv_mtime_nsec;
	int64_t  rv_ctime;	/* Time of last status change.  */
	uint64_t rv_ctime_nsec;
	uint32_t __unused4;
	uint32_t __unused5;
};
inline void copy_stat_buffer(struct stat& st, struct riscv_stat& rst)
{
	rst.st_dev = st.st_dev;
	rst.st_ino = st.st_ino;
	rst.st_mode = st.st_mode;
	rst.st_nlink = st.st_nlink;
	rst.st_uid = st.st_uid;
	rst.st_gid = st.st_gid;
	rst.st_rdev = st.st_rdev;
	rst.st_size = st.st_size;
	rst.st_blksize = st.st_blksize;
	rst.st_blocks = st.st_blocks;
	rst.rv_atime = st.st_atime;
	rst.rv_atime_nsec = st.st_atim.tv_nsec;
	rst.rv_mtime = st.st_mtime;
	rst.rv_mtime_nsec = st.st_mtim.tv_nsec;
	rst.rv_ctime = st.st_ctime;
	rst.rv_ctime_nsec = st.st_ctim.tv_nsec;
}

template <int W>
static void syscall_fstatat(Machine<W>& machine)
{
	const auto vfd = machine.template sysarg<int> (0);
	const auto g_path = machine.template sysarg<address_type<W>> (1);
	const auto g_buf = machine.template sysarg<address_type<W>> (2);
	const auto flags = machine.template sysarg<int> (3);

	char path[PATH_MAX];
	machine.copy_from_guest(path, g_path, sizeof(path)-1);
	path[sizeof(path)-1] = 0;

	SYSPRINT("SYSCALL fstatat, fd: %d path: %s buf: 0x%lX flags: %#x)\n",
			vfd, path, (long)g_buf, flags);

	if (machine.has_file_descriptors()) {

		int real_fd = machine.fds().translate(vfd);

		struct stat st;
		const int res = ::fstatat(real_fd, path, &st, flags);
		if (res == 0) {
			// Convert to RISC-V structure
			struct riscv_stat rst;
			copy_stat_buffer(st, rst);
			machine.copy_to_guest(g_buf, &rst, sizeof(rst));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}

template <int W>
static void syscall_fstat(Machine<W>& machine)
{
	const auto vfd = machine.template sysarg<int> (0);
	const auto g_buf = machine.template sysarg<address_type<W>> (1);

	SYSPRINT("SYSCALL fstat, fd: %d buf: 0x%lX)\n",
			vfd, (long)g_buf);

	if (machine.has_file_descriptors()) {

		int real_fd = machine.fds().translate(vfd);

		struct stat st;
		int res = ::fstat(real_fd, &st);
		if (res == 0) {
			// Convert to RISC-V structure
			struct riscv_stat rst;
			copy_stat_buffer(st, rst);
			machine.copy_to_guest(g_buf, &rst, sizeof(rst));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}
template <int W>
static void syscall_statx(Machine<W>& machine)
{
	const int   dir_fd = machine.template sysarg<int> (0);
	const auto  g_path = machine.template sysarg<address_type<W>> (1);
	const int    flags = machine.template sysarg<int> (2);
	const auto    mask = machine.template sysarg<uint32_t> (3);
	const auto  buffer = machine.template sysarg<address_type<W>> (4);

	char path[PATH_MAX];
	machine.copy_from_guest(path, g_path, sizeof(path)-1);
	path[sizeof(path)-1] = 0;

	SYSPRINT("SYSCALL statx, fd: %d path: %s flags: %x buf: 0x%lX)\n",
			dir_fd, path, flags, (long)buffer);

	if (machine.has_file_descriptors()) {
		if (machine.fds().filter_stat != nullptr) {
			if (!machine.fds().filter_stat(machine.template get_userdata<void>(), path)) {
				machine.set_result(-EPERM);
				return;
			}
		}

		struct statx st;
		int res = ::statx(dir_fd, path, flags, mask, &st);
		if (res == 0) {
			machine.copy_to_guest(buffer, &st, sizeof(struct statx));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-ENOSYS);
}

template <int W>
static void syscall_gettimeofday(Machine<W>& machine)
{
	const auto buffer = machine.template sysarg<address_type<W>>(0);
	SYSPRINT("SYSCALL gettimeofday, buffer: 0x%lX\n", (long)buffer);
	struct timeval tv;
	const int res = gettimeofday(&tv, nullptr);
	if (res >= 0) {
		if constexpr (W == 4) {
			int32_t timeval32[2] = { (int) tv.tv_sec, (int) tv.tv_usec };
			machine.copy_to_guest(buffer, timeval32, sizeof(timeval32));
		} else {
			machine.copy_to_guest(buffer, &tv, sizeof(tv));
		}
	}
	machine.set_result_or_error(res);
}
template <int W>
static void syscall_clock_gettime(Machine<W>& machine)
{
	const auto clkid = machine.template sysarg<int>(0);
	const auto buffer = machine.template sysarg<address_type<W>>(1);
	SYSPRINT("SYSCALL clock_gettime, clkid: %x buffer: 0x%lX\n",
		clkid, (long)buffer);

	struct timespec ts;
	const int res = clock_gettime(clkid, &ts);
	if (res >= 0) {
		machine.copy_to_guest(buffer, &ts, sizeof(ts));
	}
	machine.set_result_or_error(res);
}

template <int W>
static void syscall_uname(Machine<W>& machine)
{
	const auto buffer = machine.template sysarg<address_type<W>>(0);
	SYSPRINT("SYSCALL uname, buffer: 0x%lX\n", (long)buffer);
	static constexpr int UTSLEN = 65;
	struct {
		char sysname [UTSLEN];
		char nodename[UTSLEN];
		char release [UTSLEN];
		char version [UTSLEN];
		char machine [UTSLEN];
		char domain  [UTSLEN];
	} uts;
	strcpy(uts.sysname, "RISC-V C++ Emulator");
	strcpy(uts.nodename,"libriscv");
	strcpy(uts.release, "5.0.0");
	strcpy(uts.version, "");
	if constexpr (W == 4)
		strcpy(uts.machine, "rv32imafdc");
	else if constexpr (W == 8)
		strcpy(uts.machine, "rv64imafdc");
	else
		strcpy(uts.machine, "rv128imafdc");
	strcpy(uts.domain,  "(none)");

	machine.copy_to_guest(buffer, &uts, sizeof(uts));
	machine.set_result(0);
}

template <int W>
static void syscall_brk(Machine<W>& machine)
{
	auto new_end = machine.template sysarg<address_type<W>>(0);
	if (new_end > machine.memory.heap_address() + Memory<W>::BRK_MAX) {
		new_end = machine.memory.heap_address() + Memory<W>::BRK_MAX;
	} else if (new_end < machine.memory.heap_address()) {
		new_end = machine.memory.heap_address();
	}

	if constexpr (verbose_syscalls) {
		printf("SYSCALL brk, new_end: 0x%lX\n", (long)new_end);
	}
	machine.set_result(new_end);
}

template <int W>
static void add_mman_syscalls(Machine<W>& machine)
{
	// munmap
	machine.install_syscall_handler(215,
	[] (Machine<W>& machine) {
		const auto addr = machine.template sysarg<address_type<W>> (0);
		const auto len  = machine.template sysarg<address_type<W>> (1);
		SYSPRINT(">>> munmap(0x%lX, len=%zu)\n", (long)addr, (size_t)len);
		machine.memory.free_pages(addr, len);
		auto& nextfree = machine.memory.mmap_address();
		if (addr + len == nextfree) {
			nextfree = addr;
			if (nextfree < machine.memory.heap_address() + Memory<W>::BRK_MAX)
				nextfree = machine.memory.heap_address() + Memory<W>::BRK_MAX;
		}
		machine.set_result(0);
	});
	// mmap
	machine.install_syscall_handler(222,
	[] (Machine<W>& machine) {
		const auto addr_g = machine.template sysarg<address_type<W>>(0);
		const auto length = machine.template sysarg<address_type<W>>(1);
		const auto prot   = machine.template sysarg<int>(2);
		const auto flags  = machine.template sysarg<int>(3);
		SYSPRINT(">>> mmap(addr 0x%lX, len %zu, prot %#x, flags %#X)\n",
				(long)addr_g, (size_t)length, prot, flags);
		if (addr_g % Page::size() != 0 || length % Page::size() != 0) {
			machine.set_result(-1); // = MAP_FAILED;
			return;
		}
		auto& nextfree = machine.memory.mmap_address();
		if (addr_g == 0 || addr_g == nextfree)
		{
			// anon pages need to be zeroed
			if (flags & MAP_ANONYMOUS) {
				// ... but they are already CoW
				//machine.memory.memset(nextfree, 0, length);
			}
			machine.set_result(nextfree);
			nextfree += length;
			return;
		} else if (addr_g < nextfree) {
			printf("Invalid mapping attempted\n");
			machine.set_result(-1); // = MAP_FAILED;
			return;
		} else { // addr_g != 0x0
			address_type<W> addr_end = addr_g + length;
			for (address_type<W> addr = addr_g; addr < addr_end; addr += Page::size())
			{
				// do nothing?
			}
			machine.set_result(addr_g);
			return;
		}
		(void) flags;
		(void) prot;
		machine.set_result(-1); // = MAP_FAILED;
	});
	// mremap
	machine.install_syscall_handler(163,
	[] (Machine<W>& machine) {
		const auto old_addr = machine.template sysarg<address_type<W>>(0);
		const auto old_size = machine.template sysarg<address_type<W>>(1);
		const auto new_size = machine.template sysarg<address_type<W>>(2);
		const auto flags    = machine.template sysarg<int>(3);
		SYSPRINT(">>> mremap(addr 0x%lX, len %zu, newsize %zu, flags %#X)\n",
				(long)old_addr, (size_t)old_size, (size_t)new_size, flags);
		auto& nextfree = machine.memory.mmap_address();
		// We allow the common case of reallocating the
		// last mapping to a bigger one
		if (old_addr + old_size == nextfree) {
			nextfree = old_addr + new_size;
			machine.set_result(old_addr);
			return;
		}
		(void) flags;
		machine.set_result(-1);
	});
	// mprotect
	machine.install_syscall_handler(226,
	[] (Machine<W>& machine) {
		const auto addr = machine.template sysarg<address_type<W>> (0);
		const auto len  = machine.template sysarg<address_type<W>> (1);
		const int  prot = machine.template sysarg<int> (2);
		SYSPRINT(">>> mprotect(0x%lX, len=%zu, prot=%x)\n",
			(long)addr, (size_t)len, prot);
		machine.memory.set_page_attr(addr, len, {
			.read  = bool(prot & 1),
			.write = bool(prot & 2),
			.exec  = bool(prot & 4)
		});
		machine.set_result(0);
	});
	// madvise
	machine.install_syscall_handler(233,
	[] (Machine<W>& machine) {
		const auto addr  = machine.template sysarg<address_type<W>> (0);
		const auto len   = machine.template sysarg<address_type<W>> (1);
		const int advice = machine.template sysarg<int> (2);
		SYSPRINT(">>> madvise(0x%lX, len=%zu, prot=%x)\n",
			(uint64_t)addr, (size_t)len, advice);
		switch (advice) {
			case MADV_NORMAL:
			case MADV_RANDOM:
			case MADV_SEQUENTIAL:
			case MADV_WILLNEED:
				machine.set_result(0);
				return;
			case MADV_DONTNEED:
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			case MADV_REMOVE:
			//case MADV_FREE:
				machine.memory.free_pages(addr, len);
				machine.set_result(0);
				return;
			default:
				machine.set_result(-EINVAL);
				return;
		}
	});
}

template <int W>
void Machine<W>::setup_minimal_syscalls()
{
	this->install_syscall_handler(SYSCALL_EBREAK, syscall_ebreak<W>);
	this->install_syscall_handler(62, syscall_lseek<W>);
	this->install_syscall_handler(63, syscall_read<W>);
	this->install_syscall_handler(64, syscall_write<W>);
	this->install_syscall_handler(93, syscall_exit<W>);
}

template <int W>
void Machine<W>::setup_newlib_syscalls()
{
	this->setup_minimal_syscalls();
	this->install_syscall_handler(214, syscall_brk<W>);
	add_mman_syscalls(*this);
}

template <int W>
void Machine<W>::setup_linux_syscalls(bool filesystem, bool sockets)
{
	this->setup_minimal_syscalls();

	// dup
	this->install_syscall_handler(23, syscall_dup<W>);
	// fcntl
	this->install_syscall_handler(25, syscall_fcntl<W>);
	// ioctl
	this->install_syscall_handler(29, syscall_ioctl<W>);
	// faccessat
	this->install_syscall_handler(48, syscall_stub_nosys<W>);

	this->install_syscall_handler(56, syscall_openat<W>);
	this->install_syscall_handler(57, syscall_close<W>);
	this->install_syscall_handler(66, syscall_writev<W>);
	this->install_syscall_handler(78, syscall_readlinkat<W>);
	// 79: fstatat
	this->install_syscall_handler(79, syscall_fstatat<W>);
	// 80: fstat
	this->install_syscall_handler(80, syscall_fstat<W>);

	// clock_gettime
	this->install_syscall_handler(113, syscall_clock_gettime<W>);
	// rt_sigaction
	this->install_syscall_handler(134, syscall_sigaction<W>);
	// rt_sigprocmask
	this->install_syscall_handler(135, syscall_stub_zero<W>);

	// gettimeofday
	this->install_syscall_handler(169, syscall_gettimeofday<W>);
	// getpid
	this->install_syscall_handler(172, syscall_stub_zero<W>);
	// getuid
	this->install_syscall_handler(174, syscall_stub_zero<W>);
	// geteuid
	this->install_syscall_handler(175, syscall_stub_zero<W>);
	// getgid
	this->install_syscall_handler(176, syscall_stub_zero<W>);
	// getegid
	this->install_syscall_handler(177, syscall_stub_zero<W>);

	this->install_syscall_handler(160, syscall_uname<W>);
	this->install_syscall_handler(214, syscall_brk<W>);

	add_mman_syscalls(*this);

	if (filesystem || sockets) {
		m_fds.reset(new FileDescriptors);
		if (sockets)
			add_socket_syscalls(*this);
	}

	// statx
	this->install_syscall_handler(291, syscall_statx<W>);
}

template void Machine<4>::setup_minimal_syscalls();
template void Machine<4>::setup_newlib_syscalls();
template void Machine<4>::setup_linux_syscalls(bool, bool);

template void Machine<8>::setup_minimal_syscalls();
template void Machine<8>::setup_newlib_syscalls();
template void Machine<8>::setup_linux_syscalls(bool, bool);

} // riscv