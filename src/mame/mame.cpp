// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    mame.cpp

    Specific (per target) constants

****************************************************************************/

#include "emu.h"
#include "main.h"

#define APPNAME                 "Theseus-CDi"
#define APPNAME_LOWER           "theseus-cdi"
#define CONFIGNAME              "theseus-cdi"
#define COPYRIGHT               "Copyright MAMEdev and OM3GAZX\nhttps://github.com/OM3GAZX/cdi-neo"
#define COPYRIGHT_INFO          "Copyright MAMEdev and OM3GAZX"

const char * emulator_info::get_appname() { return APPNAME;}
const char * emulator_info::get_appname_lower() { return APPNAME_LOWER;}
const char * emulator_info::get_configname() { return CONFIGNAME;}
const char * emulator_info::get_copyright() { return COPYRIGHT;}
const char * emulator_info::get_copyright_info() { return COPYRIGHT_INFO;}
