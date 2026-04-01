#pragma once

#include <Arduino.h>

class BookReader;  // forward declaration

void wifi_upload_init();
void wifi_upload_start();
void wifi_upload_stop();
void wifi_upload_handle();
bool wifi_upload_running();
String wifi_upload_ip();
void wifi_upload_set_reader(BookReader* reader);  // enable book search
