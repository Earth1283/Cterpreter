#ifndef CT_CONFIG_H
#define CT_CONFIG_H

#include <stdio.h>

enum { CFG_TIPS, CFG_HIGHLIGHTING, CFG_SUGGESTIONS, CFG_SIGNATURES, CFG_DIAGNOSTICS, CFG_COLOR, CFG_COUNT };
typedef struct { int values[CFG_COUNT]; } CliConfig;

void config_defaults(CliConfig *config);
int config_index(const char *name);
const char *config_name(int index);
const char *config_value(const CliConfig *config, int index);
const char *config_description(int index);
int config_set(CliConfig *config, const char *name, const char *value);
/* Loading is transactional. A missing file is ignored only when optional. */
int config_load(CliConfig *config, const char *path, int optional);
int config_save(const CliConfig *config, const char *path);

#endif
