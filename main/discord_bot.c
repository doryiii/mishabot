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
#include "discord_bot.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "discord_bot";

#ifdef CONFIG_LED_INVERTED
#define LED_ON 0
#define LED_OFF 1
#else
#define LED_ON 1
#define LED_OFF 0
#endif

static esp_websocket_client_handle_t ws_client = NULL;
static int last_seq_num = -1;
static int heartbeat_interval_ms = 0;
static TaskHandle_t heartbeat_task_handle = NULL;
static discord_bot_config_t* bot_config = NULL;
static char global_app_id[32] = {0};

#define DANBOORU_API_BASE                                                     \
  "https://danbooru.donmai.us/posts/random.json?login=" CONFIG_DANBOORU_LOGIN \
  "&api_key=" CONFIG_DANBOORU_API_KEY "&tags="

static void discord_send_identify(esp_websocket_client_handle_t client) {
  if (!bot_config) return;

  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "op", 2);

  cJSON* d = cJSON_CreateObject();
  cJSON_AddStringToObject(d, "token", bot_config->token);
  cJSON_AddNumberToObject(d, "intents", bot_config->intents);

  cJSON* props = cJSON_CreateObject();
  cJSON_AddStringToObject(props, "os", "linux");
  cJSON_AddStringToObject(props, "browser", "esp32");
  cJSON_AddStringToObject(props, "device", "esp32");

  cJSON_AddItemToObject(d, "properties", props);
  cJSON_AddItemToObject(root, "d", d);

  char* payload = cJSON_PrintUnformatted(root);
  if (payload) {
    ESP_LOGI(TAG, "Sending Identify");
    esp_websocket_client_send_text(
        client, payload, strlen(payload), portMAX_DELAY
    );
    free(payload);
  }
  cJSON_Delete(root);
}


