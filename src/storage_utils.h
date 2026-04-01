#pragma once

#include <Arduino.h>

bool storage_write_text_atomic(const String& finalPath, const String& tmpPath, const String& contents);
