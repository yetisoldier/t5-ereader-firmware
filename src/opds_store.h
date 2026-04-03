#pragma once

#include <Arduino.h>
#include "opds_client.h"
#include <vector>

struct OpdsStoreState {
    std::vector<OpdsEntry> entries;
    std::vector<String>    navStack;      // Breadcrumb for back nav
    String                 currentUrl;
    int                    scrollOffset;
    int                    serverIndex;
    String                 statusMsg;
    bool                   downloading;
    int                    downloadPct;
};

void opds_store_init();
void opds_store_draw();
void opds_store_handle_touch(int x, int y);
OpdsStoreState& opds_store_state();
bool opds_store_needs_library_refresh();
void opds_store_clear_refresh_flag();
