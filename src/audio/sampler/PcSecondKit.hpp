// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcSecondKit.hpp
 * @brief A song's second kit on the simulator (core ISecondKit).
 *
 * The desktop counterpart of the board's IdfSecondKit in platform-idf's
 * SamplerEnginePort.cpp: a `crosspad::SamplerEngine` of its own beside the
 * pads' one, on a mixer channel of its own ("Kit 2"), played by note (the
 * sequencer's sink for the second kit track, SecondKitPadLogic for the
 * fingers) and never through the pads' logic.
 *
 * A worker reads the kit named by hold() off the virtual SD card (the same
 * readers and pad builders as the pads' engine, PcKitLoad) while a streaming
 * thread of its own services the engine's rings, the board's audio_bg pass.
 * Nothing here is shared with the pads' kit, so the two never wait on each
 * other. Unlike the board there is no arena to divide and no voice budget to
 * split between the kits.
 *
 * The engine is released only once the audio thread is out of it: the node
 * counts the render passes and notes inside the engine, and a release closes
 * the gate and waits for the count to reach zero before a single sample is
 * given back.
 */

namespace crosspad { class IAudioNode; class ISecondKit; }

namespace crosspad_pc {

/// The "Kit 2" channel's generator. Silent until a kit is held.
crosspad::IAudioNode* second_kit_node();

/// The ISecondKit to publish as PlatformServices::secondKit. Its worker and
/// streaming thread start with the first hold().
crosspad::ISecondKit* second_kit();

} // namespace crosspad_pc