static esp_err_t discord_api_request(
    esp_http_client_method_t method, const char* endpoint, const char* req_data
) {
  if (!bot_config) {
    return ESP_ERR_INVALID_STATE;
  }

  char url[512];
  snprintf(url, sizeof(url), "https://discord.com/api/v10%s", endpoint);

  esp_http_client_config_t config = {
      .url = url,
      .method = method,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  char auth_header[256];
  snprintf(auth_header, sizeof(auth_header), "Bot %s", bot_config->token);
  esp_http_client_set_header(client, "Authorization", auth_header);
  esp_http_client_set_header(client, "Content-Type", "application/json");

  if (req_data) {
    esp_http_client_set_post_field(client, req_data, strlen(req_data));
  }

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    if (status_code >= 200 && status_code < 300) {
      ESP_LOGI(TAG, "Discord API %s success (%d)", endpoint, status_code);
    } else {
      ESP_LOGE(TAG, "Discord API %s failed (%d)", endpoint, status_code);
      err = ESP_FAIL;
    }
  } else {
    ESP_LOGE(TAG, "Discord API %s error: %s", endpoint, esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  return err;
}


static esp_err_t discord_api_post(
    const char* channel_id, const char* endpoint, const char* post_data
) {
  char path[128];
  snprintf(path, sizeof(path), "/channels/%s/%s", channel_id, endpoint);
  return discord_api_request(HTTP_METHOD_POST, path, post_data);
}


static void send_discord_typing(const char* channel_id) {
  discord_api_post(channel_id, "typing", NULL);
}


static void send_discord_image_embed(
    const char* channel_id, const char* image_url
) {
  cJSON* root = cJSON_CreateObject();
  cJSON* embeds = cJSON_CreateArray();
  cJSON* embed = cJSON_CreateObject();
  cJSON* image = cJSON_CreateObject();

  cJSON_AddStringToObject(image, "url", image_url);
  cJSON_AddItemToObject(embed, "image", image);
  cJSON_AddItemToArray(embeds, embed);
  cJSON_AddItemToObject(root, "embeds", embeds);

  char* post_data = cJSON_PrintUnformatted(root);

  discord_api_post(channel_id, "messages", post_data);

  free(post_data);
  cJSON_Delete(root);
}


static char* fetch_danbooru_url(const char* tags, const char* filter) {
  char* image_url = NULL;
  // TODO: handle http chunks to avoid allocating max buf every time
  const int buffer_size = 12288;
  char* buffer = malloc(buffer_size);
  if (!buffer) return NULL;

  char url[256];
  snprintf(url, sizeof(url), "%s%s+%s", DANBOORU_API_BASE, tags, filter);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  esp_err_t err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(client);
    int len = esp_http_client_read(client, buffer, buffer_size - 1);
    if (len > 0) {
      buffer[len] = '\0';
      ESP_LOGI(TAG, "Danbooru response received: %d bytes", len);
      cJSON* root = cJSON_Parse(buffer);
      if (root) {
        cJSON* file_url = cJSON_GetObjectItem(root, "file_url");
        if (cJSON_IsString(file_url)) {
          image_url = strdup(file_url->valuestring);
        }
        cJSON_Delete(root);
      }
    }
  } else {
    ESP_LOGE(TAG, "Failed to fetch from Danbooru: %s", esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  free(buffer);
  ESP_LOGI(TAG, "image: %s", image_url);
  return image_url;
}


static char* fetch_danbooru_safe_url(const char* tags) {
  return fetch_danbooru_url(tags, "rating%3Ageneral");
}


static char* fetch_danbooru_risky_url(const char* tags) {
  return fetch_danbooru_url(tags, "rating%3Asafe");
}


static void handle_character_command(const char* channel_id, const char* tags) {
  gpio_set_level(CONFIG_LED_GPIO, LED_ON);
  send_discord_typing(channel_id);

  ESP_LOGI(TAG, "Getting %s images", tags);
  char* image_url = fetch_danbooru_safe_url(tags);
  if (image_url) {
    send_discord_image_embed(channel_id, image_url);
    free(image_url);
  } else {
    ESP_LOGW(TAG, "Could not get image URL from Danbooru for %s", tags);
  }

  gpio_set_level(CONFIG_LED_GPIO, LED_OFF);
}


static void on_message(cJSON* d) {
  cJSON* author = cJSON_GetObjectItem(d, "author");
  if (!author || !cJSON_IsObject(author)) return;

  cJSON* is_bot = cJSON_GetObjectItem(author, "bot");
  if (cJSON_IsTrue(is_bot)) return;

  cJSON* content = cJSON_GetObjectItem(d, "content");
  if (!cJSON_IsString(content)) return;

  cJSON* channel_id = cJSON_GetObjectItem(d, "channel_id");
  if (!cJSON_IsString(channel_id)) return;
  const char* channel = channel_id->valuestring;

  if (strcmp(content->valuestring, ".misha") == 0) {
    ESP_LOGI(TAG, ".misha");
    handle_character_command(channel, "misha_%28honkai%3A_star_rail%29");

  } else if (strcmp(content->valuestring, ".furina") == 0) {
    handle_character_command(channel, "furina_%28genshin_impact%29");

  } else if (strcmp(content->valuestring, ".karen") == 0) {
    handle_character_command(channel, "kujou_karen");

  } else if (strcmp(content->valuestring, ".kokomi") == 0) {
    handle_character_command(channel, "sangonomiya_kokomi");

  } else if (strcmp(content->valuestring, ".reisen") == 0) {
    handle_character_command(channel, "reisen_udongein_inaba");

  } else {
    cJSON* username = cJSON_GetObjectItem(author, "username");
    if (cJSON_IsString(username)) {
      ESP_LOGV(TAG, "[%s]: %s", username->valuestring, content->valuestring);
    }
  }
}


static void register_slash_commands(const char* app_id) {
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", "fish");
  cJSON_AddStringToObject(root, "description", "Start a fishing minigame");
  cJSON_AddNumberToObject(root, "type", 1);  // CHAT_INPUT

  char* payload = cJSON_PrintUnformatted(root);
  if (payload) {
    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "/applications/%s/commands", app_id);
    discord_api_request(HTTP_METHOD_POST, endpoint, payload);
    free(payload);
  }
  cJSON_Delete(root);
}


static void handle_interaction_create(cJSON* d) {
  ESP_LOGI(TAG, "Handling INTERACTION_CREATE");
  cJSON* type = cJSON_GetObjectItem(d, "type");
  if (!cJSON_IsNumber(type)) {
    ESP_LOGE(TAG, "Interaction missing type or not a number");
    return;
  }

  cJSON* id = cJSON_GetObjectItem(d, "id");
  cJSON* token = cJSON_GetObjectItem(d, "token");
  if (!cJSON_IsString(id) || !cJSON_IsString(token)) {
    ESP_LOGE(TAG, "Interaction missing id or token");
    return;
  }

  int int_type = type->valueint;
  ESP_LOGI(TAG, "Interaction type: %d", int_type);

  if (int_type == 2) {  // APPLICATION_COMMAND
    cJSON* data = cJSON_GetObjectItem(d, "data");
    if (data) {
      cJSON* name = cJSON_GetObjectItem(data, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, "fish") == 0) {
        ESP_LOGI(TAG, "Received /fish command");
        const char* fish_events[20] = {
            "The water ripples in the shape of a heart.",
            "A tiny bubble floats up and pops with a 'meow'.",
            "The bobber spins like a top.",
            "You smell... strawberries?",
            "The line feels strangely heavy, then light.",
            "A spectral hand briefly grasps your line.",
            "The water around the bobber turns neon green.",
            "You hear a faint whispering from the depths.",
            "The bobber suddenly sinks, then shoots into the sky!",
            "A small vortex forms around the line.",
            "The fish is trying to write something in the water.",
            "A golden light shines from beneath the surface.",
            "The bobber multiplies, then merges back into one.",
            "You feel a tug, but the line goes sideways.",
            "The water briefly parts, revealing a tiny treasure chest.",
            "A melodious chime echoes across the water.",
            "The line vibrates at a peculiar frequency.",
            "You see a reflection of the moon, even if it's day.",
            "The bobber turns into a tiny rubber duck.",
            "The fish seems to be blowing bubbles."
        };
        int event_id = esp_random() % 20;
        char response_payload[1024];
        snprintf(
            response_payload, sizeof(response_payload),
            "{\"type\":4,\"data\":{\"content\":\"You cast your line... %s How "
            "do you reel it "
            "in?\",\"components\":[{\"type\":1,\"components\":[{\"type\":2,"
            "\"style\":1,\"custom_id\":\"fish_gentle_%d\",\"label\":\"Reels "
            "gently\"},{\"type\":2,\"style\":1,\"custom_id\":\"fish_fast_%d\","
            "\"label\":\"Reels "
            "faster\"},{\"type\":2,\"style\":1,\"custom_id\":\"fish_erratic_%"
            "d\","
            "\"label\":\"Reels erratically\"},{\"type\":2,\"style\":1,"
            "\"custom_id\":\"fish_suggestive_%d\",\"label\":\"Reels "
            "suggestively\"}]}]}}",
            fish_events[event_id], event_id, event_id, event_id, event_id
        );
        char endpoint[512];
        snprintf(
            endpoint, sizeof(endpoint), "/interactions/%s/%s/callback",
            id->valuestring, token->valuestring
        );
        discord_api_request(HTTP_METHOD_POST, endpoint, response_payload);
      }
    }
  } else if (int_type == 3) {  // MESSAGE_COMPONENT
    ESP_LOGI(TAG, "Received MESSAGE_COMPONENT interaction");
    cJSON* data = cJSON_GetObjectItem(d, "data");
    if (data) {
      cJSON* custom_id = cJSON_GetObjectItem(data, "custom_id");
      if (cJSON_IsString(custom_id)) {
        ESP_LOGI(TAG, "Component custom_id: %s", custom_id->valuestring);
        if (strncmp(custom_id->valuestring, "fish_", 5) == 0) {
          ESP_LOGI(TAG, "Processing fish component interaction");
          const char* defer_payload = "{\"type\":6}";
          char endpoint[512];
          snprintf(
              endpoint, sizeof(endpoint), "/interactions/%s/%s/callback",
              id->valuestring, token->valuestring
          );
          discord_api_request(HTTP_METHOD_POST, endpoint, defer_payload);

          bool is_suggestive =
              (strncmp(custom_id->valuestring, "fish_suggestive", 15) == 0);
          bool is_gentle =
              (strncmp(custom_id->valuestring, "fish_gentle", 11) == 0);
          bool is_fast = (strncmp(custom_id->valuestring, "fish_fast", 9) == 0);
          bool is_erratic =
              (strncmp(custom_id->valuestring, "fish_erratic", 12) == 0);

          int event_id = 0;
          const char* last_underscore = strrchr(custom_id->valuestring, '_');
          if (last_underscore) {
            event_id = atoi(last_underscore + 1);
          }

          int favored_btn = event_id % 4;
          bool is_favored = false;
          if (is_gentle && favored_btn == 0) is_favored = true;
          if (is_fast && favored_btn == 1) is_favored = true;
          if (is_erratic && favored_btn == 2) is_favored = true;
          if (is_suggestive && favored_btn == 3) is_favored = true;

          int win_chance = is_suggestive ? 33 : 50;
          if (is_favored) {
            win_chance += 15;
          }
          int rand_val = esp_random() % 100;
          bool won = (rand_val < win_chance);

          cJSON* patch_data = cJSON_CreateObject();
          cJSON_AddArrayToObject(patch_data, "components");

          if (won) {
            const char* fish_pool[] = {
                "sangonomiya_kokomi+-comic",
                "mualani_%28genshin_impact%29+-comic"
            };
            const char* fish_names[] = {"Kokomi", "Mualani"};
            int fish_idx = esp_random() % 2;

            char content_buf[64];
            snprintf(
                content_buf, sizeof(content_buf), "You caught a %s!",
                fish_names[fish_idx]
            );
            cJSON_AddStringToObject(patch_data, "content", content_buf);

            char* image_url =
                is_suggestive ? fetch_danbooru_risky_url(fish_pool[fish_idx])
                              : fetch_danbooru_safe_url(fish_pool[fish_idx]);
            if (image_url) {
              cJSON* embeds = cJSON_CreateArray();
              cJSON* embed = cJSON_CreateObject();
              cJSON* image = cJSON_CreateObject();
              cJSON_AddStringToObject(image, "url", image_url);
              cJSON_AddItemToObject(embed, "image", image);
              cJSON_AddItemToArray(embeds, embed);
              cJSON_AddItemToObject(patch_data, "embeds", embeds);
              free(image_url);
            }
          } else {
            cJSON_AddStringToObject(
                patch_data, "content", "The fish got away..."
            );
          }

          char* patch_str = cJSON_PrintUnformatted(patch_data);
          if (global_app_id[0] != '\0' && patch_str) {
            char webhook_endpoint[512];
            snprintf(
                webhook_endpoint, sizeof(webhook_endpoint),
                "/webhooks/%s/%s/messages/@original", global_app_id,
                token->valuestring
            );
            discord_api_request(HTTP_METHOD_PATCH, webhook_endpoint, patch_str);
          } else if (global_app_id[0] == '\0') {
            ESP_LOGE(TAG, "global_app_id is empty!");
          }
          if (patch_str) free(patch_str);
          cJSON_Delete(patch_data);
        }
      } else {
        ESP_LOGE(TAG, "custom_id missing or not a string");
      }
    } else {
      ESP_LOGE(TAG, "Interaction missing data object");
    }
  }
}


static void heartbeat_task(void* pvParameters) {
  while (1) {
    if (heartbeat_interval_ms <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(heartbeat_interval_ms));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", 1);
    if (last_seq_num >= 0) {
      cJSON_AddNumberToObject(root, "d", last_seq_num);
    } else {
      cJSON_AddNullToObject(root, "d");
    }

    char* payload = cJSON_PrintUnformatted(root);
    if (payload) {
      ESP_LOGD(TAG, "Sending Heartbeat");
      esp_websocket_client_send_text(
          ws_client, payload, strlen(payload), portMAX_DELAY
      );
      free(payload);
    }
    cJSON_Delete(root);
  }
}


static void websocket_event_handler(
    void* handler_args, esp_event_base_t base, int32_t event_id,
    void* event_data
) {
  esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
      if (heartbeat_task_handle) {
        vTaskDelete(heartbeat_task_handle);
        heartbeat_task_handle = NULL;
      }
      last_seq_num = -1;
      break;

    case WEBSOCKET_EVENT_DATA:
      if ((data->op_code != 0x01 && data->op_code != 0x00) ||
          data->data_len <= 0) {
        // op_code we don't care about, or no data.
        break;
      }
      static char* ws_rx_buffer = NULL;
      static int ws_rx_len = 0;

      if (data->payload_offset == 0) {
        if (ws_rx_buffer) {
          free(ws_rx_buffer);
          ws_rx_buffer = NULL;
        }
        ws_rx_buffer = malloc(data->payload_len + 1);
        ws_rx_len = 0;
        if (!ws_rx_buffer) {
          ESP_LOGE(
              TAG, "Failed to allocate memory for JSON string (len: %d)",
              data->payload_len
          );
          break;
        }
      }

      if (!ws_rx_buffer ||
          (data->payload_offset + data->data_len > data->payload_len)) {
        ESP_LOGW(
            TAG, "ws_rx_buffer=%d, payload_offset+data_len=%d, payload_len=%d",
            ws_rx_buffer, data->payload_offset + data->data_len,
            data->payload_len
        );
        break;
      }
      memcpy(
          ws_rx_buffer + data->payload_offset, data->data_ptr, data->data_len
      );
      ws_rx_len += data->data_len;

      if (ws_rx_len != data->payload_len) {
        // not complete message yet
        break;
      }
      ws_rx_buffer[ws_rx_len] = '\0';

      cJSON* root = cJSON_Parse(ws_rx_buffer);
      if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON. Payload length: %d", ws_rx_len);
      } else {
        cJSON* op = cJSON_GetObjectItem(root, "op");
        cJSON* s = cJSON_GetObjectItem(root, "s");
        cJSON* t = cJSON_GetObjectItem(root, "t");
        cJSON* d = cJSON_GetObjectItem(root, "d");

        if (cJSON_IsNumber(s)) {
          last_seq_num = s->valueint;
        }

        if (!cJSON_IsNumber(op)) {
          ESP_LOGW(TAG, "opcode is not number");
        } else {
          int opcode = op->valueint;
          if (opcode == 10) {  // Hello
            if (d && cJSON_IsObject(d)) {
              cJSON* interval = cJSON_GetObjectItem(d, "heartbeat_interval");
              if (cJSON_IsNumber(interval)) {
                heartbeat_interval_ms = interval->valueint;
                ESP_LOGI(
                    TAG, "Received Hello, heartbeat interval: %d ms",
                    heartbeat_interval_ms
                );

                xTaskCreate(
                    heartbeat_task, "heartbeat_task", 4096, NULL, 5,
                    &heartbeat_task_handle
                );

                discord_send_identify(ws_client);
              }
            }
          } else if (opcode == 11) {  // Heartbeat ACK
            ESP_LOGD(TAG, "Received Heartbeat ACK");
          } else if (opcode == 0) {  // Dispatch
            if (cJSON_IsString(t)) {
              ESP_LOGD(TAG, "Received Dispatch Event: %s", t->valuestring);
              if (strcmp(t->valuestring, "READY") == 0) {
                ESP_LOGI(TAG, "Discord Bot is READY!");
                cJSON* app = cJSON_GetObjectItem(d, "application");
                if (app) {
                  cJSON* app_id = cJSON_GetObjectItem(app, "id");
                  if (cJSON_IsString(app_id)) {
                    strncpy(
                        global_app_id, app_id->valuestring,
                        sizeof(global_app_id) - 1
                    );
                    global_app_id[sizeof(global_app_id) - 1] = '\0';
                    register_slash_commands(global_app_id);
                  }
                }
              } else if (strcmp(t->valuestring, "MESSAGE_CREATE") == 0) {
                on_message(d);
              } else if (strcmp(t->valuestring, "INTERACTION_CREATE") == 0) {
                handle_interaction_create(d);
              }
            }
          }
        }
        cJSON_Delete(root);
      }
      free(ws_rx_buffer);
      ws_rx_buffer = NULL;
      ws_rx_len = 0;
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "WEBSOCKET_EVENT_ERROR");
      break;
  }
}


void discord_bot_task(void* pvParameters) {
  ESP_LOGI(TAG, "Starting Discord Bot Task");

  // Initialize LED GPIO
  gpio_reset_pin(CONFIG_LED_GPIO);
  gpio_set_direction(CONFIG_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_LED_GPIO, LED_OFF);

  bot_config = (discord_bot_config_t*)pvParameters;

  if (!bot_config || !bot_config->token) {
    ESP_LOGE(TAG, "Invalid bot configuration");
    vTaskDelete(NULL);
    return;
  }

  const esp_websocket_client_config_t websocket_cfg = {
      .uri = "wss://gateway.discord.gg/?v=10&encoding=json",
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 16384,
      .task_stack = 8192,
      .reconnect_timeout_ms = 5000,
      .network_timeout_ms = 5000,
      .enable_close_reconnect = true,
  };
  ws_client = esp_websocket_client_init(&websocket_cfg);
  if (!ws_client) {
    ESP_LOGE(TAG, "Failed to initialize websocket client");
    vTaskDelete(NULL);
    return;
  }

  esp_websocket_register_events(
      ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)ws_client
  );
  esp_websocket_client_start(ws_client);

  while (1) {
    // we need to keep the structs alive for the websocket task
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
