/* SPDX-License-Identifier: MPL-2.0 */
#include "inspect.h"

#include <string.h>

void
inspect_usage(FILE *stream)
{
  fputs("Usage: pyxisfs-inspect --image PATH [--memory-limit SIZE]\n"
        "  [--gpt-partition N --sector-size 512|4096] COMMAND\n"
        "Commands:\n"
        "  info | volumes\n"
        "  list | stat --volume NAME --path PATH\n"
        "  access --volume NAME --root PATH --principal ID --target PATH\n"
        "    --scope object|subtree --rights LIST --ceiling LIST\n"
        "Use --volume-id ID instead of --volume NAME. Paths are root-relative; . is the root.\n"
        "Rights are comma-separated file.*, dir.*, admin.* tokens, or none.\n"
        "Inspection is diagnostic authority; access simulates supplied trusted inputs.\n", stream);
}

static enum pfs_status
decimal_u32(const char *text, uint32_t *value)
{
  if (*text == '\0') {
    return PFS_INVALID;
  }
  uint32_t result = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || result > (UINT32_MAX - (unsigned)(*text - '0')) / 10) {
      return PFS_INVALID;
    }
    result = result * 10 + (unsigned)(*text - '0');
  }
  *value = result;
  return PFS_OK;
}

static enum pfs_status
rights_parse(const char *text, struct pfs_rights *rights)
{
  static const struct {
    const char *name;
    unsigned domain;
    uint64_t bit;
  } tokens[] = {
    {"file.metadata", 0, PFS_FILE_METADATA},
    {"file.read", 0, PFS_FILE_READ},
    {"file.write", 0, PFS_FILE_WRITE},
    {"file.resize", 0, PFS_FILE_RESIZE},
    {"file.checkpoint", 0, PFS_FILE_CHECKPOINT},
    {"dir.metadata", 1, PFS_DIR_METADATA},
    {"dir.list", 1, PFS_DIR_LIST},
    {"dir.lookup", 1, PFS_DIR_LOOKUP},
    {"dir.create", 1, PFS_DIR_CREATE},
    {"dir.remove", 1, PFS_DIR_REMOVE},
    {"dir.replace", 1, PFS_DIR_REPLACE},
    {"admin.inspect", 2, PFS_ADMIN_INSPECT},
    {"admin.grants", 2, PFS_ADMIN_GRANTS},
    {"admin.owner", 2, PFS_ADMIN_OWNER},
  };
  *rights = (struct pfs_rights){0};
  if (strcmp(text, "none") == 0) {
    return PFS_OK;
  }
  while (*text) {
    const char *end = strchr(text, ',');
    size_t length = end ? (size_t)(end - text) : strlen(text);
    size_t i = 0;
    while (i < sizeof(tokens) / sizeof(tokens[0]) &&
           (strlen(tokens[i].name) != length || memcmp(text, tokens[i].name, length) != 0)) {
      ++i;
    }
    if (i == sizeof(tokens) / sizeof(tokens[0])) {
      return PFS_INVALID;
    }
    uint64_t *mask = tokens[i].domain == 0 ? &rights->file :
                     tokens[i].domain == 1 ? &rights->directory : &rights->admin;
    if ((*mask & tokens[i].bit) != 0) {
      return PFS_INVALID;
    }
    *mask |= tokens[i].bit;
    if (end == NULL) {
      return PFS_OK;
    }
    text = end + 1;
  }
  return PFS_INVALID;
}

enum option_bit {
  OPT_IMAGE = 1u << 0,
  OPT_MEMORY = 1u << 1,
  OPT_VOLUME = 1u << 2,
  OPT_VOLUME_ID = 1u << 3,
  OPT_PATH = 1u << 4,
  OPT_ROOT = 1u << 5,
  OPT_TARGET = 1u << 6,
  OPT_PRINCIPAL = 1u << 7,
  OPT_SCOPE = 1u << 8,
  OPT_RIGHTS = 1u << 9,
  OPT_CEILING = 1u << 10,
  OPT_PARTITION = 1u << 11,
  OPT_SECTOR = 1u << 12,
};

