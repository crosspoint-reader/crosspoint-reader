/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUD_ENABLED

#include "device/dcd.h"
#include "tusb.h"
#include "common/tusb_private.h"

#include "device/usbd.h"
#include "device/usbd_pvt.h"

//--------------------------------------------------------------------+
// USBD Configuration
//--------------------------------------------------------------------+
#ifndef CFG_TUD_TASK_QUEUE_SZ
  #define CFG_TUD_TASK_QUEUE_SZ   16
#endif

//--------------------------------------------------------------------+
// Weak stubs: invoked if no strong implementation is available
//--------------------------------------------------------------------+
TU_ATTR_WEAK void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
  (void) rhport; (void) eventid; (void) in_isr;
}

TU_ATTR_WEAK void tud_sof_cb(uint32_t frame_count) {
  (void) frame_count;
}

TU_ATTR_WEAK uint8_t const* tud_descriptor_bos_cb(void) {
  return NULL;
}

TU_ATTR_WEAK uint8_t const* tud_descriptor_device_qualifier_cb(void) {
  return NULL;
}

TU_ATTR_WEAK uint8_t const* tud_descriptor_other_speed_configuration_cb(uint8_t index) {
  (void) index;
  return NULL;
}

TU_ATTR_WEAK void tud_mount_cb(void) {
}

TU_ATTR_WEAK void tud_umount_cb(void) {
}

TU_ATTR_WEAK void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
}

TU_ATTR_WEAK void tud_resume_cb(void) {
}

TU_ATTR_WEAK bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
  (void) rhport; (void) stage; (void) request;
  return false;
}

TU_ATTR_WEAK bool dcd_deinit(uint8_t rhport) {
  (void) rhport;
  return false;
}

TU_ATTR_WEAK void dcd_connect(uint8_t rhport) {
  (void) rhport;
}

TU_ATTR_WEAK void dcd_disconnect(uint8_t rhport) {
  (void) rhport;
}

TU_ATTR_WEAK bool dcd_dcache_clean(const void* addr, uint32_t data_size) {
  (void) addr; (void) data_size;
  return true;
}

TU_ATTR_WEAK bool dcd_dcache_invalidate(const void* addr, uint32_t data_size) {
  (void) addr; (void) data_size;
  return true;
}

TU_ATTR_WEAK bool dcd_dcache_clean_invalidate(const void* addr, uint32_t data_size) {
  (void) addr; (void) data_size;
  return true;
}

//--------------------------------------------------------------------+
// Device Data
//--------------------------------------------------------------------+

// Per-control-transfer state: populated at process_setup_received() entry,
// consumed asynchronously by usbd_control_xfer_cb() when the EP0 transfer completes.
typedef struct {
  tusb_control_request_t request;
  uint8_t* buffer;
  uint16_t data_len;
  uint16_t total_xferred;
  usbd_control_xfer_cb_t complete_cb;
} usbd_control_xfer_t;

typedef struct {
  usbd_control_xfer_t ctrl_xfer;

  // Note: these may share an enum state
  volatile uint8_t connected;
  volatile uint8_t addressed;
  volatile uint8_t suspended;

  union {
    struct TU_ATTR_PACKED {
      uint8_t self_powered     : 1; // configuration descriptor's attribute;
      uint8_t remote_wakeup_en : 1; // enable/disable by host
    };
    uint8_t dev_state_bm;
  };

  uint8_t          cfg_num; // current active configuration (0x00 is not configured)
  uint8_t          speed;
  volatile uint8_t sof_consumer;

  uint8_t itf2drv[CFG_TUD_INTERFACE_MAX];   // map interface number to driver (0xff is invalid)
  uint8_t ep2drv[CFG_TUD_ENDPPOINT_MAX][2]; // map endpoint to driver ( 0xff is invalid ), can use only 4-bit each

  volatile uint8_t ep_status[CFG_TUD_ENDPPOINT_MAX][2];
} usbd_device_t;

static usbd_device_t    _usbd_dev;
static volatile uint8_t _usbd_queued_setup;

CFG_TUD_MEM_SECTION static struct {
  TUD_EPBUF_DEF(buf, CFG_TUD_ENDPOINT0_BUFSIZE);
} _ctrl_epbuf;

//--------------------------------------------------------------------+
// Class Driver
//--------------------------------------------------------------------+
  #if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    #define DRIVER_NAME(_name) _name
  #else
    #define DRIVER_NAME(_name) NULL
  #endif

