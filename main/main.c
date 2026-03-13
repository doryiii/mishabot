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
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "misha_bot.h"
#include "nvs_flash.h"
#include "wifi_station.h"

static const char* TAG = "main";

#ifdef CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
#define MEM_MON_STACK 3072
#else
#define MEM_MON_STACK 2048
#endif

static void memory_monitor_task(void* pvParameter) {
  while (1) {
    // Wait for Enter key (blocks until newline received)
    int c = getchar();
    if (c == '\n' || c == '\r') {
      ESP_LOGI(
          "MEM", "Free: %zu | Min: %zu",
          heap_caps_get_free_size(MALLOC_CAP_8BIT),
          heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)
      );
#ifdef CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
      char task_list[1024];
      vTaskList(task_list);
      ESP_LOGI("TASKS", "\n%s", task_list);
#endif
    }
  }
}


void app_main(void) {
  xTaskCreate(memory_monitor_task, "mem_mon", MEM_MON_STACK, NULL, 1, NULL);

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
    esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
  }

  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
  wifi_init_sta();

  /* Check if we are connected */
  EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
  if (!(bits & WIFI_CONNECTED_BIT)) {
    ESP_LOGE(TAG, "no wifi, exiting");
  }

  misha_bot_init(CONFIG_DISCORD_BOT_TOKEN);
}
