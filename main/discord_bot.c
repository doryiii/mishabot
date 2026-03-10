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
#include "misha_bot.h"

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
      ESP_LOGD(TAG, "Discord API %s success (%d)", endpoint, status_code);
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


esp_err_t discord_api_post(const char* path, const char* data) {
  return discord_api_request(HTTP_METHOD_POST, path, data);
}


esp_err_t discord_api_patch(const char* path, const char* data) {
  return discord_api_request(HTTP_METHOD_PATCH, path, data);
}


void send_discord_typing(const char* channel_id) {
  char path[128];
  snprintf(path, sizeof(path), "/channels/%s/typing", channel_id);
  discord_api_post(path, NULL);
}


void send_discord_image_embed(const char* channel_id, const char* image_url) {
  cJSON* root = cJSON_CreateObject();
  cJSON* embeds = cJSON_CreateArray();
  cJSON* embed = cJSON_CreateObject();
  cJSON* image = cJSON_CreateObject();

  cJSON_AddStringToObject(image, "url", image_url);
  cJSON_AddItemToObject(embed, "image", image);
  cJSON_AddItemToArray(embeds, embed);
  cJSON_AddItemToObject(root, "embeds", embeds);

  char* post_data = cJSON_PrintUnformatted(root);

  char path[128];
  snprintf(path, sizeof(path), "/channels/%s/messages", channel_id);
  discord_api_post(path, post_data);

  free(post_data);
  cJSON_Delete(root);
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


static void message_task(void* pvParameters) {
  cJSON* d = (cJSON*)pvParameters;
  if (!d) {
    vTaskDelete(NULL);
  }

  cJSON* author = cJSON_GetObjectItem(d, "author");
  if (!author || !cJSON_IsObject(author)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* username = cJSON_GetObjectItem(author, "username");
  if (!username || !cJSON_IsString(username)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* is_bot = cJSON_GetObjectItem(author, "bot");
  if (cJSON_IsTrue(is_bot)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* content = cJSON_GetObjectItem(d, "content");
  if (!cJSON_IsString(content)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* channel_id = cJSON_GetObjectItem(d, "channel_id");
  if (!cJSON_IsString(channel_id)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  char* username_str = strdup(username->valuestring);
  char* content_str = strdup(content->valuestring);
  char* channel = strdup(channel_id->valuestring);
  cJSON_Delete(d);

  on_message(username_str, content_str, channel);

  free(username_str);
  free(content_str);
  free(channel);
  vTaskDelete(NULL);
}


static void interaction_task(void* pvParameters) {
  cJSON* d = (cJSON*)pvParameters;
  if (!d) {
    vTaskDelete(NULL);
  }

  cJSON* type_item = cJSON_GetObjectItem(d, "type");
  if (!cJSON_IsNumber(type_item)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* id_item = cJSON_GetObjectItem(d, "id");
  cJSON* token_item = cJSON_GetObjectItem(d, "token");
  if (!cJSON_IsString(id_item) || !cJSON_IsString(token_item)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* member = cJSON_GetObjectItem(d, "member");
  cJSON* user = (member && cJSON_IsObject(member))
                    ? cJSON_GetObjectItem(member, "user")
                    : cJSON_GetObjectItem(d, "user");
  if (!user || !cJSON_IsObject(user)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }
  cJSON* uid = cJSON_GetObjectItem(user, "id");
  if (!cJSON_IsString(uid)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  cJSON* data = cJSON_GetObjectItem(d, "data");
  if (!data || !cJSON_IsObject(data)) {
    cJSON_Delete(d);
    vTaskDelete(NULL);
  }

  int int_type = type_item->valueint;
  char* user_id = strdup(uid->valuestring);
  char* id = strdup(id_item->valuestring);
  char* token = strdup(token_item->valuestring);
  char* cmd_name = NULL;
  char* custom_id = NULL;
  cJSON* name_item = cJSON_GetObjectItem(data, "name");
  if (cJSON_IsString(name_item)) cmd_name = strdup(name_item->valuestring);
  cJSON* cid_item = cJSON_GetObjectItem(data, "custom_id");
  if (cJSON_IsString(cid_item)) custom_id = strdup(cid_item->valuestring);

  cJSON_Delete(d);

  if (int_type == 2 && cmd_name) {  // APPLICATION_COMMAND
    on_interaction_cmd(id, token, cmd_name, user_id);
  } else if (int_type == 3 && custom_id) {  // MESSAGE_COMPONENT
    on_interaction_action(global_app_id, id, token, custom_id, user_id);
  }
  free(id);
  free(token);
  free(user_id);
  if (cmd_name) free(cmd_name);
  if (custom_id) free(custom_id);
  vTaskDelete(NULL);
}


static void websocket_event_handler(
    void* handler_args, esp_event_base_t base, int32_t event_id,
    void* event_data
) {
  gpio_set_level(CONFIG_LED_GPIO, LED_ON);

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
        // no buffer or incomplete data; wait for more packets
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
      free(ws_rx_buffer);
      ws_rx_buffer = NULL;
      ws_rx_len = 0;
      if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON.");
        break;
      }

      cJSON* op = cJSON_GetObjectItem(root, "op");
      cJSON* s = cJSON_GetObjectItem(root, "s");
      cJSON* t = cJSON_GetObjectItem(root, "t");
      cJSON* d = cJSON_GetObjectItem(root, "d");

      if (cJSON_IsNumber(s)) {
        last_seq_num = s->valueint;
      }

      if (!cJSON_IsNumber(op)) {
        ESP_LOGW(TAG, "opcode is not number");
        goto cleanup_rootjson;
      }

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
            cJSON* msg_dup = cJSON_Duplicate(d, 1);
            if (msg_dup) {
              xTaskCreate(message_task, "message_task", 8192, msg_dup, 5, NULL);
            }
          } else if (strcmp(t->valuestring, "INTERACTION_CREATE") == 0) {
            cJSON* interaction_dup = cJSON_Duplicate(d, 1);
            if (interaction_dup) {
              xTaskCreate(
                  interaction_task, "int_task", 8192, interaction_dup, 5, NULL
              );
            }
          }
        }
      }
    cleanup_rootjson:
      cJSON_Delete(root);
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGW(TAG, "WEBSOCKET_EVENT_ERROR");
      break;
  }

  gpio_set_level(CONFIG_LED_GPIO, LED_OFF);
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