// Built-in class drivers
static const usbd_class_driver_t _usbd_driver[] = {
  #if CFG_TUD_CDC
    {
        .name             = DRIVER_NAME("CDC"),
        .init             = cdcd_init,
        .deinit           = cdcd_deinit,
        .reset            = cdcd_reset,
        .open             = cdcd_open,
        .control_xfer_cb  = cdcd_control_xfer_cb,
        .xfer_cb          = cdcd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_MSC
    {
        .name             = DRIVER_NAME("MSC"),
        .init             = mscd_init,
        .deinit           = NULL,
        .reset            = mscd_reset,
        .open             = mscd_open,
        .control_xfer_cb  = mscd_control_xfer_cb,
        .xfer_cb          = mscd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_HID
    {
        .name             = DRIVER_NAME("HID"),
        .init             = hidd_init,
        .deinit           = hidd_deinit,
        .reset            = hidd_reset,
        .open             = hidd_open,
        .control_xfer_cb  = hidd_control_xfer_cb,
        .xfer_cb          = hidd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_AUDIO
    {
        .name             = DRIVER_NAME("AUDIO"),
        .init             = audiod_init,
        .deinit           = audiod_deinit,
        .reset            = audiod_reset,
        .open             = audiod_open,
        .control_xfer_cb  = audiod_control_xfer_cb,
        .xfer_cb          = audiod_xfer_cb,
        .xfer_isr         = audiod_xfer_isr,
        .sof              = audiod_sof_isr
    },
    #endif

    #if CFG_TUD_VIDEO
    {
        .name             = DRIVER_NAME("VIDEO"),
        .init             = videod_init,
        .deinit           = videod_deinit,
        .reset            = videod_reset,
        .open             = videod_open,
        .control_xfer_cb  = videod_control_xfer_cb,
        .xfer_cb          = videod_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_MIDI
    {
        .name             = DRIVER_NAME("MIDI"),
        .init             = midid_init,
        .deinit           = midid_deinit,
        .open             = midid_open,
        .reset            = midid_reset,
        .control_xfer_cb  = midid_control_xfer_cb,
        .xfer_cb          = midid_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_MIDI2
    {
        .name             = DRIVER_NAME("MIDI2"),
        .init             = midi2d_init,
        .deinit           = midi2d_deinit,
        .open             = midi2d_open,
        .reset            = midi2d_reset,
        .control_xfer_cb  = midi2d_control_xfer_cb,
        .xfer_cb          = midi2d_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_VENDOR
    {
        .name             = DRIVER_NAME("VENDOR"),
        .init             = vendord_init,
        .deinit           = vendord_deinit,
        .reset            = vendord_reset,
        .open             = vendord_open,
        .control_xfer_cb  = vendord_control_xfer_cb,
        .xfer_cb          = vendord_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_USBTMC
    {
        .name             = DRIVER_NAME("TMC"),
        .init             = usbtmcd_init_cb,
        .deinit           = usbtmcd_deinit,
        .reset            = usbtmcd_reset_cb,
        .open             = usbtmcd_open_cb,
        .control_xfer_cb  = usbtmcd_control_xfer_cb,
        .xfer_cb          = usbtmcd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_DFU_RUNTIME
    {
        .name             = DRIVER_NAME("DFU-RUNTIME"),
        .init             = dfu_rtd_init,
        .deinit           = dfu_rtd_deinit,
        .reset            = dfu_rtd_reset,
        .open             = dfu_rtd_open,
        .control_xfer_cb  = dfu_rtd_control_xfer_cb,
        .xfer_cb          = NULL,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_DFU
    {
        .name             = DRIVER_NAME("DFU"),
        .init             = dfu_moded_init,
        .deinit           = dfu_moded_deinit,
        .reset            = dfu_moded_reset,
        .open             = dfu_moded_open,
        .control_xfer_cb  = dfu_moded_control_xfer_cb,
        .xfer_cb          = NULL,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_ECM_RNDIS || CFG_TUD_NCM
    {
        .name             = DRIVER_NAME("NET"),
        .init             = netd_init,
        .deinit           = netd_deinit,
        .reset            = netd_reset,
        .open             = netd_open,
        .control_xfer_cb  = netd_control_xfer_cb,
        .xfer_cb          = netd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL,
    },
    #endif

    #if CFG_TUD_BTH
    {
        .name             = DRIVER_NAME("BTH"),
        .init             = btd_init,
        .deinit           = btd_deinit,
        .reset            = btd_reset,
        .open             = btd_open,
        .control_xfer_cb  = btd_control_xfer_cb,
        .xfer_cb          = btd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_MTP
    {
        .name             = DRIVER_NAME("MTP"),
        .init             = mtpd_init,
        .deinit           = mtpd_deinit,
        .reset            = mtpd_reset,
        .open             = mtpd_open,
        .control_xfer_cb  = mtpd_control_xfer_cb,
        .xfer_cb          = mtpd_xfer_cb,
        .xfer_isr         = NULL,
        .sof              = NULL
    },
    #endif

    #if CFG_TUD_PRINTER
    {
        .name             = DRIVER_NAME("PRINTER"),
        .init             = printerd_init,
        .deinit           = printerd_deinit,
        .reset            = printerd_reset,
        .open             = printerd_open,
        .control_xfer_cb  = printerd_control_xfer_cb,
        .xfer_cb          = printerd_xfer_cb,
        .sof              = NULL
    },
    #endif
};

// Additional class drivers implemented by application
static const usbd_class_driver_t *_app_driver = NULL;
static const uint8_t _builtin_driver_count    = TU_ARRAY_SIZE(_usbd_driver);
static uint8_t              _app_driver_count = 0;

#define TOTAL_DRIVER_COUNT    ((uint8_t) (_app_driver_count + _builtin_driver_count))

// virtually joins built-in and application drivers together.
// Application is positioned first to allow overwriting built-in ones.
TU_ATTR_ALWAYS_INLINE static inline usbd_class_driver_t const * get_driver(uint8_t drvid) {
  usbd_class_driver_t const *driver = NULL;
  if (drvid < _app_driver_count) {
    // Application drivers
    driver = &_app_driver[drvid];
  } else{
    drvid -= _app_driver_count;
    if (_builtin_driver_count > 0 && drvid < _builtin_driver_count) {
      driver = &_usbd_driver[drvid];
    }
  }

  return driver;
}

//--------------------------------------------------------------------+
// DCD Event
//--------------------------------------------------------------------+
enum {
  RHPORT_INVALID = 0xFFu
};
static uint8_t _usbd_rhport = RHPORT_INVALID;

static OSAL_SPINLOCK_DEF(_usbd_spin, usbd_int_set);

// Event queue: usbd_int_set() is used as mutex in OS NONE config
OSAL_QUEUE_DEF(usbd_int_set, _usbd_qdef, CFG_TUD_TASK_QUEUE_SZ, dcd_event_t);
static osal_queue_t _usbd_q;

// held by usbd_defer_func_after_queue(), set and run by the usbd task only
static struct {
  osal_task_func_t func;
  void* param;
} _usbd_after_queue;

// Mutex for claiming endpoint
#if OSAL_MUTEX_REQUIRED
  static osal_mutex_def_t _ubsd_mutexdef;
  static osal_mutex_t _usbd_mutex;
#else
  #define _usbd_mutex   NULL
#endif

TU_ATTR_ALWAYS_INLINE static inline bool queue_event(dcd_event_t const * event, bool in_isr) {
  TU_ASSERT(osal_queue_send(_usbd_q, event, in_isr));
  tud_event_hook_cb(event->rhport, event->event_id, in_isr);
  return true;
}

//--------------------------------------------------------------------+
// Prototypes
//--------------------------------------------------------------------+
static bool usbd_control_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
static bool process_setup_received(uint8_t rhport, tusb_control_request_t const * p_request);
static bool process_get_status(uint8_t rhport, tusb_control_request_t const * request, uint16_t status);
static bool process_set_config(uint8_t rhport, uint8_t cfg_num);
static bool process_get_descriptor(uint8_t rhport, tusb_control_request_t const * p_request);

#if CFG_TUD_TEST_MODE
static bool process_test_mode_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request) {
  TU_VERIFY(CONTROL_STAGE_ACK == stage);
  uint8_t const selector = tu_u16_high(request->wIndex);
  TU_LOG_USBD("    Enter Test Mode (test selector index: %d)\r\n", selector);
  dcd_enter_test_mode(rhport, (tusb_feature_test_mode_t) selector);
  return true;
}
#endif

//--------------------------------------------------------------------+
// Weak stubs: invoked if no strong implementation is available
//--------------------------------------------------------------------+
TU_ATTR_WEAK usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
  *driver_count = 0;
  return NULL;
}

TU_ATTR_WEAK bool dcd_edpt_xfer_fifo(uint8_t rhport, uint8_t ep_addr, tu_fifo_t * ff, uint16_t total_bytes, bool is_isr) {
  (void) rhport; (void) ep_addr; (void) ff; (void) total_bytes; (void) is_isr;
  return false;
}

TU_ATTR_WEAK bool dcd_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg_param) {
  (void) rhport; (void) cfg_id; (void) cfg_param;
  return false;
}

//--------------------------------------------------------------------+
// Debug
//--------------------------------------------------------------------+
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
static char const *const _usbd_event_str[DCD_EVENT_COUNT] = {
    "Invalid",
    "Bus Reset Start",
    "Bus Reset End",
    "Unplugged",
    "SOF",
    "Suspend",
    "Resume",
    "Setup Received",
    "Xfer Complete",
    "Func Call"
};

#endif

//--------------------------------------------------------------------+
// Application API
//--------------------------------------------------------------------+
tusb_speed_t tud_speed_get(void) {
  return (tusb_speed_t) _usbd_dev.speed;
}

bool tud_connected(void) {
  return _usbd_dev.connected;
}

bool tud_mounted(void) {
  return _usbd_dev.cfg_num ? true : false;
}

bool tud_suspended(void) {
  return _usbd_dev.suspended;
}

bool tud_remote_wakeup(void) {
  // only wake up host if this feature is enabled and we are suspended
  TU_VERIFY(_usbd_dev.suspended && _usbd_dev.remote_wakeup_en);
  dcd_remote_wakeup(_usbd_rhport);
  return true;
}

bool tud_disconnect(void) {
  dcd_disconnect(_usbd_rhport);
  return true;
}

bool tud_connect(void) {
  dcd_connect(_usbd_rhport);
  return true;
}

void tud_sof_cb_enable(bool en) {
  usbd_sof_enable(_usbd_rhport, SOF_CONSUMER_USER, en);
}

bool tud_inited(void) {
  return _usbd_rhport != RHPORT_INVALID;
}

bool tud_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg_param) {
  return dcd_configure(rhport, cfg_id, cfg_param);
}

bool tud_rhport_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  if (tud_inited()) {
    return true; // skip if already initialized
  }
  TU_ASSERT(rh_init);
 #if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
  char const* speed_str = 0;
  switch (rh_init->speed) {
    case TUSB_SPEED_HIGH:
      speed_str = "High";
    break;
    case TUSB_SPEED_FULL:
      speed_str = "Full";
    break;
    case TUSB_SPEED_LOW:
      speed_str = "Low";
    break;
    case TUSB_SPEED_AUTO:
      speed_str = "Auto";
    break;
  default:
    break;
  }
  TU_LOG_USBD("USBD init on controller %u, speed = %s\r\n", rhport, speed_str);
  TU_LOG_INT(CFG_TUD_LOG_LEVEL, sizeof(usbd_device_t));
  TU_LOG_INT(CFG_TUD_LOG_LEVEL, sizeof(dcd_event_t));
  TU_LOG_INT(CFG_TUD_LOG_LEVEL, sizeof(tu_fifo_t));
  TU_LOG_INT(CFG_TUD_LOG_LEVEL, sizeof(tu_edpt_stream_t));
#endif

  tu_varclr(&_usbd_dev);
  _usbd_queued_setup = 0;

  osal_spin_init(&_usbd_spin);

#if OSAL_MUTEX_REQUIRED
  // Init device mutex
  _usbd_mutex = osal_mutex_create(&_ubsd_mutexdef);
  TU_ASSERT(_usbd_mutex);
#endif

  // Init device queue & task
  _usbd_q = osal_queue_create(&_usbd_qdef);
  TU_ASSERT(_usbd_q);

  // Get application driver if available
  _app_driver = usbd_app_driver_get_cb(&_app_driver_count);
  TU_ASSERT(_app_driver_count + _builtin_driver_count <= UINT8_MAX);

  // Init class drivers
  for (uint8_t i = 0; i < TOTAL_DRIVER_COUNT; i++) {
    usbd_class_driver_t const* driver = get_driver(i);
    TU_ASSERT(driver && driver->init);
    TU_LOG_USBD("%s init\r\n", driver->name);
    driver->init();
  }

  _usbd_rhport = rhport;

  // Init device controller driver
  TU_ASSERT(dcd_init(rhport, rh_init));
  dcd_int_enable(rhport);

  return true;
}

bool tud_deinit(uint8_t rhport) {
  if (!tud_inited()) {
    return true; // skip if not initialized
  }

  TU_LOG_USBD("USBD deinit on controller %u\r\n", rhport);

  const uint8_t cfg_num = _usbd_dev.cfg_num;

  // Deinit device controller driver
  dcd_int_disable(rhport);
  dcd_disconnect(rhport);
  TU_ASSERT(dcd_deinit(rhport));

  // Deinit class drivers
  for (uint8_t i = 0; i < TOTAL_DRIVER_COUNT; i++) {
    usbd_class_driver_t const* driver = get_driver(i);
    if(driver && driver->deinit) {
      TU_LOG_USBD("%s deinit\r\n", driver->name);
      driver->deinit();
    }
  }

  tu_varclr(&_usbd_dev); // Clear device data

  // Deinit device queue & task
  osal_queue_delete(_usbd_q);
  _usbd_q = NULL;
  tu_varclr(&_usbd_after_queue);

#if OSAL_MUTEX_REQUIRED
  // TODO make sure there is no task waiting on this mutex
  osal_mutex_delete(_usbd_mutex);
  _usbd_mutex = NULL;
#endif

  osal_spin_deinit(&_usbd_spin);

  _usbd_rhport = RHPORT_INVALID;

  if (cfg_num > 0) {
    tud_umount_cb();
  }

  return true;
}

static void configuration_reset(uint8_t rhport) {
  for (uint8_t i = 0; i < TOTAL_DRIVER_COUNT; i++) {
    usbd_class_driver_t const* driver = get_driver(i);
    TU_ASSERT(driver,);
    driver->reset(rhport);
  }

  tu_varclr(&_usbd_dev);
  (void)memset(_usbd_dev.itf2drv, TUSB_INDEX_INVALID_8, sizeof(_usbd_dev.itf2drv)); // invalid mapping
  (void)memset(_usbd_dev.ep2drv, TUSB_INDEX_INVALID_8, sizeof(_usbd_dev.ep2drv));   // invalid mapping
}

static void usbd_reset(uint8_t rhport) {
  configuration_reset(rhport);
  // discard any pre-reset SETUP still counted: a stale count skips post-reset SETUPs
  _usbd_queued_setup = 0;
}

bool tud_task_event_ready(void) {
  TU_VERIFY(tud_inited()); // Skip if stack is not initialized
  return !osal_queue_empty(_usbd_q) || _usbd_after_queue.func != NULL;
}

// the call usbd_defer_func_after_queue() holds; false if none
static bool run_after_queue_call(void) {
  osal_task_func_t const func = _usbd_after_queue.func;
  if (func == NULL) {
    return false;
  }
  _usbd_after_queue.func = NULL;
  func(_usbd_after_queue.param);
  return true;
}

//--------------------------------------------------------------------+
// USBD Task
//--------------------------------------------------------------------+
/* USB Device Driver task
 * This top level thread manages all device controller event and delegates events to class-specific drivers.
 * This should be called periodically within the mainloop or rtos thread.
 *
    int main(void) {
      application_init();
      tusb_init(0, TUSB_ROLE_DEVICE);

      while(1) { // the mainloop
        application_code();
        tud_task(); // tinyusb device task
      }
    }
 */
void tud_task_ext(uint32_t timeout_ms, bool in_isr) {
  (void) in_isr; // not implemented yet

  // Skip if stack is not initialized
  if (!tud_inited()) {
    return;
  }

  // Loop until there are no more events in the queue or CFG_TUD_TASK_EVENTS_PER_RUN is reached
  for (unsigned epr = 0;; epr++) {
#if CFG_TUD_TASK_EVENTS_PER_RUN > 0
    if (epr >= CFG_TUD_TASK_EVENTS_PER_RUN) {
      TU_LOG_USBD("USBD event limit (" TU_XSTRING(CFG_TUD_TASK_EVENTS_PER_RUN) ") reached\r\n");
      break;
    }
#endif
    dcd_event_t event;
    if (!osal_queue_receive(_usbd_q, &event, (_usbd_after_queue.func != NULL) ? 0 : timeout_ms)) {
      if (!run_after_queue_call()) {
        return;
      }
      timeout_ms = 0;
      continue;
    }

#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    if (event.event_id == DCD_EVENT_SETUP_RECEIVED) {
      TU_LOG_USBD("\r\n"); // extra line for setup
    }
    TU_LOG_USBD("USBD %s ", event.event_id < DCD_EVENT_COUNT ? _usbd_event_str[event.event_id] : "CORRUPTED");
#endif

    switch (event.event_id) {
      case DCD_EVENT_BUS_RESET_START:
        TU_LOG_USBD("\r\n");
        usbd_reset(event.rhport);
        break;

      case DCD_EVENT_BUS_RESET_END:
        TU_LOG_USBD(": %s Speed\r\n", tu_str_speed[event.bus_reset.speed]);
        // TODO a DCD that reports both edges pays for two teardowns: track a per-rhport
        // "start seen" flag and skip this reset, keeping it for the single-event DCDs.
        usbd_reset(event.rhport);
        _usbd_dev.speed = event.bus_reset.speed;
        break;

      case DCD_EVENT_UNPLUGGED:
        TU_LOG_USBD("\r\n");
        usbd_reset(event.rhport);
        tud_umount_cb();
        break;

      case DCD_EVENT_SETUP_RECEIVED:
        if (_usbd_queued_setup == 0) {
          break;
        }
        _usbd_queued_setup--;
        TU_LOG_BUF(CFG_TUD_LOG_LEVEL, &event.setup_received, 8);
        if (_usbd_queued_setup != 0) {
          TU_LOG_USBD("  Skipped since there is other SETUP in queue\r\n");
          break;
        }

        // Mark as connected after receiving 1st setup packet.
        // But it is easier to set it every time instead of wasting time to check then set
        _usbd_dev.connected = 1;

        // reset ep state
        _usbd_dev.ep_status[0][TUSB_DIR_OUT] = 0;
        _usbd_dev.ep_status[0][TUSB_DIR_IN] = 0;

        // Process control request
        if (!process_setup_received(event.rhport, &event.setup_received)) {
          TU_LOG_USBD("  Stall EP0\r\n");
          // Failed -> stall both control endpoint IN and OUT
          dcd_edpt_stall(event.rhport, TU_EP0_OUT);
          dcd_edpt_stall(event.rhport, TU_EP0_IN);
        }
        break;

      case DCD_EVENT_XFER_COMPLETE: {
        // Invoke the class callback associated with the endpoint address
        uint8_t const ep_addr = event.xfer_complete.ep_addr;
        uint8_t const epnum = tu_edpt_number(ep_addr);
        uint8_t const ep_dir = tu_edpt_dir(ep_addr);

        TU_LOG_USBD("on EP %02X with %u bytes\r\n", ep_addr, (unsigned int) event.xfer_complete.len);

        usbd_class_driver_t const* driver = NULL; // EP0 has no class driver
        if (0 != epnum) {
          driver = get_driver(_usbd_dev.ep2drv[epnum][ep_dir]);
          TU_ASSERT(driver,);
        }

        // Clear busy + claimed. An OUT endpoint changed to RX_PENDING, so no other task can claim and re-arm it before
        // its buffer is consumed
        uint8_t const rx_pending = (0 != epnum && ep_dir == TUSB_DIR_OUT) ? TU_EDPT_STATE_RX_PENDING : 0u;
        _usbd_dev.ep_status[epnum][ep_dir] = (uint8_t) (
          (_usbd_dev.ep_status[epnum][ep_dir] & ~(TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED)) | rx_pending);

        if (driver == NULL) {
          // Not stalled on failure: a DCD refuses an EP0 prime when a newer setup is already
          // latched, and EP0 stalls are cleared by hardware when that setup arrives - so a stall
          // issued here lands after the auto-clear and would stall the transfer that superseded
          // this one. The pending setup re-drives EP0 by itself.
          if (!usbd_control_xfer_cb(event.rhport, ep_addr, (xfer_result_t) event.xfer_complete.result,
                                    event.xfer_complete.len)) {
            TU_LOG_USBD("  Control stage not continued\r\n");
          }
        } else {
          TU_LOG_USBD("  %s xfer callback\r\n", driver->name);
          driver->xfer_cb(event.rhport, ep_addr, (xfer_result_t) event.xfer_complete.result, event.xfer_complete.len);
        }
        break;
      }

      case DCD_EVENT_SUSPEND:
        // NOTE: When plugging/unplugging device, the D+/D- state are unstable and
        // can accidentally meet the SUSPEND condition ( Bus Idle for 3ms ), which result in a series of event
        // e.g suspend -> resume -> unplug/plug. Skip suspend/resume if not connected
        if (_usbd_dev.connected) {
          TU_LOG_USBD(": Remote Wakeup = %u\r\n", _usbd_dev.remote_wakeup_en);
          tud_suspend_cb(_usbd_dev.remote_wakeup_en);
        } else {
          TU_LOG_USBD(" Skipped\r\n");
        }
        break;

      case DCD_EVENT_RESUME:
        if (_usbd_dev.connected) {
          TU_LOG_USBD("\r\n");
          tud_resume_cb();
        } else {
          TU_LOG_USBD(" Skipped\r\n");
        }
        break;

      case USBD_EVENT_FUNC_CALL:
        TU_LOG_USBD("\r\n");
        if (event.func_call.func != NULL) {
          event.func_call.func(event.func_call.param);
        }
        break;

      case DCD_EVENT_SOF:
        if (tu_bit_test(_usbd_dev.sof_consumer, SOF_CONSUMER_USER)) {
          TU_LOG_USBD("\r\n");
          tud_sof_cb(event.sof.frame_count);
        }
      break;

      default:
        TU_BREAKPOINT();
        break;
    }

    // allow to exit tud_task() if there is no event in the next run
    timeout_ms = 0;
  }
}

//--------------------------------------------------------------------+
// Control Endpoint
//--------------------------------------------------------------------+

// Weak hook: invoked when the control transfer's status stage completes
TU_ATTR_WEAK void dcd_edpt0_status_complete(uint8_t rhport, const tusb_control_request_t* request) {
  (void) rhport;
  (void) request;
}

uint8_t* usbd_get_ctrl_buf(void) {
  return _ctrl_epbuf.buf;
}

// Endpoint used for the Status stage of a control transfer.
// Per USB 2.0 §9.3.1, when wLength == 0 the Direction bit is ignored and the Status
// stage is always IN. Otherwise the Status stage is opposite of the Data stage direction.
TU_ATTR_ALWAYS_INLINE static inline uint8_t status_stage_ep(const tusb_control_request_t* request) {
  return (request->wLength != 0 && request->bmRequestType_bit.direction) ? TU_EP0_OUT : TU_EP0_IN;
}

// Queue ZLP status transaction
TU_ATTR_ALWAYS_INLINE static inline bool status_stage_xact(uint8_t rhport, uint8_t ep_status) {
  return usbd_edpt_xfer(rhport, ep_status, NULL, 0, false);
}

// Queue a transaction in Data Stage. Each transaction has up to Endpoint0's max
// packet size. This function can also transfer a zero-length packet.
static bool data_stage_xact(uint8_t rhport) {
  usbd_control_xfer_t* const ctrl_xfer = &_usbd_dev.ctrl_xfer;
  const uint16_t xact_len = tu_min16(ctrl_xfer->data_len - ctrl_xfer->total_xferred, CFG_TUD_ENDPOINT0_BUFSIZE);
  uint8_t ep_addr = TU_EP0_OUT;

  if (ctrl_xfer->request.bmRequestType_bit.direction == TUSB_DIR_IN) {
    ep_addr = TU_EP0_IN;
    if (0u != xact_len && ctrl_xfer->buffer != _ctrl_epbuf.buf) {
      TU_VERIFY(0 == tu_memcpy_s(_ctrl_epbuf.buf, CFG_TUD_ENDPOINT0_BUFSIZE, ctrl_xfer->buffer, xact_len));
    }
  }

  return usbd_edpt_xfer(rhport, ep_addr, xact_len ? _ctrl_epbuf.buf : NULL, xact_len, false);
}

// Status phase
bool tud_control_status(uint8_t rhport, const tusb_control_request_t* request) {
  // _usbd_dev.ctrl_xfer fields are pre-initialized at process_setup_received entry
  (void) request;
  return status_stage_xact(rhport, status_stage_ep(&_usbd_dev.ctrl_xfer.request));
}

// Transmit data to/from the control endpoint. If wLength is zero, a status packet is sent instead.
bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t* request, void* buffer, uint16_t len) {
  // _usbd_dev.ctrl_xfer.request and reset fields are pre-initialized at process_setup_received entry
  (void) request;
  usbd_control_xfer_t* const ctrl_xfer = &_usbd_dev.ctrl_xfer;
  ctrl_xfer->buffer = (uint8_t*) buffer;
  ctrl_xfer->data_len = tu_min16(len, ctrl_xfer->request.wLength);

  if (ctrl_xfer->request.wLength > 0U) {
    if (ctrl_xfer->data_len > 0U) {
      TU_ASSERT(buffer);
    }
    TU_VERIFY(data_stage_xact(rhport));
  } else {
    // wLength == 0: Status stage is always IN per USB 2.0 §9.3.1
    TU_VERIFY(status_stage_xact(rhport, TU_EP0_IN));
  }

  return true;
}

// Callback when a transaction completes on the DATA stage or Status stage of EP0
static bool usbd_control_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  (void) result;
  usbd_control_xfer_t* const ctrl_xfer = &_usbd_dev.ctrl_xfer;

  // Status Stage complete: ep_addr matches the resolved Status stage endpoint
  uint8_t const ep_status = status_stage_ep(&ctrl_xfer->request);
  if (ep_addr == ep_status) {
    TU_ASSERT(0 == xferred_bytes);

    // invoke optional dcd hook if available
    dcd_edpt0_status_complete(rhport, &ctrl_xfer->request);

    if (NULL != ctrl_xfer->complete_cb) {
      ctrl_xfer->complete_cb(rhport, CONTROL_STAGE_ACK, &ctrl_xfer->request);
    }

    return true;
  }

  // Data stage progress
  if (ctrl_xfer->request.bmRequestType_bit.direction == TUSB_DIR_OUT) {
    TU_VERIFY(ctrl_xfer->buffer);
    // Clamp host overrun to remaining capacity (data_len) so memcpy can't overflow the caller buffer
    xferred_bytes = tu_min32(xferred_bytes, ctrl_xfer->data_len - ctrl_xfer->total_xferred);
    if (ctrl_xfer->buffer != _ctrl_epbuf.buf) {
      memcpy(ctrl_xfer->buffer, _ctrl_epbuf.buf, xferred_bytes);
    }
    TU_LOG_MEM(CFG_TUD_LOG_LEVEL, ctrl_xfer->buffer, xferred_bytes, 2);
  }

  ctrl_xfer->total_xferred += (uint16_t) xferred_bytes;
  ctrl_xfer->buffer += xferred_bytes;

  // Data Stage complete when wLength reached or short packet (incl. ZLP) seen
  if ((ctrl_xfer->request.wLength == ctrl_xfer->total_xferred) ||
      (xferred_bytes < CFG_TUD_ENDPOINT0_BUFSIZE)) {
    bool is_ok = true;

    if (NULL != ctrl_xfer->complete_cb) {
      // Callback can still stall control in status phase, e.g. OUT data doesn't make sense
      is_ok = ctrl_xfer->complete_cb(rhport, CONTROL_STAGE_DATA, &ctrl_xfer->request);
    }

    if (is_ok) {
      TU_VERIFY(status_stage_xact(rhport, ep_status));
    } else {
      // Stall both IN and OUT control endpoint
      dcd_edpt_stall(rhport, TU_EP0_OUT);
      dcd_edpt_stall(rhport, TU_EP0_IN);
    }
  } else {
    // More data to transfer
    TU_VERIFY(data_stage_xact(rhport));
  }

  return true;
}

//--------------------------------------------------------------------+
// Control Request Parser & Handling
//--------------------------------------------------------------------+

// Helper to invoke class driver control request handler
static bool invoke_class_control(uint8_t rhport, usbd_class_driver_t const * driver, tusb_control_request_t const * request) {
  _usbd_dev.ctrl_xfer.complete_cb = driver->control_xfer_cb;
  TU_LOG_USBD("  %s control request\r\n", driver->name);
  return driver->control_xfer_cb(rhport, CONTROL_STAGE_SETUP, request);
}

// Process a standard request to the device recipient.
static bool process_std_device_request(uint8_t rhport, tusb_control_request_t const * p_request) {
  switch (p_request->bRequest) { //-V2520
    case TUSB_REQ_SET_ADDRESS:
      // Depending on mcu, status phase could be sent either before or after changing device address,
      // or even require stack to not response with status at all
      // Therefore DCD must take full responsibility to response and include zlp status packet if needed.
      dcd_set_address(rhport, (uint8_t) p_request->wValue);
      _usbd_dev.addressed = 1;
      return true;

    case TUSB_REQ_GET_CONFIGURATION: {
      uint8_t cfg_num = _usbd_dev.cfg_num;
      tud_control_xfer(rhport, p_request, &cfg_num, 1);
      return true;
    }

    case TUSB_REQ_SET_CONFIGURATION: {
      uint8_t const cfg_num = (uint8_t) p_request->wValue;

      // Only process if new configure is different
      if (_usbd_dev.cfg_num != cfg_num) {
        if (_usbd_dev.cfg_num != 0) {
          // already configured: need to clear all endpoints and driver first
          TU_LOG_USBD("  Clear current Configuration (%u) before switching\r\n", _usbd_dev.cfg_num);

          dcd_sof_enable(rhport, false);
          dcd_edpt_close_all(rhport);

          // close all drivers and current configured state except bus speed
          const uint8_t speed = _usbd_dev.speed;
          configuration_reset(rhport);

          _usbd_dev.speed = speed; // restore speed
        }

        _usbd_dev.cfg_num = cfg_num;

        // Handle the new configuration
        if (cfg_num == 0) {
          tud_umount_cb();
        } else {
          if (!process_set_config(rhport, cfg_num)) {
            _usbd_dev.cfg_num = 0;
            TU_ASSERT(false);
          }
          tud_mount_cb();
        }
      }

      tud_control_status(rhport, p_request);
      return true;
    }

    case TUSB_REQ_GET_DESCRIPTOR:
      return process_get_descriptor(rhport, p_request);

    case TUSB_REQ_SET_FEATURE:
      switch (p_request->wValue) { //-V2520
        case TUSB_REQ_FEATURE_REMOTE_WAKEUP:
          TU_LOG_USBD("    Enable Remote Wakeup\r\n");
          // Host may enable remote wake up before suspending especially HID device
          _usbd_dev.remote_wakeup_en = 1;
          tud_control_status(rhport, p_request);
          return true;

        #if CFG_TUD_TEST_MODE
        case TUSB_REQ_FEATURE_TEST_MODE: {
          // Only handle the test mode if supported and valid
          TU_VERIFY(0 == tu_u16_low(p_request->wIndex));

          uint8_t const selector = tu_u16_high(p_request->wIndex);
          TU_VERIFY(TUSB_FEATURE_TEST_J <= selector && selector <= TUSB_FEATURE_TEST_FORCE_ENABLE);

          _usbd_dev.ctrl_xfer.complete_cb = process_test_mode_cb;
          tud_control_status(rhport, p_request);
          return true;
        }
        #endif

        // Stall unsupported feature selector
        default: return false;
      }

    case TUSB_REQ_CLEAR_FEATURE:
      // Only support remote wakeup for device feature
      TU_VERIFY(TUSB_REQ_FEATURE_REMOTE_WAKEUP == p_request->wValue);
      TU_LOG_USBD("    Disable Remote Wakeup\r\n");

      // Host may disable remote wake up after resuming
      _usbd_dev.remote_wakeup_en = 0;
      tud_control_status(rhport, p_request);
      return true;

    case TUSB_REQ_GET_STATUS: {
      // Device status bit mask
      // - Bit 0: Self Powered TODO must invoke callback to get actual status
      // - Bit 1: Remote Wakeup enabled
      return process_get_status(rhport, p_request, (uint16_t) _usbd_dev.dev_state_bm);
    }

    default:
      TU_BREAKPOINT();
      return false;
  }
}


// Reply to a standard GET_STATUS (device/interface/endpoint) with its 2-byte status word.
// GET_STATUS is Device-to-host only; reject a mis-directed (OUT) request rather than handing
// usbd the address of a stack local to write host data into after this frame has returned.
static bool process_get_status(uint8_t rhport, tusb_control_request_t const * request, uint16_t status) {
  TU_VERIFY(request->bmRequestType_bit.direction == TUSB_DIR_IN);
  return tud_control_xfer(rhport, request, &status, 2);
}

// This handles the actual request and its response.
// Returns false if unable to complete the request, causing caller to stall control endpoints.
static bool process_setup_received(uint8_t rhport, tusb_control_request_t const * p_request) {
  // Initialize control transfer state for this request. The request copy must be
  // visible to usbd_control_xfer_cb when the (asynchronous) status ZLP completes,
  // since the SETUP packet event has already gone out of scope by then.
  usbd_control_xfer_t* const ctrl_xfer = &_usbd_dev.ctrl_xfer;
  ctrl_xfer->request = *p_request;
  ctrl_xfer->buffer = NULL;
  ctrl_xfer->total_xferred = 0;
  ctrl_xfer->data_len = 0;
  ctrl_xfer->complete_cb = NULL;

  p_request = &ctrl_xfer->request; // re-direct request pointer to internal copy (modifiable for hacking)
  TU_ASSERT(p_request->bmRequestType_bit.type < TUSB_REQ_TYPE_INVALID);

  // Vendor request
  if ( p_request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR ) {
    ctrl_xfer->complete_cb = tud_vendor_control_xfer_cb;
    return tud_vendor_control_xfer_cb(rhport, CONTROL_STAGE_SETUP, p_request);
  }

#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
  if (TUSB_REQ_TYPE_STANDARD == p_request->bmRequestType_bit.type && p_request->bRequest <= TUSB_REQ_SYNCH_FRAME) {
    TU_LOG_USBD("  %s", tu_str_std_request[p_request->bRequest]);
    if (TUSB_REQ_GET_DESCRIPTOR != p_request->bRequest) {
      TU_LOG_USBD("\r\n");
    }
  }
#endif

  switch (p_request->bmRequestType_bit.recipient) { //-V2520
    //------------- Device Requests e.g in enumeration -------------//
    case TUSB_REQ_RCPT_DEVICE:
      if ( TUSB_REQ_TYPE_CLASS == p_request->bmRequestType_bit.type ) {
        uint8_t const itf = tu_u16_low(p_request->wIndex);
        TU_VERIFY(itf < TU_ARRAY_SIZE(_usbd_dev.itf2drv));

        usbd_class_driver_t const * driver = get_driver(_usbd_dev.itf2drv[itf]);
        TU_VERIFY(driver);

        // forward to class driver: "non-STD request to Interface"
        return invoke_class_control(rhport, driver, p_request);
      }

      if (TUSB_REQ_TYPE_STANDARD != p_request->bmRequestType_bit.type) {
        // Non-standard request is not supported
        TU_BREAKPOINT();
        return false;
      }

      return process_std_device_request(rhport, p_request);

    //------------- Class/Interface Specific Request -------------//
    case TUSB_REQ_RCPT_INTERFACE: {
      uint8_t itf;
      #if CFG_TUD_PRINTER
      // Printer GET_DEVICE_ID has a weird wIndex = interface (high) | alt (low)
      // attempt to interpret this as a printer request if matched
      if (TUSB_REQ_TYPE_CLASS == p_request->bmRequestType_bit.type &&
          TUSB_DIR_IN == p_request->bmRequestType_bit.direction &&
          TUSB_PRINTER_REQUEST_GET_DEVICE_ID == p_request->bRequest) {
        itf = tu_u16_high(p_request->wIndex);
        if (itf < TU_ARRAY_SIZE(_usbd_dev.itf2drv)) {
          const usbd_class_driver_t * driver = get_driver(_usbd_dev.itf2drv[itf]);
          if (driver != NULL && driver->control_xfer_cb == printerd_control_xfer_cb) {
            if (invoke_class_control(rhport, driver, p_request)) {
              return true;
            }
          }
        }
      }
      #endif
      itf = tu_u16_low(p_request->wIndex);
      TU_VERIFY(itf < TU_ARRAY_SIZE(_usbd_dev.itf2drv));

      usbd_class_driver_t const * driver = get_driver(_usbd_dev.itf2drv[itf]);
      TU_VERIFY(driver);

      // all requests to Interface (STD or Class) is forwarded to class driver.
      // notable requests are: GET HID REPORT DESCRIPTOR, SET_INTERFACE, GET_INTERFACE
      if (!invoke_class_control(rhport, driver, p_request)) {
        // For GET_INTERFACE and SET_INTERFACE, it is mandatory to respond even if the class
        // driver doesn't use alternate settings or implement this
        TU_VERIFY(TUSB_REQ_TYPE_STANDARD == p_request->bmRequestType_bit.type);

        // Clear complete callback if driver set since it can also stall the request.
        ctrl_xfer->complete_cb = NULL;

        switch (p_request->bRequest) { //-V2520
          case TUSB_REQ_GET_INTERFACE: {
            uint8_t alternate = 0;
            tud_control_xfer(rhport, p_request, &alternate, 1);
            break;
          }

          case TUSB_REQ_SET_INTERFACE:
            // A class that implements altsettings handles SET_INTERFACE itself and returns true,
            // so reaching here means the class does not — where only alt 0 is valid. Any non-zero
            // alt (unimplemented, or rejected as invalid by the class) is a Request Error (stall).
            TU_VERIFY(tu_u16_low(p_request->wValue) == 0);
            tud_control_status(rhport, p_request);
            break;

          case TUSB_REQ_GET_STATUS:
            // USB 2.0 9.4.5: interface GET_STATUS returns 2 reserved (zero) bytes
            TU_VERIFY(process_get_status(rhport, p_request, 0x0000));
            break;

          default: return false;
        }
      }
      break;
    }

    //------------- Endpoint Request -------------//
    case TUSB_REQ_RCPT_ENDPOINT: {
      uint8_t const ep_addr = tu_u16_low(p_request->wIndex);
      uint8_t const ep_num  = tu_edpt_number(ep_addr);
      uint8_t const ep_dir  = tu_edpt_dir(ep_addr);

      TU_ASSERT(ep_num < TU_ARRAY_SIZE(_usbd_dev.ep2drv) );
      usbd_class_driver_t const * driver = get_driver(_usbd_dev.ep2drv[ep_num][ep_dir]);

      if (TUSB_REQ_TYPE_STANDARD != p_request->bmRequestType_bit.type) {
        // Forward class request to its driver
        TU_VERIFY(driver);
        return invoke_class_control(rhport, driver, p_request);
      } else {
        // Handle STD request to endpoint
        switch (p_request->bRequest) { //-V2520
          case TUSB_REQ_GET_STATUS:
            // USB 2.0 9.4.5: endpoint GET_STATUS bit 0 = Halt
            TU_VERIFY(process_get_status(rhport, p_request, usbd_edpt_stalled(rhport, ep_addr) ? 0x0001u : 0x0000u));
            break;

          case TUSB_REQ_CLEAR_FEATURE:
          case TUSB_REQ_SET_FEATURE: {
            // ENDPOINT_HALT is the only endpoint feature; it exists only on a non-control endpoint
            // that an interface actually owns. Any other selector, the control endpoint (EP0 has no
            // Halt feature, USB 2.0 9.4.9), or an endpoint no driver owns is a Request Error (stall).
            TU_VERIFY(TUSB_REQ_FEATURE_EDPT_HALT == p_request->wValue);
            TU_VERIFY(ep_num != 0);
            TU_VERIFY(driver != NULL);

            if ( TUSB_REQ_CLEAR_FEATURE == p_request->bRequest ) {
              usbd_edpt_clear_stall(rhport, ep_addr);
            } else {
              usbd_edpt_stall(rhport, ep_addr);
            }

            // Some classes such as USBTMC need to clear/re-init their buffer on CLEAR_FEATURE.
            // Clear complete callback if driver set since it can also stall the request.
            (void) invoke_class_control(rhport, driver, p_request);
            ctrl_xfer->complete_cb = NULL;

            // STD request must always be ACKed; skip ZLP status if driver already did that.
            if (!(_usbd_dev.ep_status[0][TUSB_DIR_IN] & TU_EDPT_STATE_BUSY)) {
              tud_control_status(rhport, p_request);
            }
          }
          break;

          // Unknown/Unsupported request
          default:
            TU_BREAKPOINT();
            return false;
        }
      }
      break;
    }

    // Unknown recipient
    default:
      TU_BREAKPOINT();
      return false;
  }

  return true;
}

// Process Set Configure Request
// This function parse configuration descriptor & open drivers accordingly
static bool process_set_config(uint8_t rhport, uint8_t cfg_num) {
  // index is cfg_num-1
  const tusb_desc_configuration_t *desc_cfg =
    (const tusb_desc_configuration_t *)tud_descriptor_configuration_cb(cfg_num - 1);
  TU_ASSERT(desc_cfg != NULL && desc_cfg->bDescriptorType == TUSB_DESC_CONFIGURATION);

  // Parse configuration descriptor
  _usbd_dev.self_powered = (desc_cfg->bmAttributes & TUSB_DESC_CONFIG_ATT_SELF_POWERED) ? 1u : 0u;

  // Parse interface descriptor
  const uint8_t *p_desc   = ((const uint8_t *)desc_cfg) + sizeof(tusb_desc_configuration_t);
  const uint8_t *desc_end = ((const uint8_t *)desc_cfg) + tu_le16toh(desc_cfg->wTotalLength);

  while (tu_desc_in_bounds(p_desc, desc_end)) {
    // Class will always start with Interface Association (if any) and then Interface descriptor
    if (TUSB_DESC_INTERFACE_ASSOCIATION == tu_desc_type(p_desc)) {
      p_desc = tu_desc_next(p_desc); // next to Interface
      continue;
    }

    TU_ASSERT(TUSB_DESC_INTERFACE == tu_desc_type(p_desc));
    const tusb_desc_interface_t *desc_itf = (const tusb_desc_interface_t *)p_desc;

    // Find driver for this interface
    const uint16_t remaining_len = (uint16_t)(desc_end - p_desc);
    uint8_t        drv_id;
    for (drv_id = 0; drv_id < TOTAL_DRIVER_COUNT; drv_id++) {
      const usbd_class_driver_t *driver = get_driver(drv_id);
      TU_ASSERT(driver);
      const uint16_t drv_len = driver->open(rhport, desc_itf, remaining_len);

      if ((sizeof(tusb_desc_interface_t) <= drv_len) && (drv_len <= remaining_len)) {
        // Open successfully
        TU_LOG_USBD("  %s opened\r\n", driver->name);

        // bind found driver to all interfaces and endpoint within drv_len
        TU_ASSERT(tu_bind_driver_to_ep_itf(drv_id, _usbd_dev.ep2drv, _usbd_dev.itf2drv, CFG_TUD_INTERFACE_MAX,
                                           CFG_TUD_ENDPPOINT_MAX, p_desc, drv_len));

        p_desc += drv_len; // next Interface
        break; // exit driver find loop
      }
    }

    // Failed if there is no supported drivers
    TU_ASSERT(drv_id < TOTAL_DRIVER_COUNT);
  }

  return true;
}

// return descriptor's buffer and update desc_len
static bool process_get_descriptor(uint8_t rhport, tusb_control_request_t const * p_request) {
  tusb_desc_type_t const desc_type = (tusb_desc_type_t) tu_u16_high(p_request->wValue);
  uint8_t const desc_index = tu_u16_low( p_request->wValue );

  switch(desc_type) { //-V2520
    case TUSB_DESC_DEVICE: {
      TU_LOG_USBD(" Device\r\n");

      void *desc_device = (void *)(uintptr_t)tud_descriptor_device_cb();
      TU_ASSERT(desc_device);

      // Only response with exactly 1 Packet if: not addressed and host requested more data than device descriptor has.
      // This only happens with the very first get device descriptor and EP0 size = 8 or 16.
      if ((CFG_TUD_ENDPOINT0_SIZE < sizeof(tusb_desc_device_t)) && !_usbd_dev.addressed &&
          p_request->wLength > sizeof(tusb_desc_device_t)) {
        // Hack here: we modify the request length to prevent usbd_control response with zlp
        // since we are responding with 1 packet & less data than wLength.
        ((tusb_control_request_t *)(uintptr_t)p_request)->wLength = CFG_TUD_ENDPOINT0_SIZE;
        return tud_control_xfer(rhport, p_request, desc_device, CFG_TUD_ENDPOINT0_SIZE);
      } else {
        return tud_control_xfer(rhport, p_request, desc_device, sizeof(tusb_desc_device_t));
      }
    }
    // break; // unreachable

    case TUSB_DESC_BOS: {
      TU_LOG_USBD(" BOS\r\n");

      // requested by host if USB > 2.0 ( i.e 2.1 or 3.x )
      uintptr_t desc_bos = (uintptr_t) tud_descriptor_bos_cb();
      TU_VERIFY(desc_bos != 0);

      // Use offsetof to avoid pointer to the odd/misaligned address
      uint16_t const total_len = tu_le16toh( tu_unaligned_read16((const void*) (desc_bos + offsetof(tusb_desc_bos_t, wTotalLength))) );

      return tud_control_xfer(rhport, p_request, (void*) desc_bos, total_len);
    }
    // break; // unreachable

    case TUSB_DESC_CONFIGURATION:
    case TUSB_DESC_OTHER_SPEED_CONFIG: {
      uintptr_t desc_config;

      if ( desc_type == TUSB_DESC_CONFIGURATION ) {
        TU_LOG_USBD(" Configuration[%u]\r\n", desc_index);
        desc_config = (uintptr_t) tud_descriptor_configuration_cb(desc_index);
        TU_ASSERT(desc_config != 0);
      }else {
        // Host only request this after getting Device Qualifier descriptor
        TU_LOG_USBD(" Other Speed Configuration\r\n");
        desc_config = (uintptr_t) tud_descriptor_other_speed_configuration_cb(desc_index);
        TU_VERIFY(desc_config != 0);
      }

      // Use offsetof to avoid pointer to the odd/misaligned address
      uint16_t const total_len = tu_le16toh( tu_unaligned_read16((const void*) (desc_config + offsetof(tusb_desc_configuration_t, wTotalLength))) );

      return tud_control_xfer(rhport, p_request, (void*) desc_config, total_len);
    }
    // break; // unreachable

    case TUSB_DESC_STRING: {
      TU_LOG_USBD(" String[%u]\r\n", desc_index);

      // String Descriptor always uses the desc set from user
      uint8_t const* desc_str = (uint8_t const*) tud_descriptor_string_cb(desc_index, p_request->wIndex);
      TU_VERIFY(desc_str);

      // first byte of descriptor is its size
      return tud_control_xfer(rhport, p_request, (void*) (uintptr_t) desc_str, tu_desc_len(desc_str));
    }
    // break; // unreachable

    case TUSB_DESC_DEVICE_QUALIFIER: {
      TU_LOG_USBD(" Device Qualifier\r\n");
      uint8_t const* desc_qualifier = tud_descriptor_device_qualifier_cb();
      TU_VERIFY(desc_qualifier);
      return tud_control_xfer(rhport, p_request, (void*) (uintptr_t) desc_qualifier, tu_desc_len(desc_qualifier));
    }
    // break; // unreachable

    default: return false;
  }
}

//--------------------------------------------------------------------+
// DCD Event Handler
//--------------------------------------------------------------------+
TU_ATTR_FAST_FUNC void dcd_event_handler(dcd_event_t const* event, bool in_isr) {
  bool send = false;
  switch (event->event_id) {
    case DCD_EVENT_UNPLUGGED:
      _usbd_dev.connected = 0;
      _usbd_dev.addressed = 0;
      _usbd_dev.cfg_num = 0;
      _usbd_dev.suspended = 0;
      send = true;
      break;

    case DCD_EVENT_SUSPEND:
      // NOTE: When plugging/unplugging device, the D+/D- state are unstable and
      // can accidentally meet the SUSPEND condition ( Bus Idle for 3ms ).
      // In addition, some MCUs such as SAMD or boards that haven no VBUS detection cannot distinguish
      // suspended vs disconnected. We will skip handling SUSPEND/RESUME event if not currently connected
      if (_usbd_dev.connected) {
        _usbd_dev.suspended = 1;
        send = true;
      }
      break;

    case DCD_EVENT_RESUME:
      // skip event if not connected (especially required for SAMD)
      if (_usbd_dev.connected) {
        _usbd_dev.suspended = 0;
        send = true;
      }
      break;

    case DCD_EVENT_SOF:
      // SOF driver handler in ISR context
      for (uint8_t i = 0; i < TOTAL_DRIVER_COUNT; i++) {
        usbd_class_driver_t const* driver = get_driver(i);
        if (driver && driver->sof) {
          driver->sof(event->rhport, event->sof.frame_count);
        }
      }

      // Some MCUs after running dcd_remote_wakeup() does not have way to detect the end of remote wakeup
      // which last 1-15 ms. DCD can use SOF as a clear indicator that bus is back to operational
      if (_usbd_dev.suspended) {
        _usbd_dev.suspended = 0;

        dcd_event_t const event_resume = {.rhport = event->rhport, .event_id = DCD_EVENT_RESUME};
        queue_event(&event_resume, in_isr);
      }

      if (tu_bit_test(_usbd_dev.sof_consumer, SOF_CONSUMER_USER)) {
        dcd_event_t const event_sof = {.rhport = event->rhport, .event_id = DCD_EVENT_SOF, .sof.frame_count = event->sof.frame_count};
        queue_event(&event_sof, in_isr);
      }
      break;

    case DCD_EVENT_SETUP_RECEIVED:
      _usbd_queued_setup++;
      send = true;
      break;

    case DCD_EVENT_XFER_COMPLETE: {
      // Invoke the class callback associated with the endpoint address
      uint8_t const ep_addr = event->xfer_complete.ep_addr;
      uint8_t const epnum = tu_edpt_number(ep_addr);
      uint8_t const ep_dir = tu_edpt_dir(ep_addr);

      send = true;
      if(epnum > 0) {
        usbd_class_driver_t const* driver = get_driver(_usbd_dev.ep2drv[epnum][ep_dir]);

        if (driver && driver->xfer_isr) {
          // Clear busy + claimed
          _usbd_dev.ep_status[epnum][ep_dir] &= (uint8_t) ~(TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED);

          send = !driver->xfer_isr(event->rhport, ep_addr, (xfer_result_t) event->xfer_complete.result, event->xfer_complete.len);

          // xfer_isr() is deferred to xfer_cb(), revert busy/claimed status
          if (send) {
            // set busy + claimed
            _usbd_dev.ep_status[epnum][ep_dir] |= (TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED);
          }
        }
      }
      break;
    }

    default:
      send = true;
      break;
  }

  if (send && !queue_event(event, in_isr)) {
    // event dropped by a full queue: undo state that would otherwise wedge permanently
    if (event->event_id == DCD_EVENT_SETUP_RECEIVED) {
      // undo the increment, else every later SETUP is skipped as "other SETUP in queue"
      // and EP0 is deaf until re-init
      _usbd_queued_setup--;
    } else if (event->event_id == DCD_EVENT_XFER_COMPLETE) {
      // clear busy + claimed, else the endpoint can never be claimed or re-armed again
      uint8_t const epnum = tu_edpt_number(event->xfer_complete.ep_addr);
      uint8_t const ep_dir = tu_edpt_dir(event->xfer_complete.ep_addr);
      _usbd_dev.ep_status[epnum][ep_dir] &= (uint8_t) ~(TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED);
    }
  }
}

//--------------------------------------------------------------------+
// USBD API For Class Driver
//--------------------------------------------------------------------+

void usbd_int_set(bool enabled) {
  if (enabled) {
    dcd_int_enable(_usbd_rhport);
  } else {
    dcd_int_disable(_usbd_rhport);
  }
}

void usbd_spin_lock(bool in_isr) {
  osal_spin_lock(&_usbd_spin, in_isr);
}
void usbd_spin_unlock(bool in_isr) {
  osal_spin_unlock(&_usbd_spin, in_isr);
}

// Parse consecutive endpoint descriptors (IN & OUT)
bool usbd_open_edpt_pair(uint8_t rhport, const uint8_t *p_desc, uint8_t ep_count, uint8_t xfer_type, uint8_t *ep_out,
                         uint8_t *ep_in) {
  for (int i = 0; i < ep_count; i++) {
    const tusb_desc_endpoint_t *desc_ep = (const tusb_desc_endpoint_t *)p_desc;

    TU_ASSERT(TUSB_DESC_ENDPOINT == desc_ep->bDescriptorType && xfer_type == desc_ep->bmAttributes.xfer);
    TU_ASSERT(usbd_edpt_open(rhport, desc_ep));

    if (tu_edpt_dir(desc_ep->bEndpointAddress) == TUSB_DIR_IN) {
      (*ep_in) = desc_ep->bEndpointAddress;
    } else {
      (*ep_out) = desc_ep->bEndpointAddress;
    }

    p_desc = tu_desc_next(p_desc);
  }

  return true;
}

// Helper to defer an isr function
void usbd_defer_func(osal_task_func_t func, void* param, bool in_isr) {
  dcd_event_t event = {
      .rhport   = 0,
      .event_id = USBD_EVENT_FUNC_CALL,
  };
  event.func_call.func  = func;
  event.func_call.param = param;

  queue_event(&event, in_isr);
}

bool usbd_defer_func_after_queue(osal_task_func_t func, void* param) {
  if (_usbd_after_queue.func != NULL) {
    return _usbd_after_queue.func == func && _usbd_after_queue.param == param;
  }
  _usbd_after_queue.func = func;
  _usbd_after_queue.param = param;
  return true;
}

//--------------------------------------------------------------------+
// USBD Endpoint API
//--------------------------------------------------------------------+

bool usbd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const* desc_ep) {
  rhport = _usbd_rhport;

  TU_ASSERT(tu_edpt_number(desc_ep->bEndpointAddress) < CFG_TUD_ENDPPOINT_MAX);
  TU_ASSERT(tu_edpt_validate(desc_ep, (tusb_speed_t)_usbd_dev.speed));

  return dcd_edpt_open(rhport, desc_ep);
}

bool usbd_edpt_claim(uint8_t rhport, uint8_t ep_addr) {
  (void) rhport;

  // TODO add this check later, also make sure we don't starve an out endpoint while suspending
  // TU_VERIFY(tud_ready());

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);
  return tu_edpt_claim(&_usbd_dev.ep_status[epnum][dir], _usbd_mutex);
}

bool usbd_edpt_release(uint8_t rhport, uint8_t ep_addr) {
  (void) rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);
  return tu_edpt_release(&_usbd_dev.ep_status[epnum][dir], _usbd_mutex);
}