enum pfs_status
inspect_options_parse(int argc, char **argv, struct inspect_options *options)
{
  *options = (struct inspect_options){0};
  unsigned seen = 0;
  for (int i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    enum inspect_command command = INSPECT_NONE;
    if (strcmp(argument, "info") == 0) {
      command = INSPECT_INFO;
    } else if (strcmp(argument, "volumes") == 0) {
      command = INSPECT_VOLUMES;
    } else if (strcmp(argument, "list") == 0) {
      command = INSPECT_LIST;
    } else if (strcmp(argument, "stat") == 0) {
      command = INSPECT_STAT;
    } else if (strcmp(argument, "access") == 0) {
      command = INSPECT_ACCESS;
    }
    if (command != INSPECT_NONE) {
      if (options->command != INSPECT_NONE) {
        return PFS_INVALID;
      }
      options->command = command;
      continue;
    }
    unsigned bit;
    if (strcmp(argument, "--image") == 0) {
      bit = OPT_IMAGE;
    } else if (strcmp(argument, "--memory-limit") == 0) {
      bit = OPT_MEMORY;
    } else if (strcmp(argument, "--volume") == 0) {
      bit = OPT_VOLUME;
    } else if (strcmp(argument, "--volume-id") == 0) {
      bit = OPT_VOLUME_ID;
    } else if (strcmp(argument, "--path") == 0) {
      bit = OPT_PATH;
    } else if (strcmp(argument, "--root") == 0) {
      bit = OPT_ROOT;
    } else if (strcmp(argument, "--target") == 0) {
      bit = OPT_TARGET;
    } else if (strcmp(argument, "--principal") == 0) {
      bit = OPT_PRINCIPAL;
    } else if (strcmp(argument, "--scope") == 0) {
      bit = OPT_SCOPE;
    } else if (strcmp(argument, "--rights") == 0) {
      bit = OPT_RIGHTS;
    } else if (strcmp(argument, "--ceiling") == 0) {
      bit = OPT_CEILING;
    } else if (strcmp(argument, "--gpt-partition") == 0) {
      bit = OPT_PARTITION;
    } else if (strcmp(argument, "--sector-size") == 0) {
      bit = OPT_SECTOR;
    } else {
      return PFS_INVALID;
    }
    if ((seen & bit) != 0 || i + 1 == argc) {
      return PFS_INVALID;
    }
    seen |= bit;
    const char *value = argv[++i];
    if (*value == '\0') {
      return PFS_INVALID;
    }
    enum pfs_status status = PFS_OK;
    switch (bit) {
      case OPT_IMAGE: options->image = value; break;
      case OPT_MEMORY:
        status = host_size_parse(value, &options->memory_limit);
        if (status == PFS_OK && options->memory_limit == 0) {
          status = PFS_INVALID;
        }
        break;
      case OPT_VOLUME:
        status = pfs_name_validate((const uint8_t *)value, strlen(value));
        if (status != PFS_OK) {
          status = PFS_INVALID;
        }
        options->volume_name = value;
        break;
      case OPT_VOLUME_ID:
        status = pfs_volume_id_parse(value, strlen(value), &options->volume_id);
        options->volume_id_set = true;
        break;
      case OPT_PATH: options->path = value; break;
      case OPT_ROOT: options->root = value; break;
      case OPT_TARGET: options->target = value; break;
      case OPT_PRINCIPAL:
        status = pfs_principal_id_parse(value, strlen(value), &options->principal);
        break;
      case OPT_SCOPE:
        if (strcmp(value, "object") == 0) {
          options->scope = PFS_SCOPE_OBJECT;
        } else if (strcmp(value, "subtree") == 0) {
          options->scope = PFS_SCOPE_SUBTREE;
        } else {
          status = PFS_INVALID;
        }
        break;
      case OPT_RIGHTS: status = rights_parse(value, &options->rights); break;
      case OPT_CEILING: status = rights_parse(value, &options->ceiling); break;
      case OPT_PARTITION:
        status = decimal_u32(value, &options->partition.partition_entry);
        if (options->partition.partition_entry == 0) {
          status = PFS_INVALID;
        }
        break;
      case OPT_SECTOR:
        status = decimal_u32(value, &options->partition.sector_size);
        if (options->partition.sector_size != 512 && options->partition.sector_size != 4096) {
          status = PFS_INVALID;
        }
        break;
    }
    if (status != PFS_OK) {
      return status;
    }
  }
  unsigned common = OPT_IMAGE | OPT_MEMORY | OPT_PARTITION | OPT_SECTOR;
  if ((seen & OPT_IMAGE) == 0 || options->command == INSPECT_NONE ||
      ((seen & OPT_PARTITION) != 0) != ((seen & OPT_SECTOR) != 0)) {
    return PFS_INVALID;
  }
  options->gpt = (seen & OPT_PARTITION) != 0;
  if (options->command == INSPECT_INFO || options->command == INSPECT_VOLUMES) {
    return (seen & ~common) == 0 ? PFS_OK : PFS_INVALID;
  }
  if (((seen & OPT_VOLUME) != 0) == ((seen & OPT_VOLUME_ID) != 0)) {
    return PFS_INVALID;
  }
  common |= OPT_VOLUME | OPT_VOLUME_ID;
  unsigned required = options->command == INSPECT_ACCESS ?
    OPT_ROOT | OPT_TARGET | OPT_PRINCIPAL | OPT_SCOPE | OPT_RIGHTS | OPT_CEILING : OPT_PATH;
  return (seen & required) == required && (seen & ~(common | required)) == 0 ?
    PFS_OK : PFS_INVALID;
}
