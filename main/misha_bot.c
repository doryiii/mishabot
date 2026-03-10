#include "misha_bot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "discord_bot.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char* TAG = "misha_bot";

#define DANBOORU_BASE                                                         \
  "https://danbooru.donmai.us/posts/random.json?login=" CONFIG_DANBOORU_LOGIN \
  "&api_key=" CONFIG_DANBOORU_API_KEY "&tags="

static char* fetch_danbooru(const char* tags, const char* filter) {
  char* image_url = NULL;

  char url[256];
  snprintf(url, sizeof(url), "%s%s+%s+-animated", DANBOORU_BASE, tags, filter);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_GET,
      .crt_bundle_attach = esp_crt_bundle_attach,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  esp_err_t err = esp_http_client_open(client, 0);
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(client);

    int content_length = esp_http_client_get_content_length(client);
    int total_read = 0;
    int buffer_size = content_length > 0 ? content_length + 1 : 2048;
    char* buf = malloc(buffer_size);

    if (buf) {
      while (1) {
        if (content_length <= 0 && total_read == buffer_size - 1) {
          buffer_size += 1024;
          char* new_buf = realloc(buf, buffer_size);
          if (!new_buf) {
            ESP_LOGE(TAG, "Failed to reallocate buffer");
            break;
          }
          buf = new_buf;
        }

        int to_read = buffer_size - 1 - total_read;
        int read_len = esp_http_client_read(client, buf + total_read, to_read);
        if (read_len <= 0) {
          break;
        }
        total_read += read_len;
      }

      buf[total_read] = '\0';
      if (total_read > 0) {
        ESP_LOGI(TAG, "Danbooru response received: %d bytes", total_read);
        cJSON* root = cJSON_Parse(buf);
        if (root) {
          cJSON* file_url = cJSON_GetObjectItem(root, "file_url");
          if (cJSON_IsString(file_url)) {
            image_url = strdup(file_url->valuestring);
          }
          cJSON_Delete(root);
        }
      }
      free(buf);
    }
  } else {
    ESP_LOGE(TAG, "Failed to fetch from Danbooru: %s", esp_err_to_name(err));
  }

  esp_http_client_cleanup(client);
  ESP_LOGI(TAG, "image: %s", image_url);
  return image_url;
}


char* fetch_danbooru_safe_img(const char* tags) {
  return fetch_danbooru(tags, "rating%3Ageneral");
}


char* fetch_danbooru_risky_img(const char* tags) {
  return fetch_danbooru(tags, "rating%3Asafe");
}


static void handle_character_command(const char* channel_id, const char* tags) {
  send_discord_typing(channel_id);

  ESP_LOGI(TAG, "Getting %s images", tags);
  char* image_url = fetch_danbooru_safe_img(tags);
  if (image_url) {
    send_discord_image_embed(channel_id, image_url);
    free(image_url);
  } else {
    ESP_LOGW(TAG, "Could not get image URL from Danbooru for %s", tags);
  }
}


void on_message(
    const char* username, const char* content, const char* channel
) {
  // debug logging
  if (content[0] == '.') {
    ESP_LOGI(TAG, "cmd: %s", content);
  }

  if (strcmp(content, ".misha") == 0) {
    handle_character_command(channel, "misha_%28honkai%3A_star_rail%29");

  } else if (strcmp(content, ".furina") == 0) {
    handle_character_command(channel, "furina_%28genshin_impact%29");

  } else if (strcmp(content, ".karen") == 0) {
    handle_character_command(channel, "kujou_karen");

  } else if (strcmp(content, ".kokomi") == 0) {
    handle_character_command(channel, "sangonomiya_kokomi");

  } else if (strcmp(content, ".reisen") == 0) {
    handle_character_command(channel, "reisen_udongein_inaba");

  } else if (strcmp(content, ".ika") == 0) {
    handle_character_command(channel, "ikamusume");

  } else if (strcmp(content, ".amber") == 0) {
    handle_character_command(channel, "amber_%28genshin_impact%29");

  } else if (strcmp(content, ".venti") == 0) {
    handle_character_command(channel, "venti_%28genshin_impact%29");

  } else {
    ESP_LOGV(TAG, "[%s]: %s", username, content);
  }
}

