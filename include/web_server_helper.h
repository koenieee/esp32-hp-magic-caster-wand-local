// Helper macro for broadcasting with disconnect detection
#define BROADCAST_TO_CLIENTS(json_data)                                                          \
    if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(10)) == pdTRUE)                               \
    {                                                                                            \
        for (int i = 0; i < ws_client_count; i++)                                                \
        {                                                                                        \
            int ret = httpd_socket_send(server, ws_clients[i], json_data, strlen(json_data), 0); \
            if (ret < 0)                                                                         \
            {                                                                                    \
                ESP_LOGW(TAG, "Client fd=%d disconnected", ws_clients[i]);                       \
                for (int j = i; j < ws_client_count - 1; j++)                                    \
                {                                                                                \
                    ws_clients[j] = ws_clients[j + 1];                                           \
                }                                                                                \
                ws_client_count--;                                                               \
                i--;                                                                             \
            }                                                                                    \
        }                                                                                        \
        xSemaphoreGive(client_mutex);                                                            \
    }