void usbd_edpt_rx_consume(uint8_t rhport, uint8_t ep_addr) {
  (void) rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);
  volatile uint8_t *ep_state = &_usbd_dev.ep_status[epnum][dir];

  TU_VERIFY((*ep_state & TU_EDPT_STATE_RX_PENDING) != 0,);
  (void) osal_mutex_lock(_usbd_mutex, OSAL_TIMEOUT_WAIT_FOREVER);
  if ((*ep_state & TU_EDPT_STATE_RX_PENDING) != 0) {
    *ep_state &= (uint8_t) ~TU_EDPT_STATE_RX_PENDING;
  }
  (void) osal_mutex_unlock(_usbd_mutex);
}

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes, bool is_isr) {
  rhport = _usbd_rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  // TODO skip ready() check for now since enumeration also use this API
  // TU_VERIFY(tud_ready());

  TU_LOG_USBD("  Queue EP %02X with %u bytes ...\r\n", ep_addr, total_bytes);
#if CFG_TUD_LOG_LEVEL >= 3
  if(dir == TUSB_DIR_IN) {
    TU_LOG_MEM(CFG_TUD_LOG_LEVEL, buffer, total_bytes, 2);
  }
#endif

  // Attempt to transfer on a busy endpoint, sound like an race condition !
  TU_ASSERT((_usbd_dev.ep_status[epnum][dir] & TU_EDPT_STATE_BUSY) == 0);

  // Set busy first since the actual transfer can be complete before dcd_edpt_xfer()
  // could return and USBD task can preempt and clear the busy
  _usbd_dev.ep_status[epnum][dir] =
    (uint8_t) ((_usbd_dev.ep_status[epnum][dir] & ~TU_EDPT_STATE_RX_PENDING) | TU_EDPT_STATE_BUSY);

  if (dcd_edpt_xfer(rhport, ep_addr, buffer, total_bytes, is_isr)) {
    return true;
  } else {
    // Driver refused the transfer, mark endpoint as ready to allow next transfer. This is a
    // recoverable condition (e.g. a new setup superseding a control response), not a bug, so
    // do not break into the debugger - TU_BREAKPOINT() halts the CPU whenever a probe is
    // attached, which on a test rig is always.
    _usbd_dev.ep_status[epnum][dir] &= (uint8_t) ~(TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED);
    TU_LOG_USBD("FAILED\r\n");
    return false;
  }
}

