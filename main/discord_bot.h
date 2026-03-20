/* Mishabot: ESP32 Discord Image Bot
 * Copyright (C) 2026 Dory
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <stdint.h>

#include "esp_check.h"

/**
 * Discord Gateway Intents (v10)
 * Reference: https://discord.com/developers/docs/topics/gateway#gateway-intents
 */
#define DISCORD_INTENT_GUILDS (1 << 0)
#define DISCORD_INTENT_GUILD_MEMBERS (1 << 1)  // Privileged
#define DISCORD_INTENT_GUILD_MODERATION (1 << 2)
#define DISCORD_INTENT_GUILD_EXPRESSIONS (1 << 3)
#define DISCORD_INTENT_GUILD_INTEGRATIONS (1 << 4)
#define DISCORD_INTENT_GUILD_WEBHOOKS (1 << 5)
#define DISCORD_INTENT_GUILD_INVITES (1 << 6)
#define DISCORD_INTENT_GUILD_VOICE_STATES (1 << 7)
#define DISCORD_INTENT_GUILD_PRESENCES (1 << 8)  // Privileged
#define DISCORD_INTENT_GUILD_MESSAGES (1 << 9)
#define DISCORD_INTENT_GUILD_MESSAGE_REACTIONS (1 << 10)
#define DISCORD_INTENT_GUILD_MESSAGE_TYPING (1 << 11)
#define DISCORD_INTENT_DIRECT_MESSAGES (1 << 12)
#define DISCORD_INTENT_DIRECT_MESSAGE_REACTIONS (1 << 13)
#define DISCORD_INTENT_DIRECT_MESSAGE_TYPING (1 << 14)
#define DISCORD_INTENT_MESSAGE_CONTENT (1 << 15)  // Privileged
#define DISCORD_INTENT_GUILD_SCHEDULED_EVENTS (1 << 16)
#define DISCORD_INTENT_AUTO_MODERATION_CONFIGURATION (1 << 20)
#define DISCORD_INTENT_AUTO_MODERATION_EXECUTION (1 << 21)
#define DISCORD_INTENT_GUILD_MESSAGE_POLLS (1 << 24)
#define DISCORD_INTENT_DIRECT_MESSAGE_POLLS (1 << 25)

// callbacks
typedef void (*discord_on_ready_cb)(const char* app_id);
typedef void (*discord_on_message_cb)(
    const char* username, const char* content, const char* channel
);
typedef void (*discord_on_interaction_cmd_cb)(
    const char* id, const char* token, const char* cmd, const char* user_id
);
typedef void (*discord_on_interaction_action_cb)(
    const char* global_app_id, const char* id, const char* token,
    const char* custom_action_id, const char* user_id
);

// main bot config struct
typedef struct {
  const char* token;
  uint32_t intents;
  discord_on_ready_cb on_ready;
  discord_on_message_cb on_message;
  discord_on_interaction_cmd_cb on_interaction_cmd;
  discord_on_interaction_action_cb on_interaction_action;
} discord_bot_config_t;

void discord_bot_init(const discord_bot_config_t* config);

// utility functions
esp_err_t discord_send_typing(const char* channel_id);
esp_err_t discord_send_message(const char* channel_id, const char* content);
esp_err_t discord_send_image_embed(const char* channel_id, const char* img_url);
esp_err_t discord_api_post(const char* path, const char* data);
esp_err_t discord_api_patch(const char* path, const char* data);

void* discord_start_typing(const char* channel_id);
void discord_stop_typing(void* handle);
