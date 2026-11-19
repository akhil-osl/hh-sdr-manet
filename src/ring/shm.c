#include "hhsdr/ring/shm.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Copies the path into the handle. False if it is empty or does not fit. */
static int set_path(hh_ring_shm_t *h, const char *path)
{
    if (!path || *path == '\0' || strlen(path) >= sizeof h->path) return 0;
    memset(h, 0, sizeof *h);
    strcpy(h->path, path);
    return 1;
}

/* Maps fd for size bytes into the handle. */
static hh_status_t map_fd(hh_ring_shm_t *h, int fd, size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (p == MAP_FAILED) return HH_ERR_IO;
    h->base = p;
    h->size = size;
    return HH_OK;
}

hh_status_t hh_ring_shm_create(hh_ring_shm_t *h, hh_ring_region_t *region, const char *path,
                               uint32_t rx_slots, uint32_t tx_slots, uint32_t slot_size)
{
    size_t size;
    int fd;
    hh_status_t st;

    if (!h || !region || !set_path(h, path)) return HH_ERR_INVAL;
    size = hh_ring_region_size(rx_slots, tx_slots, slot_size);
    if (size == 0) return HH_ERR_INVAL;

    /* A stale file is replaced, not truncated: a process still mapping the old
     * one keeps its own memory and cannot be hit by a shrinking file. */
    unlink(path);
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0660);
    if (fd < 0) return HH_ERR_IO;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        unlink(path);
        return HH_ERR_IO;
    }
    st = map_fd(h, fd, size);
    close(fd);
    if (st == HH_OK) st = hh_ring_region_init(h->base, size, rx_slots, tx_slots, slot_size);
    if (st == HH_OK) st = hh_ring_region_attach(region, h->base, size);
    if (st != HH_OK) {
        if (h->base) munmap(h->base, size);
        unlink(path);
        memset(h, 0, sizeof *h);
        return st;
    }
    h->owner = 1;
    return HH_OK;
}

hh_status_t hh_ring_shm_open(hh_ring_shm_t *h, hh_ring_region_t *region, const char *path)
{
    struct stat sb;
    int fd;
    hh_status_t st;

    if (!h || !region || !set_path(h, path)) return HH_ERR_INVAL;
    fd = open(path, O_RDWR);
    if (fd < 0) return HH_ERR_IO;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size <= 0) {
        close(fd);
        return HH_ERR_INVAL;
    }
    st = map_fd(h, fd, (size_t)sb.st_size);
    close(fd);
    if (st == HH_OK) st = hh_ring_region_attach(region, h->base, h->size);
    if (st != HH_OK) {
        if (h->base) munmap(h->base, h->size);
        memset(h, 0, sizeof *h);
    }
    return st;
}

void hh_ring_shm_close(hh_ring_shm_t *h)
{
    if (!h) return;
    if (h->base) munmap(h->base, h->size);
    if (h->owner && h->path[0] != '\0') unlink(h->path);
    memset(h, 0, sizeof *h);
}
