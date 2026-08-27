/* config.c -- OpenBOR Switch wrapper configuration.
 * MIT license; see LICENSE. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

/* Actual storage for the globals declared extern in config.h */
Config config;
int screen_width = 0;
int screen_height = 0;

static void config_set_defaults(void) {
    config.screen_width = -1;
    config.screen_height = -1;
    snprintf(config.data_root, sizeof(config.data_root), "%s", DEFAULT_DATA_ROOT);
    snprintf(config.save_root, sizeof(config.save_root), "%s", DEFAULT_SAVE_ROOT);
}

int read_config(const char *file) {
    FILE *fp = fopen(file, "r");
    if (!fp) {
        config_set_defaults();
        return -1;
    }

    config_set_defaults();

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = strchr(line, '\n');
        if (p) *p = '\0';
        p = strchr(line, '\r');
        if (p) *p = '\0';

        if (line[0] == '#' || line[0] == ';' || line[0] == '\0')
            continue;

        char key[128];
        char value[384];
        if (sscanf(line, "%127[^=]=%383s", key, value) != 2)
            continue;

        if (strcmp(key, "screen_width") == 0) {
            config.screen_width = atoi(value);
        } else if (strcmp(key, "screen_height") == 0) {
            config.screen_height = atoi(value);
        } else if (strcmp(key, "data_root") == 0) {
            snprintf(config.data_root, sizeof(config.data_root), "%s", value);
        } else if (strcmp(key, "save_root") == 0) {
            snprintf(config.save_root, sizeof(config.save_root), "%s", value);
        }
    }

    fclose(fp);
    return 0;
}

int write_config(const char *file) {
    FILE *fp = fopen(file, "w");
    if (!fp)
        return -1;

    fprintf(fp, "screen_width=%d\n", config.screen_width);
    fprintf(fp, "screen_height=%d\n", config.screen_height);
    fprintf(fp, "data_root=%s\n", config.data_root);
    fprintf(fp, "save_root=%s\n", config.save_root);

    fclose(fp);
    return 0;
}