void on_interaction_cmd(
    const char* id, const char* token, const char* cmd, const char* user_id
) {
  if (strcmp(cmd, "fish") == 0) {
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
    char payload[1024];
    snprintf(
        payload, sizeof(payload),
        "{\"type\":4,\"data\":{\"content\":\"You cast your line... %s How do "
        "you reel it in?\",\"components\":[{\"type\":1,\"components\":["
        "{\"type\":2,\"style\":1,\"custom_id\":\"fish_gentle_%d_%s\","
        "\"label\":\"Reel gently\"},"
        "{\"type\":2,\"style\":1,\"custom_id\":\"fish_fast_%d_%s\",\"label\":"
        "\"Reel faster\"},"
        "{\"type\":2,\"style\":1,\"custom_id\":\"fish_erratic_%d_%s\","
        "\"label\":\"Reel erratically\"},"
        "{\"type\":2,\"style\":1,\"custom_id\":\"fish_suggestive_%d_%s\","
        "\"label\":\"Reel suggestively\"}]}]}}",
        fish_events[event_id], event_id, user_id, event_id, user_id, event_id,
        user_id, event_id, user_id
    );
    char path[512];
    snprintf(path, sizeof(path), "/interactions/%s/%s/callback", id, token);
    discord_api_post(path, payload);
  }
}

void on_interaction_action(
    const char* global_app_id, const char* id, const char* token,
    const char* custom_action_id, const char* user_id
) {
  if (strncmp(custom_action_id, "fish_", 5) == 0) {
    char action[32];
    int event_id;
    char orig_uid[32];
    if (sscanf(
            custom_action_id, "fish_%31[^_]_%d_%31s", action, &event_id,
            orig_uid
        ) == 3) {
      char path[512];
      snprintf(path, sizeof(path), "/interactions/%s/%s/callback", id, token);

      if (strcmp(user_id, orig_uid) != 0) {
        // not original user clicking button
        const char* eph =
            "{\"type\":4,\"data\":{\"content\":\"This is not your fishing "
            "line!\",\"flags\":64}}";
        discord_api_post(path, eph);

      } else {
        discord_api_post(path, "{\"type\":6}");  // send "defer interaction"

        int chance = (strcmp(action, "suggestive") == 0) ? 30 : 50;
        if ((strcmp(action, "gentle") == 0 && event_id % 4 == 0) ||
            (strcmp(action, "fast") == 0 && event_id % 4 == 1) ||
            (strcmp(action, "erratic") == 0 && event_id % 4 == 2) ||
            (strcmp(action, "suggestive") == 0 && event_id % 4 == 3))
          chance += 15;
        bool won = ((int)(esp_random() % 100) < chance);
        cJSON* patch = cJSON_CreateObject();
        cJSON_AddArrayToObject(patch, "components");
        if (won) {
          const char* pool[] = {
              "sangonomiya_kokomi+-comic+-everyone",
              "mualani_%28genshin_impact%29+-comic+-everyone",
              "ikamusume+-comic+-everyone", "gawr_gura+-comic+-everyone",
              "sameko_saba+-comic+-everyone"
          };
          const char* names[] = {
              "Kokomi", "Mualani", "squid", "shark", "mackerel"
          };
          int idx = esp_random() % 5;
          char buf[64];
          snprintf(buf, sizeof(buf), "You caught a %s!", names[idx]);
          cJSON_AddStringToObject(patch, "content", buf);
          char* img = (strcmp(action, "suggestive") == 0)
                          ? fetch_danbooru_risky_img(pool[idx])
                          : fetch_danbooru_safe_img(pool[idx]);
          if (img) {
            cJSON* embeds = cJSON_CreateArray();
            cJSON* embed = cJSON_CreateObject();
            cJSON* image = cJSON_CreateObject();
            cJSON_AddStringToObject(image, "url", img);
            cJSON_AddItemToObject(embed, "image", image);
            cJSON_AddItemToArray(embeds, embed);
            cJSON_AddItemToObject(patch, "embeds", embeds);
            free(img);
          }
        } else {
          cJSON_AddStringToObject(patch, "content", "The fish got away...");
        }

        char* pstr = cJSON_PrintUnformatted(patch);
        if (global_app_id[0] != '\0' && pstr) {
          snprintf(
              path, sizeof(path), "/webhooks/%s/%s/messages/@original",
              global_app_id, token
          );
          discord_api_patch(path, pstr);
        }
        if (pstr) {
          free(pstr);
        }
        cJSON_Delete(patch);
      }
    }
  }
}
