#include <pyxis_fs/platform.h>

static enum pfs_status
geometry_validate(const struct pfs_geometry *geometry)
{
  if (geometry == NULL || geometry->block_count == 0 ||
      geometry->max_transfer_blocks == 0) {
    return PFS_INVALID;
  }
  if (geometry->block_count > PFS_POOL_BLOCKS_MAX) {
    return PFS_LIMIT;
  }
  return PFS_OK;
}

enum pfs_status
pfs_block_reader_init(struct pfs_block_reader *reader, void *context,
                      const struct pfs_geometry *geometry,
                      pfs_read_blocks_fn read)
{
  if (reader == NULL || read == NULL) {
    return PFS_INVALID;
  }
  enum pfs_status status = geometry_validate(geometry);
  if (status != PFS_OK) {
    return status;
  }

  *reader = (struct pfs_block_reader){
    .context = context,
    .geometry = *geometry,
    .read = read,
  };
  return PFS_OK;
}

enum pfs_status
pfs_block_builder_init(struct pfs_block_builder *builder, void *context,
                       const struct pfs_geometry *geometry,
                       pfs_read_blocks_fn read, pfs_write_blocks_fn write,
                       pfs_flush_fn flush)
{
  if (builder == NULL || write == NULL || flush == NULL) {
    return PFS_INVALID;
  }
  struct pfs_block_reader reader;
  enum pfs_status status = pfs_block_reader_init(&reader, context, geometry, read);
  if (status != PFS_OK) {
    return status;
  }

  *builder = (struct pfs_block_builder){
    .reader = reader,
    .write = write,
    .flush = flush,
  };
  return PFS_OK;
}

static enum pfs_status
transfer_validate(const struct pfs_block_reader *reader, uint64_t first,
                  uint32_t count, const void *buffer, size_t length)
{
  if (reader == NULL || reader->read == NULL || buffer == NULL || count == 0) {
    return PFS_INVALID;
  }
  enum pfs_status status = geometry_validate(&reader->geometry);
  if (status != PFS_OK) {
    return status;
  }
  if (count > PFS_IO_BLOCKS_MAX || count > reader->geometry.max_transfer_blocks) {
    return PFS_LIMIT;
  }
  size_t block_count = count;
  if (block_count > SIZE_MAX / PFS_BLOCK_SIZE) {
    return PFS_INVALID;
  }
  if (length != block_count * PFS_BLOCK_SIZE) {
    return PFS_INVALID;
  }
  if (first >= reader->geometry.block_count ||
      count > reader->geometry.block_count - first) {
    return PFS_INVALID;
  }
  return PFS_OK;
}

enum pfs_status
pfs_block_read(const struct pfs_block_reader *reader, uint64_t first,
               uint32_t count, void *buffer, size_t length)
{
  enum pfs_status status = transfer_validate(reader, first, count, buffer, length);
  if (status != PFS_OK) {
    return status;
  }
  return reader->read(reader->context, first, count, buffer) == PFS_OK ?
    PFS_OK : PFS_IO;
}

enum pfs_status
pfs_block_write(const struct pfs_block_builder *builder, uint64_t first,
                uint32_t count, const void *buffer, size_t length)
{
  if (builder == NULL || builder->write == NULL) {
    return PFS_INVALID;
  }
  enum pfs_status status = transfer_validate(&builder->reader, first, count,
                                            buffer, length);
  if (status != PFS_OK) {
    return status;
  }
  return builder->write(builder->reader.context, first, count, buffer) == PFS_OK ?
    PFS_OK : PFS_IO;
}

enum pfs_status
pfs_block_flush(const struct pfs_block_builder *builder)
{
  if (builder == NULL || builder->flush == NULL || builder->write == NULL ||
      builder->reader.read == NULL) {
    return PFS_INVALID;
  }
  enum pfs_status status = geometry_validate(&builder->reader.geometry);
  if (status != PFS_OK) {
    return status;
  }
  return builder->flush(builder->reader.context) == PFS_OK ? PFS_OK : PFS_IO;
}

enum pfs_status
pfs_memory_init(struct pfs_memory *memory, void *context,
                pfs_allocate_fn allocate, pfs_free_fn free, uint64_t limit)
{
  if (memory == NULL || allocate == NULL || free == NULL) {
    return PFS_INVALID;
  }
  if (limit == 0) {
    limit = PFS_MEMORY_DEFAULT;
  }
  if (limit > PFS_MEMORY_MAX) {
    return PFS_LIMIT;
  }

  *memory = (struct pfs_memory){
    .context = context,
    .allocate = allocate,
    .free = free,
    .limit = limit,
    .used = 0,
  };
  return PFS_OK;
}

static bool
allocation_empty(const struct pfs_allocation *allocation)
{
  return allocation->owner == NULL && allocation->data == NULL &&
    allocation->size == 0 && allocation->alignment == 0;
}

static bool
memory_valid(const struct pfs_memory *memory)
{
  return memory != NULL && memory->allocate != NULL && memory->free != NULL &&
    memory->limit > 0 && memory->limit <= PFS_MEMORY_MAX &&
    memory->used <= memory->limit;
}

enum pfs_status
pfs_memory_allocate(struct pfs_memory *memory, size_t size, size_t alignment,
                    struct pfs_allocation *allocation)
{
  if (!memory_valid(memory) || allocation == NULL || !allocation_empty(allocation) ||
      size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
    return PFS_INVALID;
  }
  if (size > memory->limit - memory->used) {
    return PFS_LIMIT;
  }

  void *data = memory->allocate(memory->context, size, alignment);
  if (data == NULL) {
    return PFS_NO_MEMORY;
  }
  if ((uintptr_t)data % alignment != 0) {
    memory->free(memory->context, data, size, alignment);
    return PFS_INVALID;
  }

  *allocation = (struct pfs_allocation){
    .owner = memory,
    .data = data,
    .size = size,
    .alignment = alignment,
  };
  memory->used += size;
  return PFS_OK;
}

enum pfs_status
pfs_memory_move(struct pfs_memory *memory, struct pfs_allocation *source,
                 struct pfs_allocation *destination)
{
  if (!memory_valid(memory) || source == NULL || destination == NULL ||
      source == destination || !allocation_empty(destination) ||
      source->owner != memory || source->data == NULL ||
      source->size == 0 || source->size > memory->used ||
      source->alignment == 0 ||
      (source->alignment & (source->alignment - 1)) != 0 ||
      (uintptr_t)source->data % source->alignment != 0) {
    return PFS_INVALID;
  }
  *destination = *source;
  *source = (struct pfs_allocation){0};
  return PFS_OK;
}

enum pfs_status
pfs_memory_free(struct pfs_memory *memory, struct pfs_allocation *allocation)
{
  if (!memory_valid(memory) || allocation == NULL) {
    return PFS_INVALID;
  }
  if (allocation_empty(allocation)) {
    return PFS_OK;
  }
  if (allocation->owner != memory || allocation->data == NULL ||
      allocation->size == 0 || allocation->size > memory->used ||
      allocation->alignment == 0 ||
      (allocation->alignment & (allocation->alignment - 1)) != 0 ||
      (uintptr_t)allocation->data % allocation->alignment != 0) {
    return PFS_INVALID;
  }

  memory->free(memory->context, allocation->data, allocation->size,
               allocation->alignment);
  memory->used -= allocation->size;
  *allocation = (struct pfs_allocation){0};
  return PFS_OK;
}
