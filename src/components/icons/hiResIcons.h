#pragma once

#include <GfxRenderer.h>

#if CROSSPOINT_HIRES_UI_ICONS

// Hi-res artwork for the UI icons, for GfxRenderer::setHiResIcons: each icon
// drawn from the same Lucide SVG at three times its 1x size, which is twice
// its drawn size on a board with a 1.5x UI scale. An icon with no row here is
// drawn by enlarging its 1x bitmap.
//
// Regenerate the *2x.h headers with the command in each .manifest, using
// three times its --sizes (24,32 becomes 72,96). The *_96.h headers come from
// scripts/convert_icon.py with a 96 px PNG of the SVG on a white background.
// Cover and Bookmark use the filled SVGs kept in this folder (book-open-filled,
// bookmark-filled), at both 32 and 96 px.

#include "blocks.h"
#include "blocks_96.h"
#include "book.h"
#include "book_96.h"
#include "bookmark.h"
#include "bookmark_96.h"
#include "cover.h"
#include "cover_96.h"
#include "folder.h"
#include "folder_96.h"
#include "headerIcons.h"
#include "headerIcons2x.h"
#include "hotspot.h"
#include "hotspot_96.h"
#include "library.h"
#include "library_96.h"
#include "listIcons.h"
#include "listIcons2x.h"
#include "opdsIcons.h"
#include "opdsIcons2x.h"
#include "readerToolbarIcons.h"
#include "readerToolbarIcons2x.h"
#include "recent.h"
#include "recent_96.h"
#include "settings2.h"
#include "settings2_96.h"
#include "transfer.h"
#include "transfer_96.h"
#include "wifi.h"
#include "wifi_96.h"