// The number of bytes has to be given explicitly to allow more flexible control of how many
// bytes should be written and second to keep the return value free to give back a boolean
// success message. If total_bytes is too big, the FIFO will copy only what is available
// into the USB buffer!
bool usbd_edpt_xfer_fifo(uint8_t rhport, uint8_t ep_addr, tu_fifo_t* ff, uint16_t total_bytes, bool is_isr) {
  #if CFG_TUD_EDPT_DEDICATED_HWFIFO
  rhport = _usbd_rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  TU_LOG_USBD("  Queue FIFO EP %02X with %u bytes ... ", ep_addr, total_bytes);

  // Attempt to transfer on a busy endpoint, sound like a race condition !
  TU_ASSERT((_usbd_dev.ep_status[epnum][dir] & TU_EDPT_STATE_BUSY) == 0);

  // Set busy first since the actual transfer can be complete before dcd_edpt_xfer() could return
  // and usbd task can preempt and clear the busy
  _usbd_dev.ep_status[epnum][dir] =
    (uint8_t) ((_usbd_dev.ep_status[epnum][dir] & ~TU_EDPT_STATE_RX_PENDING) | TU_EDPT_STATE_BUSY);

  if (dcd_edpt_xfer_fifo(rhport, ep_addr, ff, total_bytes, is_isr)) {
    TU_LOG_USBD("OK\r\n");
    return true;
  } else {
    // DCD error, mark endpoint as ready to allow next transfer
    _usbd_dev.ep_status[epnum][dir] &= (uint8_t) ~(TU_EDPT_STATE_BUSY | TU_EDPT_STATE_CLAIMED);
    TU_LOG_USBD("failed\r\n");
    TU_BREAKPOINT();
    return false;
  }
  #else
  (void)rhport;
  (void)ep_addr;
  (void)ff;
  (void)total_bytes;
  (void)is_isr;
  return false;
  #endif
}

bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr) {
  (void) rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  return (_usbd_dev.ep_status[epnum][dir] & TU_EDPT_STATE_BUSY) != 0;
}

void usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr) {
  rhport = _usbd_rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  // only stalled if currently cleared
  TU_LOG_USBD("    Stall EP %02X\r\n", ep_addr);
  dcd_edpt_stall(rhport, ep_addr);
  _usbd_dev.ep_status[epnum][dir] |= (TU_EDPT_STATE_STALLED | TU_EDPT_STATE_BUSY);
}

void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) {
  rhport = _usbd_rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  TU_LOG_USBD("    Clear Stall EP %02X\r\n", ep_addr);
  const bool was_stalled = (_usbd_dev.ep_status[epnum][dir] & TU_EDPT_STATE_STALLED) != 0;
  dcd_edpt_clear_stall(rhport, ep_addr);
  // Clear STALLED|BUSY unconditionally (long-standing behavior; some classes, e.g. audio's
  // set-interface, call this on a non-stalled endpoint solely to drop a leftover BUSY bit).
  // Only release the CLAIMED ownership bit when the endpoint was actually stalled: the stall
  // aborts the in-flight transfer in the dcd with no completion event to release the claim, so
  // clearing it here prevents starvation. On a non-stalled clear (e.g. a data-toggle reset) a
  // transfer may still be legitimately claimed by another task, so keep CLAIMED to preserve the
  // claim->xfer mutual exclusion.
  uint8_t clear_mask = TU_EDPT_STATE_STALLED | TU_EDPT_STATE_BUSY;
  if (was_stalled) {
    clear_mask |= TU_EDPT_STATE_CLAIMED;
  }
  _usbd_dev.ep_status[epnum][dir] &= (uint8_t) ~clear_mask;
}

