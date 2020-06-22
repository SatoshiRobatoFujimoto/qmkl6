#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>

#include <drm_v3d.h>

#include "qmkl6.h"
#include "qmkl6_internal.h"


static MKLExitHandler exit_handler = exit;

int mkl_set_exit_hander(const MKLExitHandler myexit)
{
    exit_handler = (myexit == NULL) ? exit : myexit;
    return 0;
}

void xerbla(const char * const srname, const int * const info,
        const int len [[maybe_unused]])
{
    fprintf(stderr, "QMKL6 error: %s: %d\n", srname, *info);
    exit_handler(EXIT_FAILURE);
}

double dsecond(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int drm_fd = -1;

static
void* alloc_memory(const size_t size, uint32_t * const handle,
        uint32_t * const bus_addr)
{
    int ret;

    ret = drm_v3d_create_bo(drm_fd, size, 0, handle, bus_addr);
    if (ret) {
        fprintf(stderr, "error: drm_v3d_create_bo: %s\n", strerror(errno));
        XERBLA(ret);
    }

    uint64_t mmap_offset;
    ret = drm_v3d_mmap_bo(drm_fd, *handle, 0, &mmap_offset);
    if (ret) {
        fprintf(stderr, "error: drm_v3d_mmap_bo: %s\n", strerror(errno));
        XERBLA(ret);
    }

    void * const map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
            drm_fd, mmap_offset);
    if (map == MAP_FAILED) {
        fprintf(stderr, "error: mmap: %s\n", strerror(errno));
        XERBLA(ret);
    }

    return map;
}

static
void free_memory(const size_t size, const uint32_t handle, void * const map)
{
    int ret;

    ret = munmap(map, size);
    if (ret) {
        fprintf(stderr, "error: munmap: %s\n", strerror(errno));
        XERBLA(ret);
    }

    ret = drm_gem_close(drm_fd, handle);
    if (ret) {
        fprintf(stderr, "error: drm_gem_close: %s\n", strerror(errno));
        XERBLA(ret);
    }
}

struct memory_area {
    size_t alloc_size;
    uint32_t handle, bus_addr;
    void *virt_addr;
};

static std::unordered_map <void*, struct memory_area> memory_map;

void* mkl_malloc(size_t alloc_size, int alignment)
{
    if (alignment <= 0 || alignment & (alignment - 1))
        alignment = 32;

    alloc_size += alignment - 1;

    uint32_t handle, bus_addr;
    void * const virt_addr = alloc_memory(alloc_size, &handle, &bus_addr);

    const uint32_t offset = ((uint32_t) alignment - bus_addr) & (alignment - 1);
    void * const virt_addr_aligned = (uint8_t*) virt_addr + offset;

    struct memory_area area = {
        .alloc_size = alloc_size,
        .handle = handle,
        .bus_addr = bus_addr,
        .virt_addr = virt_addr,
    };

    memory_map.emplace(virt_addr_aligned, area);

    return virt_addr_aligned;
}

void mkl_free(void * const a_ptr)
{
    if (a_ptr == NULL)
        return;

    const auto area = memory_map.find(a_ptr);
    if (area == memory_map.end()) {
        fprintf(stderr, "error: Memory area starting at %p is not known\n",
                a_ptr);
        XERBLA(1);
    }

    free_memory(area->second.alloc_size, area->second.handle,
            area->second.virt_addr);

    memory_map.erase(area);
}

uint64_t mkl_mem_stat(unsigned *AllocatedBuffers)
{
    *AllocatedBuffers = memory_map.size();

    uint64_t AllocatedBytes = 0;
    for (auto &mem : memory_map)
        AllocatedBytes += mem.second.alloc_size;
    return AllocatedBytes;
}

static int ncalls = 0;

void qmkl6_init_support(void)
{
    if (++ncalls != 1)
        return;

    const int fd = open("/dev/dri/card0", O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "error: open: %s\n", strerror(errno));
        XERBLA(fd);
    }
    drm_fd = fd;
}

void qmkl6_finalize_support(void)
{
    if (--ncalls != 0)
        return;

    int ret;

    ret = close(drm_fd);
    if (ret) {
        fprintf(stderr, "error: close: %s\n", strerror(errno));
        XERBLA(ret);
    }
    drm_fd = -1;
}
