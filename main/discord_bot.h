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

/**
 * Discord Gateway Intents (v10)
 * Reference: https://discord.com/developers/docs/topics/gateway#gateway-intents
 */
#define DISCORD_INTENT_GUILDS (1 << 0)
#define DISCORD_INTENT_GUILD_MEMBERS (1 << 1) // Privileged
#define DISCORD_INTENT_GUILD_MODERATION (1 << 2)
#define DISCORD_INTENT_GUILD_EXPRESSIONS (1 << 3)
#define DISCORD_INTENT_GUILD_INTEGRATIONS (1 << 4)
#define DISCORD_INTENT_GUILD_WEBHOOKS (1 << 5)
#define DISCORD_INTENT_GUILD_INVITES (1 << 6)
#define DISCORD_INTENT_GUILD_VOICE_STATES (1 << 7)
#define DISCORD_INTENT_GUILD_PRESENCES (1 << 8) // Privileged
#define DISCORD_INTENT_GUILD_MESSAGES (1 << 9)
#define DISCORD_INTENT_GUILD_MESSAGE_REACTIONS (1 << 10)
#define DISCORD_INTENT_GUILD_MESSAGE_TYPING (1 << 11)
#define DISCORD_INTENT_DIRECT_MESSAGES (1 << 12)
#define DISCORD_INTENT_DIRECT_MESSAGE_REACTIONS (1 << 13)
#define DISCORD_INTENT_DIRECT_MESSAGE_TYPING (1 << 14)
#define DISCORD_INTENT_MESSAGE_CONTENT (1 << 15) // Privileged
#define DISCORD_INTENT_GUILD_SCHEDULED_EVENTS (1 << 16)
#define DISCORD_INTENT_AUTO_MODERATION_CONFIGURATION (1 << 20)
#define DISCORD_INTENT_AUTO_MODERATION_EXECUTION (1 << 21)
#define DISCORD_INTENT_GUILD_MESSAGE_POLLS (1 << 24)
#define DISCORD_INTENT_DIRECT_MESSAGE_POLLS (1 << 25)

typedef struct {
  const char *token;
  uint32_t intents;
} discord_bot_config_t;

void discord_bot_task(void *pvParameters);
