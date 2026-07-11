cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(crosspoint-reader)

idf_build_get_property(project_elf EXECUTABLE)
target_link_options(${project_elf} PRIVATE "-Wl,--wrap=bootloader_common_check_efuse_blk_validity")