// Not valid for EP0, where dcd_edpt_stall() means "stall the control transfer" rather than "cancel".
// TODO replace this stall/clear-stall stand-in with a dcd_edpt_abort_xfer() returning bool, like
// hcd_edpt_abort_xfer(): the stall is wire-visible and the clear resets the toggle.
void usbd_edpt_abort(uint8_t rhport, uint8_t ep_addr) {
  usbd_edpt_stall(rhport, ep_addr);
  usbd_edpt_clear_stall(rhport, ep_addr);
}

bool usbd_edpt_stalled(uint8_t rhport, uint8_t ep_addr) {
  (void) rhport;

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  return (_usbd_dev.ep_status[epnum][dir] & TU_EDPT_STATE_STALLED) != 0;
}

/**
 * usbd_edpt_close will disable an endpoint.
 * In progress transfers on this EP may be delivered after this call.
 */
void usbd_edpt_close(uint8_t rhport, uint8_t ep_addr) {
#ifdef TUP_DCD_EDPT_ISO_ALLOC
  (void) rhport; (void) ep_addr;
  // ISO alloc/activate Should be used instead
#else
  rhport = _usbd_rhport;

  TU_LOG_USBD("  CLOSING Endpoint: 0x%02X\r\n", ep_addr);

  uint8_t const epnum = tu_edpt_number(ep_addr);
  uint8_t const dir = tu_edpt_dir(ep_addr);

  dcd_edpt_close(rhport, ep_addr);
  _usbd_dev.ep_status[epnum][dir] = 0;
#endif

  return;
}

