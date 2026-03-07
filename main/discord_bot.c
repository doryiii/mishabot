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

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "discord_bot";

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
static discord_bot_config_t *bot_config = NULL;

#define DANBOORU_API_BASE                                                      \
  "https://danbooru.donmai.us/posts/random.json?login=" CONFIG_DANBOORU_LOGIN  \
  "&api_key=" CONFIG_DANBOORU_API_KEY "&tags=rating%3Ageneral+"

static void discord_send_identify(esp_websocket_client_handle_t client) {
  if (!bot_config)
    return;

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "op", 2);

  cJSON *d = cJSON_CreateObject();
  cJSON_AddStringToObject(d, "token", bot_config->token);
  cJSON_AddNumberToObject(d, "intents", bot_config->intents);

  cJSON *props = cJSON_CreateObject();
  cJSON_AddStringToObject(props, "os", "linux");
  cJSON_AddStringToObject(props, "browser", "esp32");
  cJSON_AddStringToObject(props, "device", "esp32");

  cJSON_AddItemToObject(d, "properties", props);
  cJSON_AddItemToObject(root, "d", d);

  char *payload = cJSON_PrintUnformatted(root);
  if (payload) {
    ESP_LOGI(TAG, "Sending Identify");
    esp_websocket_client_send_text(
        client, payload, strlen(payload), portMAX_DELAY
    );
    free(payload);
  }
  cJSON_Delete(root);
}

