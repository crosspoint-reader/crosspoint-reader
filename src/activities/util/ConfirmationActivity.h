#pragma once
#include "../Activity.h"
#include <functional>
#include <string>

class ConfirmationActivity : public Activity {
public:
    ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                         const std::string& message,
                         std::function<void(bool)> onResult); 

    void onEnter() override;
    void loop() override;

private:
    std::string message;
    std::function<void(bool)> onResult;
};