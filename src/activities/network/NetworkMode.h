#pragma once

// The File Transfer menu's rows (NetworkModeSelectionActivity), in a header of their own so the
// activity manager and the boot path can name a row without pulling in the activity.
//
// USB_SERIAL and USB_DRIVE are mutually exclusive in the menu: a board that can
// present itself as a USB mass-storage device has no use for the serial
// file-transfer protocol, so it offers the drive instead (see the menu table
// in NetworkModeSelectionActivity.cpp). Both enumerators exist in every build; only the listing
// differs.
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_SERIAL, USB_DRIVE, OPDS_BROWSER };