static esp_err_t discord_api_post(
    const char *channel_id, const char *endpoint, const char *post_data
) {
  if (!bot_config) {
    return ESP_ERR_INVALID_STATE;
  }

  char url[256];
  snprintf(
      url, sizeof(url), "https://discord.com/api/v10/channels/%s/%s",
      channel_id, endpoint
  );

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  char auth_header[256];
  snprintf(auth_header, sizeof(auth_header), "Bot %s", bot_config->token);
  esp_http_client_set_header(client, "Authorization", auth_header);
  esp_http_client_set_header(client, "Content-Type", "application/json");

  if (post_data) {
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
  }

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    if (status_code >= 200 && status_code < 300) {
      ESP_LOGI(TAG, "Discord POST %s success (%d)", endpoint, status_code);
    } else {
      ESP_LOGE(TAG, "Discord POST %s failed (%d)", endpoint, status_code);
      err = ESP_FAIL;
    }
  } else {
    ESP_LOGE(TAG, "Discord POST %s error: %s", endpoint, esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  return err;
}

static void send_discord_typing(const char *channel_id) {
  discord_api_post(channel_id, "typing", NULL);
}

static void
send_discord_image_embed(const char *channel_id, const char *image_url) {
  cJSON *root = cJSON_CreateObject();
  cJSON *embeds = cJSON_CreateArray();
  cJSON *embed = cJSON_CreateObject();
  cJSON *image = cJSON_CreateObject();

  cJSON_AddStringToObject(image, "url", image_url);
  cJSON_AddItemToObject(embed, "image", image);
  cJSON_AddItemToArray(embeds, embed);
  cJSON_AddItemToObject(root, "embeds", embeds);

  char *post_data = cJSON_PrintUnformatted(root);

  discord_api_post(channel_id, "messages", post_data);

  free(post_data);
  cJSON_Delete(root);
}

static char *fetch_danbooru_image_url(const char *tags) {
  char *image_url = NULL;
  // TODO: handle http chunks to avoid allocating max buf every time
  const int buffer_size = 12288;
  char *buffer = malloc(buffer_size);
  if (!buffer)
    return NULL;

  char url[256];
  snprintf(url, sizeof(url), "%s%s", DANBOORU_API_BASE, tags);

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
      cJSON *root = cJSON_Parse(buffer);
      if (root) {
        cJSON *file_url = cJSON_GetObjectItem(root, "file_url");
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
  return image_url;
}

static void handle_character_command(const char *channel_id, const char *tags) {
  gpio_set_level(CONFIG_LED_GPIO, LED_ON);
  send_discord_typing(channel_id);

  char *image_url = fetch_danbooru_image_url(tags);
  if (image_url) {
    send_discord_image_embed(channel_id, image_url);
    free(image_url);
  } else {
    ESP_LOGW(TAG, "Could not get image URL from Danbooru for %s", tags);
  }

  gpio_set_level(CONFIG_LED_GPIO, LED_OFF);
}

static void on_message(cJSON *d) {
  cJSON *author = cJSON_GetObjectItem(d, "author");
  if (!author || !cJSON_IsObject(author))
    return;

  cJSON *is_bot = cJSON_GetObjectItem(author, "bot");
  if (cJSON_IsTrue(is_bot))
    return;

  cJSON *content = cJSON_GetObjectItem(d, "content");
  if (!cJSON_IsString(content))
    return;

  cJSON *channel_id = cJSON_GetObjectItem(d, "channel_id");
  if (!cJSON_IsString(channel_id))
    return;

  if (strcmp(content->valuestring, ".misha") == 0) {
    ESP_LOGI(TAG, ".misha");
    handle_character_command(
        channel_id->valuestring,
        "misha_%28honkai%3A_star_rail%29"
    );
    return;
  } else if (strcmp(content->valuestring, ".furina") == 0) {
    ESP_LOGI(TAG, "Command .furina detected");
    handle_character_command(
        channel_id->valuestring, "furina_%28genshin_impact%29"
    );
    return;
  } else if (strcmp(content->valuestring, ".karen") == 0) {
    ESP_LOGI(TAG, "Command .karen detected");
    handle_character_command(channel_id->valuestring, "kujou_karen");
    return;
  } else if (strcmp(content->valuestring, ".kokomi") == 0) {
    ESP_LOGI(TAG, "Command .kokomi detected");
    handle_character_command(channel_id->valuestring, "sangonomiya_kokomi");
    return;
  } else if (strcmp(content->valuestring, ".reisen") == 0) {
    ESP_LOGI(TAG, "Command .reisen detected");
    handle_character_command(channel_id->valuestring, "reisen_udongein_inaba");
    return;
  }

  cJSON *username = cJSON_GetObjectItem(author, "username");
  if (cJSON_IsString(username)) {
    ESP_LOGV(
        TAG, "New message from %s: %s", username->valuestring,
        content->valuestring
    );
  }
}

static void heartbeat_task(void *pvParameters) {
  while (1) {
    if (heartbeat_interval_ms <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(heartbeat_interval_ms));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "op", 1);
    if (last_seq_num >= 0) {
      cJSON_AddNumberToObject(root, "d", last_seq_num);
    } else {
      cJSON_AddNullToObject(root, "d");
    }

    char *payload = cJSON_PrintUnformatted(root);
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
    void *handler_args, esp_event_base_t base, int32_t event_id,
    void *event_data
) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
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
    break;

  case WEBSOCKET_EVENT_DATA:
    if (data->op_code == 0x01 && data->data_len > 0) { // Text frame
      char *json_str = malloc(data->data_len + 1);
      if (!json_str) {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON string");
        break;
      }
      memcpy(json_str, data->data_ptr, data->data_len);
      json_str[data->data_len] = '\0';

      cJSON *root = cJSON_Parse(json_str);
      if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON");
        free(json_str);
        break;
      }

      cJSON *op = cJSON_GetObjectItem(root, "op");
      cJSON *s = cJSON_GetObjectItem(root, "s");
      cJSON *t = cJSON_GetObjectItem(root, "t");
      cJSON *d = cJSON_GetObjectItem(root, "d");

      if (cJSON_IsNumber(s)) {
        last_seq_num = s->valueint;
      }

      if (!cJSON_IsNumber(op)) {
        ESP_LOGW(TAG, "opcode is not number");
        cJSON_Delete(root);
        free(json_str);
        break;
      }

      int opcode = op->valueint;
      if (opcode == 10) { // Hello
        if (d && cJSON_IsObject(d)) {
          cJSON *interval = cJSON_GetObjectItem(d, "heartbeat_interval");
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
      } else if (opcode == 11) { // Heartbeat ACK
        ESP_LOGD(TAG, "Received Heartbeat ACK");
      } else if (opcode == 0) { // Dispatch
        if (cJSON_IsString(t)) {
          ESP_LOGD(TAG, "Received Dispatch Event: %s", t->valuestring);
          if (strcmp(t->valuestring, "READY") == 0) {
            ESP_LOGI(TAG, "Discord Bot is READY!");
          } else if (strcmp(t->valuestring, "MESSAGE_CREATE") == 0) {
            on_message(d);
          }
        }
      }
      cJSON_Delete(root);
      free(json_str);
    }
    break;

  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGW(TAG, "WEBSOCKET_EVENT_ERROR");
    break;
  }
}

void discord_bot_task(void *pvParameters) {
  ESP_LOGI(TAG, "Starting Discord Bot Task");

  // Initialize LED GPIO
  gpio_reset_pin(CONFIG_LED_GPIO);
  gpio_set_direction(CONFIG_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(CONFIG_LED_GPIO, LED_OFF);

  bot_config = (discord_bot_config_t *)pvParameters;

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
  };
  ws_client = esp_websocket_client_init(&websocket_cfg);
  if (!ws_client) {
    ESP_LOGE(TAG, "Failed to initialize websocket client");
    vTaskDelete(NULL);
    return;
  }

  esp_websocket_register_events(
      ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client
  );
  esp_websocket_client_start(ws_client);

  while (1) {
    // we need to keep the structs alive for the websocket task
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
