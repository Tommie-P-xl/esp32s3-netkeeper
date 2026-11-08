#pragma once
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle);
void vTaskDelete(void *task);
