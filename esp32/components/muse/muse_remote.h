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

#pragma once

#include "esp_err.h"

/*
 * Asking Muse from a phone on the home network (CONFIG_MUSE_REMOTE_ASK):
 *
 *   POST http://<board>/ask
 *   Authorization: Bearer <the key set over USB with >remote.key=…>
 *   {"text": "What's the weather tomorrow?"}     (or the question as plain text)
 *
 * The question goes to Muse as a typed turn, the screen wakes and the reply is
 * spoken here. Answers 202 once it's on its way, 401 for a wrong key, 403 with
 * no key set. An iPhone Shortcut ("Hey Siri, Ask Muse") asks for the question,
 * then sends it with Get Contents of URL.
 *
 * With "reply": true in the JSON, the answer comes back instead, and nothing is
 * spoken or shown here: 200 {"reply": "…"} once Muse has answered (every
 * message, a blank line between), or 502/503/504 {"error": "…"}. Waits up to
 * REPLY_WAIT_MS, one caller at a time. The Muse CarPlay app asks this way.
 */

/* Starts listening once Wi-Fi first connects. */
esp_err_t muse_remote_start(void);
