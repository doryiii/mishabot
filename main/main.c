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
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "misha_bot.h"
#include "nvs_flash.h"
#include "wifi_station.h"

static const char* TAG = "main";


#ifdef CONFIG_LED_INVERTED
#define LED_ON 0
#define LED_OFF 1
#else
#define LED_ON 1
#define LED_OFF 0
#endif

static void heartbeat_led_task(void* pvParameter) {
  gpio_reset_pin(CONFIG_LED_GPIO);
  gpio_set_direction(CONFIG_LED_GPIO, GPIO_MODE_OUTPUT);
  while (1) {
    gpio_set_level(CONFIG_LED_GPIO, LED_ON);
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_level(CONFIG_LED_GPIO, LED_OFF);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}


void app_main(void) {
  xTaskCreate(heartbeat_led_task, "heartbeat_led", 1024, NULL, 1, NULL);

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
