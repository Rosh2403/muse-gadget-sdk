/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Questions from a phone on the home network: see muse_remote.h. */
#include "muse_remote.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_chat.h"
#include "muse_input.h"
#include "muse_settings.h"
#include "muse_wifi.h"

#define BODY_MAX 2048      /* a dictated question is far shorter */
#define REPLY_MAX 8192     /* every message of a reply (muse_chat_ask) */
#define REPLY_WAIT_MS (150 * 1000)   /* agents can work a while; the caller waits longer */
#define RECV_TRIES 3       /* receive timeouts in a row before giving up on a body */
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "muse_remote";

static bool blank(char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

/* Whether got is want, taking a time that depends only on want's length. */
static bool key_ok(const char *got, const char *want)
{
    size_t n = strlen(want), m = strlen(got);
    unsigned char diff = n != m;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)want[i] ^ (unsigned char)(i < m ? got[i] : 0);
    }
    return !diff;
}

/* The question in a body, JSON {"text": …, "reply": true?} or plain text,
 * trimmed and copied into PSRAM for the chat; NULL if there's none. *reply:
 * whether the caller wants the answer back instead of spoken here. */
static char *question(const char *body, bool *reply)
{
    *reply = false;
    while (blank(*body)) {
        body++;
    }
    cJSON *json = NULL;
    const char *text = body;
    if (*body == '{') {
        json = cJSON_Parse(body);
        text = cJSON_GetStringValue(cJSON_GetObjectItem(json, "text"));
        *reply = cJSON_IsTrue(cJSON_GetObjectItem(json, "reply"));
        if (!text) {
            cJSON_Delete(json);
            return NULL;
        }
        while (blank(*text)) {
            text++;
        }
    }
    size_t n = strlen(text);
    while (n && blank(text[n - 1])) {
        n--;
    }
    char *out = n ? heap_caps_malloc(n + 1, PSRAM_CAPS) : NULL;
    if (out) {
        memcpy(out, text, n);
        out[n] = '\0';
    }
    cJSON_Delete(json);
    return out;
}

static esp_err_t answer(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* The answer to a question asked with "reply": {"reply": "…"}, or {"error": "…"}. */
static esp_err_t answer_reply(httpd_req_t *req, muse_ask_result_t res, const char *text)
{
    const char *status = res == MUSE_ASK_OK ? "200 OK"
                       : res == MUSE_ASK_TIMEOUT ? "504 Gateway Timeout"
                       : res == MUSE_ASK_FAILED ? "502 Bad Gateway"
                       : "503 Service Unavailable";
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, res == MUSE_ASK_OK ? "reply" : "error", text);
    char *s = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t sent = s ? answer(req, status, s)
                       : answer(req, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    cJSON_free(s);
    return sent;
}

static esp_err_t on_ask(httpd_req_t *req)
{
    char want[MUSE_REMOTE_KEY_MAX + 1];
    muse_settings_remote_key(want);
    if (!want[0]) {
        return answer(req, "403 Forbidden", "{\"error\":\"no key set: send >remote.key=… over USB\"}");
    }
    char auth[sizeof("Bearer ") + MUSE_REMOTE_KEY_MAX];
    bool ok = httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) == ESP_OK &&
              !strncmp(auth, "Bearer ", 7) && key_ok(auth + 7, want);
    memset(want, 0, sizeof(want));
    memset(auth, 0, sizeof(auth));
    if (!ok) {
        ESP_LOGW(TAG, "refused a question: wrong or missing key");
        return answer(req, "401 Unauthorized", "{\"error\":\"wrong key\"}");
    }
    if (!req->content_len || req->content_len > BODY_MAX) {
        return answer(req, req->content_len ? "413 Payload Too Large" : "400 Bad Request",
                      "{\"error\":\"send the question, up to 2048 bytes\"}");
    }

    char *body = heap_caps_malloc(req->content_len + 1, PSRAM_CAPS);
    if (!body) {
        return answer(req, "503 Service Unavailable", "{\"error\":\"out of memory\"}");
    }
    size_t got = 0;
    for (int timeouts = 0; got < req->content_len;) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < RECV_TRIES) {
            continue;
        }
        if (n <= 0) {
            free(body);
            return ESP_FAIL;   /* closes the connection */
        }
        got += (size_t)n;
        timeouts = 0;
    }
    body[got] = '\0';
    bool reply;
    char *text = question(body, &reply);
    free(body);
    if (!text) {
        return answer(req, "400 Bad Request", "{\"error\":\"no question\"}");
    }

    ESP_LOGI(TAG, "question over Wi-Fi (%u chars)%s", (unsigned)strlen(text), reply ? ", answer wanted back" : "");
    if (reply) {
        /* For a car, say: nothing is spoken or shown here. */
        char *out = heap_caps_malloc(REPLY_MAX, PSRAM_CAPS);
        if (!out) {
            free(text);
            return answer(req, "503 Service Unavailable", "{\"error\":\"out of memory\"}");
        }
        muse_ask_result_t res = muse_chat_ask(text, out, REPLY_MAX, REPLY_WAIT_MS);   /* takes text */
        static const char *const names[] = { "answered", "failed", "timed out", "unavailable" };
        ESP_LOGI(TAG, "asked over Wi-Fi: %s (%u chars)", names[res], (unsigned)strlen(out));
        esp_err_t sent = answer_reply(req, res, out);
        free(out);
        return sent;
    }
    muse_input_wake("asked over Wi-Fi");
    if (!muse_hatch_text_turn(text)) {   /* takes text */
        return answer(req, "503 Service Unavailable", "{\"error\":\"Muse isn't connected yet\"}");
    }
    return answer(req, "202 Accepted", "{\"ok\":true}");
}

/* The HTTP server binds once the network stack is up, and Link brings that up
 * after the Muse UI starts: wait for Wi-Fi, start it, and go. */
static void start_task(void *arg)
{
    (void)arg;
    while (!muse_wifi_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_caps = PSRAM_CAPS;    /* internal RAM is what this board runs short of */
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 2;      /* one phone at a time, and a spare */
    cfg.lru_purge_enable = true;   /* a new request closes a kept-alive one */
    cfg.max_uri_handlers = 1;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err == ESP_OK) {
        static const httpd_uri_t ask = { .uri = "/ask", .method = HTTP_POST, .handler = on_ask };
        err = httpd_register_uri_handler(server, &ask);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "listening for questions on port %u (POST /ask)", (unsigned)cfg.server_port);
    } else {
        ESP_LOGE(TAG, "can't listen for questions (%s)", esp_err_to_name(err));
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t muse_remote_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(start_task, "muse_remote", 3072, NULL, 3, NULL,
                                                    tskNO_AFFINITY, PSRAM_CAPS);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
