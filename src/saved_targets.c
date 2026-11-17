#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "saved_targets.h"

static const char *TAG = "SAVED_TGT";

static saved_target_t s_targets[SAVED_TARGET_MAX];
static int s_count = 0;

#define NVS_NS      "saved_tgt"
#define KEY_COUNT   "count"

/* Build the per-slot NVS key ("t0".."t15"). */
static void slot_key(int i, char *out, size_t n)
{
    snprintf(out, n, "t%d", i);
}

void saved_targets_init(void)
{
    nvs_handle_t h;
    s_count = 0;
    memset(s_targets, 0, sizeof(s_targets));

    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved targets yet");
        return;
    }

    int32_t cnt = 0;
    if (nvs_get_i32(h, KEY_COUNT, &cnt) != ESP_OK || cnt < 0) {
        cnt = 0;
    }
    if (cnt > SAVED_TARGET_MAX) cnt = SAVED_TARGET_MAX;

    for (int i = 0; i < cnt; i++) {
        char key[8];
        slot_key(i, key, sizeof(key));
        size_t sz = sizeof(saved_target_t);
        if (nvs_get_blob(h, key, &s_targets[i], &sz) == ESP_OK && sz == sizeof(saved_target_t)) {
            s_count = i + 1;
        } else {
            break; /* stop at first gap */
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded %d saved target(s)", s_count);
}

int saved_targets_count(void) { return s_count; }

const saved_target_t *saved_targets_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_targets[index];
}

const saved_target_t *saved_targets_find(const char *name)
{
    if (name == NULL) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strncmp(s_targets[i].name, name, SAVED_TARGET_NAME_LEN) == 0) {
            return &s_targets[i];
        }
    }
    return NULL;
}

esp_err_t saved_targets_flush(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed (%s)", esp_err_to_name(err));
        return err;
    }
    /* Write all current slots, then the count. Stale higher slots are erased
       so a reload cannot resurrect deleted entries. */
    for (int i = 0; i < SAVED_TARGET_MAX; i++) {
        char key[8];
        slot_key(i, key, sizeof(key));
        if (i < s_count) {
            nvs_set_blob(h, key, &s_targets[i], sizeof(saved_target_t));
        } else {
            nvs_erase_key(h, key); /* ok if missing */
        }
    }
    nvs_set_i32(h, KEY_COUNT, s_count);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

int saved_targets_save(const char *name, const target_info_t *target)
{
    if (target == NULL) return -1;

    char label[SAVED_TARGET_NAME_LEN] = {0};
    if (name != NULL && name[0] != '\0') {
        strlcpy(label, name, sizeof(label));
    } else {
        /* default label: SSID (bounded, may be empty for hidden) */
        strlcpy(label, (const char *)target->ssid, sizeof(label));
        if (label[0] == '\0') strlcpy(label, "(hidden)", sizeof(label));
    }

    /* Replace an existing entry with the same label, else append. */
    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (strncmp(s_targets[i].name, label, SAVED_TARGET_NAME_LEN) == 0) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        if (s_count >= SAVED_TARGET_MAX) {
            ESP_LOGW(TAG, "list full (%d), cannot save '%s'", SAVED_TARGET_MAX, label);
            return -1;
        }
        slot = s_count++;
    }

    memset(&s_targets[slot], 0, sizeof(saved_target_t));
    strlcpy(s_targets[slot].name, label, sizeof(s_targets[slot].name));
    memcpy(&s_targets[slot].target, target, sizeof(target_info_t));

    if (saved_targets_flush() != ESP_OK) {
        return -1;
    }
    ESP_LOGI(TAG, "saved target '%s' at slot %d", label, slot);
    return slot;
}

bool saved_targets_delete(int index)
{
    if (index < 0 || index >= s_count) return false;
    /* shift down */
    for (int i = index; i < s_count - 1; i++) {
        s_targets[i] = s_targets[i + 1];
    }
    s_count--;
    memset(&s_targets[s_count], 0, sizeof(saved_target_t));
    saved_targets_flush();
    ESP_LOGI(TAG, "deleted saved target at %d, %d left", index, s_count);
    return true;
}
