/*
 * The crosspad-core / crosspad-gui this simulator was written against.
 * Major mismatch is #error, minor/patch drift a #warning — update the triple
 * after building and running against the new submodule, not to silence it.
 * Same gate as platform-idf's main/include/crosspad_deps.h.
 */
#pragma once

#include <crosspad/CrosspadCoreVersion.hpp>
#include <crosspad-gui/CrosspadGuiVersion.hpp>

#define CROSSPAD_PC_REQUIRES_CORE_MAJOR 1
#define CROSSPAD_PC_REQUIRES_CORE_MINOR 0
#define CROSSPAD_PC_REQUIRES_CORE_PATCH 3

#define CROSSPAD_PC_REQUIRES_GUI_MAJOR 1
#define CROSSPAD_PC_REQUIRES_GUI_MINOR 3
#define CROSSPAD_PC_REQUIRES_GUI_PATCH 3

#define CROSSPAD_REQ_HAVE_MAJOR CROSSPAD_CORE_VERSION_MAJOR
#define CROSSPAD_REQ_HAVE_MINOR CROSSPAD_CORE_VERSION_MINOR
#define CROSSPAD_REQ_HAVE_PATCH CROSSPAD_CORE_VERSION_PATCH
#define CROSSPAD_REQ_WANT_MAJOR CROSSPAD_PC_REQUIRES_CORE_MAJOR
#define CROSSPAD_REQ_WANT_MINOR CROSSPAD_PC_REQUIRES_CORE_MINOR
#define CROSSPAD_REQ_WANT_PATCH CROSSPAD_PC_REQUIRES_CORE_PATCH
#include <crosspad/RequireVersion.hpp>

#define CROSSPAD_REQ_HAVE_MAJOR CROSSPAD_GUI_VERSION_MAJOR
#define CROSSPAD_REQ_HAVE_MINOR CROSSPAD_GUI_VERSION_MINOR
#define CROSSPAD_REQ_HAVE_PATCH CROSSPAD_GUI_VERSION_PATCH
#define CROSSPAD_REQ_WANT_MAJOR CROSSPAD_PC_REQUIRES_GUI_MAJOR
#define CROSSPAD_REQ_WANT_MINOR CROSSPAD_PC_REQUIRES_GUI_MINOR
#define CROSSPAD_REQ_WANT_PATCH CROSSPAD_PC_REQUIRES_GUI_PATCH
#include <crosspad/RequireVersion.hpp>
