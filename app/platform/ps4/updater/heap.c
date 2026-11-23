/*
 * The update helper's heap. Ported from Gold Torrent's ps4/daemon_malloc.c.
 *
 * OpenOrbis' musl malloc (malloc.lo in libc.a) reserves and maps a fixed 2.5 GiB of system
 * flexible memory on first use, and when that fails still calls sceLibcMspaceMalloc with a null
 * mspace. An app gets that much memory; a system daemon gets far less, so a daemon linked with it
 * dies on its first allocation, before main.
 *
 * This file defines every symbol malloc.lo provides, so the linker never pulls it in (a missed one
 * would fail the link with a duplicate definition); it must be a source of the executable itself,
 * not of a library. The heap starts at 32 MiB and halves on failure down to 4 MiB, first as system
 * flexible memory (what OpenOrbis uses), then as plain flexible memory.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

typedef void* mspace_t;
mspace_t sceLibcMspaceCreate(char const* name, void* base, size_t capacity, unsigned flags);
int sceLibcMspaceDestroy(mspace_t m);
void* sceLibcMspaceMalloc(mspace_t m, size_t size);
int sceLibcMspaceFree(mspace_t m, void* p);
void* sceLibcMspaceCalloc(mspace_t m, size_t n, size_t size);
void* sceLibcMspaceRealloc(mspace_t m, void* p, size_t size);
void* sceLibcMspaceMemalign(mspace_t m, size_t align, size_t size);
int sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t align);
int sceKernelMapNamedSystemFlexibleMemory(void** addr, size_t len, int prot, int flags, char const* name);
int sceKernelMapNamedFlexibleMemory(void** addr, size_t len, int prot, int flags, char const* name);
int sceKernelMunmap(void* addr, size_t len);

#define HEAP_NAME "gmca updater heap"
#define HEAP_FIRST_MB 32
#define HEAP_MIN_MB 4

static mspace_t s_mspace;
static void* s_mem;
static size_t s_size;

static int try_map(size_t size, int system)
{
	void* addr = 0;
	if (sceKernelReserveVirtualRange(&addr, size, 0, 16384) < 0) return 0;
	/* prot 2 and MAP_FIXED (0x10), as OpenOrbis' malloc maps it */
	int const r = system ? sceKernelMapNamedSystemFlexibleMemory(&addr, size, 2, 0x10, HEAP_NAME)
	                     : sceKernelMapNamedFlexibleMemory(&addr, size, 3, 0x10, HEAP_NAME);
	if (r < 0)
	{
		sceKernelMunmap(addr, size);
		return 0;
	}
	mspace_t const m = sceLibcMspaceCreate(HEAP_NAME, addr, size, 0);
	if (!m)
	{
		sceKernelMunmap(addr, size);
		return 0;
	}
	s_mspace = m;
	s_mem = addr;
	s_size = size;
	return 1;
}

int malloc_init(void)
{
	if (s_mspace) return 0;
	size_t const mb = 1024 * 1024;
	for (int system = 1; system >= 0; --system)
		for (size_t size = (size_t)HEAP_FIRST_MB * mb; size >= (size_t)HEAP_MIN_MB * mb; size /= 2)
			if (try_map(size, system)) return 0;
	return 1;
}

int malloc_finalize(void)
{
	if (!s_mspace) return 0;
	sceLibcMspaceDestroy(s_mspace);
	sceKernelMunmap(s_mem, s_size);
	s_mspace = 0;
	return 0;
}

static int ready(void)
{
	if (!s_mspace) malloc_init();
	if (!s_mspace)
	{
		errno = ENOMEM;
		return 0;
	}
	return 1;
}

void* malloc(size_t size) { return ready() ? sceLibcMspaceMalloc(s_mspace, size) : 0; }

void free(void* p)
{
	if (p && s_mspace) sceLibcMspaceFree(s_mspace, p);
}

void* calloc(size_t n, size_t size) { return ready() ? sceLibcMspaceCalloc(s_mspace, n, size) : 0; }

void* realloc(void* p, size_t size) { return ready() ? sceLibcMspaceRealloc(s_mspace, p, size) : 0; }

void* __memalign(size_t align, size_t size) { return ready() ? sceLibcMspaceMemalign(s_mspace, align, size) : 0; }

void* memalign(size_t align, size_t size) { return __memalign(align, size); }