void usbd_sof_enable(uint8_t rhport, sof_consumer_t consumer, bool en) {
  rhport = _usbd_rhport;

  uint8_t consumer_old = _usbd_dev.sof_consumer;
  // Keep track how many class instances need the SOF interrupt
  if (en) {
    _usbd_dev.sof_consumer |= (uint8_t)(1 << consumer);
  } else {
    _usbd_dev.sof_consumer &= (uint8_t)(~(1 << consumer));
  }

  // Test logically unequal
  if(!_usbd_dev.sof_consumer != !consumer_old) {
    dcd_sof_enable(rhport, _usbd_dev.sof_consumer);
  }
}

bool usbd_edpt_iso_alloc(uint8_t rhport, uint8_t ep_addr, uint16_t largest_packet_size) {
#ifdef TUP_DCD_EDPT_ISO_ALLOC
  rhport = _usbd_rhport;

  TU_ASSERT(tu_edpt_number(ep_addr) < CFG_TUD_ENDPPOINT_MAX);
  return dcd_edpt_iso_alloc(rhport, ep_addr, largest_packet_size);
#else
  (void) rhport; (void) ep_addr; (void) largest_packet_size;
  return false;
#endif
}

bool usbd_edpt_iso_activate(uint8_t rhport, tusb_desc_endpoint_t const* desc_ep) {
#ifdef TUP_DCD_EDPT_ISO_ALLOC
  rhport = _usbd_rhport;

  uint8_t const epnum = tu_edpt_number(desc_ep->bEndpointAddress);
  uint8_t const dir = tu_edpt_dir(desc_ep->bEndpointAddress);

  TU_ASSERT(epnum < CFG_TUD_ENDPPOINT_MAX);
  TU_ASSERT(tu_edpt_validate(desc_ep, (tusb_speed_t)_usbd_dev.speed));

  _usbd_dev.ep_status[epnum][dir] = 0;
  return dcd_edpt_iso_activate(rhport, desc_ep);
#else
  (void) rhport; (void) desc_ep;
  return false;
#endif
}

#endif