static const GfxRenderer::HiResIcon kHiResIcons[] = {
    {BlocksIcon, Blocks_96Icon, 32, 96},
    {BookIcon, Book_96Icon, 32, 96},
    {BookmarkIcon, Bookmark_96Icon, 32, 96},
    {CoverIcon, Cover_96Icon, 32, 96},
    {FolderIcon, Folder_96Icon, 32, 96},
    {HotspotIcon, Hotspot_96Icon, 32, 96},
    {LibraryIcon, Library_96Icon, 32, 96},
    {RecentIcon, Recent_96Icon, 32, 96},
    {Settings2Icon, Settings2_96Icon, 32, 96},
    {TransferIcon, Transfer_96Icon, 32, 96},
    {WifiIcon, Wifi_96Icon, 32, 96},
    {icon_folder_24_bits, icon_folder_72_bits, 24, 72},
    {icon_folder_32_bits, icon_folder_96_bits, 32, 96},
    {icon_file_text_24_bits, icon_file_text_72_bits, 24, 72},
    {icon_file_text_32_bits, icon_file_text_96_bits, 32, 96},
    {icon_image_24_bits, icon_image_72_bits, 24, 72},
    {icon_image_32_bits, icon_image_96_bits, 32, 96},
    {icon_book_24_bits, icon_book_72_bits, 24, 72},
    {icon_book_32_bits, icon_book_96_bits, 32, 96},
    {icon_file_24_bits, icon_file_72_bits, 24, 72},
    {icon_file_32_bits, icon_file_96_bits, 32, 96},
    {icon_wifi_24_bits, icon_wifi_72_bits, 24, 72},
    {icon_wifi_32_bits, icon_wifi_96_bits, 32, 96},
    {icon_library_24_bits, icon_library_72_bits, 24, 72},
    {icon_library_32_bits, icon_library_96_bits, 32, 96},
    {icon_blocks_24_bits, icon_blocks_72_bits, 24, 72},
    {icon_blocks_32_bits, icon_blocks_96_bits, 32, 96},
    {icon_radio_tower_24_bits, icon_radio_tower_72_bits, 24, 72},
    {icon_radio_tower_32_bits, icon_radio_tower_96_bits, 32, 96},
    {icon_sun_24_bits, icon_sun_72_bits, 24, 72},
    {icon_sun_32_bits, icon_sun_96_bits, 32, 96},
    {icon_moon_24_bits, icon_moon_72_bits, 24, 72},
    {icon_moon_32_bits, icon_moon_96_bits, 32, 96},
    {icon_upload_24_bits, icon_upload_72_bits, 24, 72},
    {icon_upload_32_bits, icon_upload_96_bits, 32, 96},
    {icon_download_24_bits, icon_download_72_bits, 24, 72},
    {icon_download_32_bits, icon_download_96_bits, 32, 96},
    {icon_bookmark_24_bits, icon_bookmark_72_bits, 24, 72},
    {icon_bookmark_32_bits, icon_bookmark_96_bits, 32, 96},
    {icon_usb_24_bits, icon_usb_72_bits, 24, 72},
    {icon_usb_32_bits, icon_usb_96_bits, 32, 96},
    {icon_refresh_cw_24_bits, icon_refresh_cw_72_bits, 24, 72},
    {icon_refresh_cw_32_bits, icon_refresh_cw_96_bits, 32, 96},
    {icon_checkbox_off_24_bits, icon_checkbox_off_72_bits, 24, 72},
    {icon_checkbox_off_32_bits, icon_checkbox_off_96_bits, 32, 96},
    {icon_settings_24_bits, icon_settings_72_bits, 24, 72},
    {icon_settings_32_bits, icon_settings_96_bits, 32, 96},
    {icon_smartphone_24_bits, icon_smartphone_72_bits, 24, 72},
    {icon_smartphone_32_bits, icon_smartphone_96_bits, 32, 96},
    {icon_book_open_24_bits, icon_book_open_72_bits, 24, 72},
    {icon_book_open_32_bits, icon_book_open_96_bits, 32, 96},
    {icon_pointer_24_bits, icon_pointer_72_bits, 24, 72},
    {icon_pointer_32_bits, icon_pointer_96_bits, 32, 96},
    {icon_trash_2_24_bits, icon_trash_2_72_bits, 24, 72},
    {icon_trash_2_32_bits, icon_trash_2_96_bits, 32, 96},
    {icon_bluetooth_24_bits, icon_bluetooth_72_bits, 24, 72},
    {icon_bluetooth_32_bits, icon_bluetooth_96_bits, 32, 96},
    {icon_bluetooth_off_24_bits, icon_bluetooth_off_72_bits, 24, 72},
    {icon_bluetooth_off_32_bits, icon_bluetooth_off_96_bits, 32, 96},
    {icon_page_first_24_bits, icon_page_first_72_bits, 24, 72},
    {icon_page_first_32_bits, icon_page_first_96_bits, 32, 96},
    {icon_page_prev_24_bits, icon_page_prev_72_bits, 24, 72},
    {icon_page_prev_32_bits, icon_page_prev_96_bits, 32, 96},
    {icon_page_next_24_bits, icon_page_next_72_bits, 24, 72},
    {icon_page_next_32_bits, icon_page_next_96_bits, 32, 96},
    {icon_page_last_24_bits, icon_page_last_72_bits, 24, 72},
    {icon_page_last_32_bits, icon_page_last_96_bits, 32, 96},
    {icon_header_back_32_bits, icon_header_back_96_bits, 32, 96},
    {icon_reader_contents_24_bits, icon_reader_contents_72_bits, 24, 72},
    {icon_reader_text_24_bits, icon_reader_text_72_bits, 24, 72},
    {icon_reader_more_24_bits, icon_reader_more_72_bits, 24, 72},
    {icon_reader_back_24_bits, icon_reader_back_72_bits, 24, 72},
    {icon_reader_next_24_bits, icon_reader_next_72_bits, 24, 72},
};

#endif  // CROSSPOINT_HIRES_UI_ICONS
