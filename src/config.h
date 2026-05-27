#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "log.h"

int config_load(const char *path);
char *config_masked_json(void);
int config_replace_from_json(const char *body, char **err);
cJSON *config_select_model_copy(const char *requested_model);
char *config_get_string_copy(const char *key);
int config_set_active_model(const char *id, char **err);

#endif