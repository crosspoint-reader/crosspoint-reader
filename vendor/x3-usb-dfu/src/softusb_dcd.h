#ifndef X3_SOFTUSB_DCD_H
#define X3_SOFTUSB_DCD_H
#include "softusb.h"
#ifdef __cplusplus
extern "C" {
#endif
// Board backend must provide these functions. No implicit GPIO/native USB path.
bool su_phy_init(void);
void su_phy_deinit(void);
void su_phy_connect(bool connected);
void su_phy_irq(bool enabled);
void su_phy_interrupt(void);
// Call after RX/response timing is complete, with packet processing excluded.
// Caller runs tud_task() afterward, outside the timing-critical PHY path.
SU_HOT void su_dcd_service(void);
// Bind only while the controller is stopped; caller owns the connection state.
void su_dcd_bind_device(su_device* device);
SU_HOT su_device* su_dcd_device(void);
#ifdef __cplusplus
}
#endif
#endif
