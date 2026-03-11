#pragma once

void register_slash_commands(const char* app_id);
void on_message(const char* username, const char* content, const char* channel);
void on_interaction_cmd(
    const char* id, const char* token, const char* cmd, const char* user_id
);
void on_interaction_action(
    const char* global_app_id, const char* id, const char* token,
    const char* custom_action_id, const char* user_id
);
