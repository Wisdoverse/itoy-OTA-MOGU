#include "wifi_board.h"
#include "config.h"

#include <esp_log.h>

#define TAG "ItoyMogu"

class ItoyMogu : public WifiBoard {
public:
    ItoyMogu() {
        ESP_LOGI(TAG, "初始化 itoy-mogu 开发板");
    }

    std::string GetBoardType() override {
        return "itoy-mogu";
    }
};

DECLARE_BOARD(ItoyMogu)
