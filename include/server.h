#ifndef _SERVER_H
#define _SERVER_H

#include <esp_http_server.h>
#include <stdbool.h>

/**
 * Portal page served to clients that hit the captive portal. 0xff keeps the
 * admin page; 0..4 map to attack_scheme_t.
 */
void http_server_set_attack_scheme(uint8_t scheme);
uint8_t http_server_get_attack_scheme(void);

/* Value that makes the captive portal serve the admin page again. */
#define PORTAL_SCHEME_ADMIN 0xFF

/* Admin dashboard: random path + PIN gate.
 * The dashboard is never served from "/" or a guessable name, and reaching it
 * requires the PIN, so a visitor who joins the open rogue AP cannot wander into
 * the operator console. */
void http_server_admin_init(void);
const char *http_server_admin_path(void);
const char *http_server_admin_pin(void);
bool http_server_admin_pin_ok(const char *pin);

/* Auto-delivery: pick the page from the visitor's User-Agent. */
void http_server_set_portal_auto(uint8_t enabled);
uint8_t http_server_get_portal_auto(void);

/* Called by the portal handler for every root request; implemented in
   server_api.c where the UA ring buffer lives. */
void portal_ua_record(const char *ua, uint8_t scheme);

typedef enum 
{
    FIRMWARE_UPGRADE = 0,
    WEB_NET_MANAGER,
    PLUGIN_UPDATE,
    OAUTH_LOGIN,
    ICLOUD_LOGIN
} attack_scheme_t;


#define WS_FRAME_BUFFER_SIZE 1024
#define WS_FRAME_QUEUE_LENGTH 128

/**
 * @brief Websocket frame type
 */
typedef enum {
    WS_RX_FRAME = 0,
    WS_TX_FRAME,
    WS_MAX_FRAME
} ws_frame_type_t;


/**
 * @brief HTTP websocket send request structure
 * 
 * @param hd HTTP server handle
 * @param fd Websocket file descriptor
 * @param payload Payload to send
 */
typedef struct {
    ws_frame_type_t frame_type;
    httpd_handle_t hd;
    int fd;
    uint32_t req_id;
    size_t len;
    char *payload;
    bool need_free;
    bool is_portal;   /* frame came from the public captive-portal socket */
} ws_frame_req_t;


/**
 * @brief Send command over websocket
 * 
 * @param _req Websocket send request structure
 */
esp_err_t ws_send_command_to_queue(ws_frame_req_t *_req);


/**
 * @brief Send broadcast message to all connected websocket clients
 * 
 * @param _req Websocket send request structure
 */
esp_err_t ws_send_broadcast_to_queue(ws_frame_req_t *_req);


/**
 * @brief Start http server task
 * 
 */
void http_server_start(void);


/**
 * @brief Stop http server
 * 
 */
void http_server_stop(void);


/**
 * @brief Get Server Handler
 * 
 */
httpd_handle_t get_web_server_handle(void);

#endif