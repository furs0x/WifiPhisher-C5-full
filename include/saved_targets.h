#ifndef _SAVED_TARGETS_H
#define _SAVED_TARGETS_H

#include <stdbool.h>
#include <stdint.h>
#include "target.h"

#define SAVED_TARGET_MAX       16
#define SAVED_TARGET_NAME_LEN  24

typedef struct {
    char   name[SAVED_TARGET_NAME_LEN];  /* user-visible label */
    target_info_t target;                /* ssid/bssid/chan/auth/ciphers/vendor */
} saved_target_t;

/** Load the saved target list from NVS into RAM (call once at boot). */
void saved_targets_init(void);

/** Number of currently saved targets. */
int  saved_targets_count(void);

/** Get a saved target by slot index [0..count-1]; NULL if out of range. */
const saved_target_t *saved_targets_get(int index);

/** Find a saved target by its name (exact). NULL if none. */
const saved_target_t *saved_targets_find(const char *name);

/**
 * Save (or replace, matching by name) a target.
 * @param name   label (<= SAVED_TARGET_NAME_LEN-1); if NULL/empty uses SSID
 * @param target target to clone
 * @return slot index on success (>=0), or -1 on error / full
 */
int  saved_targets_save(const char *name, const target_info_t *target);

/** Delete by slot index. Returns true on success. */
bool saved_targets_delete(int index);

/** Persist current RAM list to NVS. */
esp_err_t saved_targets_flush(void);

#endif
