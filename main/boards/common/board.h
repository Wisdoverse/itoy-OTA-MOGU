#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <string>
#include <network_interface.h>

#include <esp_log.h>

void* create_board();

class Board {
private:
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;

protected:
    Board();
    std::string GenerateUuid();
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetPowerSaveMode(bool enabled) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetSystemInfoJson();
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
