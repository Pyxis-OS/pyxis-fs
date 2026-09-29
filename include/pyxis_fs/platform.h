#ifndef PYXIS_FS_PLATFORM_H
#define PYXIS_FS_PLATFORM_H

#include <pyxis_fs/base.h>

/* Callbacks are synchronous and borrow buffers only until return. I/O returns
 * PFS_OK only after transferring every requested block; any other result becomes
 * PFS_IO. A failed read leaves its entire output buffer unspecified. */
typedef enum pfs_status (*pfs_read_blocks_fn)(void *context, uint64_t first,
                                            uint32_t count, void *buffer);
typedef enum pfs_status (*pfs_write_blocks_fn)(void *context, uint64_t first,
                                             uint32_t count, const void *buffer);
typedef enum pfs_status (*pfs_flush_fn)(void *context);

struct pfs_geometry {
  uint64_t block_count;
  uint32_t max_transfer_blocks;
};

/* Geometry is supplied by the trusted adapter, never taken from media. The
 * context and callback targets outlive the reader; the core does not close them.
 * Instances are used serially and are not mutated while a call is active. */
struct pfs_block_reader {
  void *context;
  struct pfs_geometry geometry;
  pfs_read_blocks_fn read;
};

/* Only construction receives this interface. A read-only reader contains no
 * write or flush callback. Flush is explicit; releasing state never flushes. */
struct pfs_block_builder {
  struct pfs_block_reader reader;
  pfs_write_blocks_fn write;
  pfs_flush_fn flush;
};

enum pfs_status pfs_block_reader_init(struct pfs_block_reader *reader,
                                      void *context,
                                      const struct pfs_geometry *geometry,
                                      pfs_read_blocks_fn read);
enum pfs_status pfs_block_builder_init(struct pfs_block_builder *builder,
                                       void *context,
                                       const struct pfs_geometry *geometry,
                                       pfs_read_blocks_fn read,
                                       pfs_write_blocks_fn write,
                                       pfs_flush_fn flush);
/* Buffer length must equal count * PFS_BLOCK_SIZE. Zero-block operations are
 * invalid. Transfer or geometry profile excess is LIMIT; bad ranges are INVALID. */
enum pfs_status pfs_block_read(const struct pfs_block_reader *reader,
                              uint64_t first, uint32_t count,
                              void *buffer, size_t length);
enum pfs_status pfs_block_write(const struct pfs_block_builder *builder,
                               uint64_t first, uint32_t count,
                               const void *buffer, size_t length);
enum pfs_status pfs_block_flush(const struct pfs_block_builder *builder);

/* Allocate returns NULL on failure, or size writable bytes aligned to alignment.
 * Alignment is a nonzero power of two, with no implicit size rounding. Free
 * receives exactly the pointer, size and alignment from successful allocation.
 * Allocated payload remains caller-owned until explicit free. */
typedef void *(*pfs_allocate_fn)(void *context, size_t size, size_t alignment);
typedef void (*pfs_free_fn)(void *context, void *data, size_t size, size_t alignment);

struct pfs_memory {
  void *context;
  pfs_allocate_fn allocate;
  pfs_free_fn free;
  uint64_t limit;
  uint64_t used;
};

/* Initialize handles to zero. Live handles and their memory owner are not copied,
 * modified or reinitialized; they outlive the allocation. Payload is caller owned
 * until free. These caller-supplied handles need no hidden heap allocation.
 * The cap charges requested bytes, not adapter-private allocator overhead. */
struct pfs_allocation {
  struct pfs_memory *owner;
  void *data;
  size_t size;
  size_t alignment;
};

/* A zero limit selects PFS_MEMORY_DEFAULT. Explicit limits may be any positive
 * byte count through PFS_MEMORY_MAX. The adapter context outlives all allocations.
 * Calls and the owner's counters are serial; no internal locking is supplied. */
enum pfs_status pfs_memory_init(struct pfs_memory *memory, void *context,
                               pfs_allocate_fn allocate, pfs_free_fn free,
                               uint64_t limit);
/* Empty output remains empty on failure. Cap exhaustion is LIMIT, allocation
 * failure below the cap is NO_MEMORY. Invalid size/alignment or a misaligned
 * allocator result is INVALID; a misaligned result is freed before return. */
enum pfs_status pfs_memory_allocate(struct pfs_memory *memory, size_t size,
                                   size_t alignment,
                                   struct pfs_allocation *allocation);
/* Free clears the handle. Free of an already empty handle is successful. */
enum pfs_status pfs_memory_free(struct pfs_memory *memory,
                               struct pfs_allocation *allocation);

#endif
