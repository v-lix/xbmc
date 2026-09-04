/*
 *  Copyright (C) 2026-present Team CoreELEC (https://coreelec.org)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>
#include <vector>

/*!
 * \brief The OPCM wire format: how Kodi describes decoded PCM to the renderer.
 *
 * The renderer does not decode. It is fed by a bridge plugin, and the bridge
 * this describes - libpcm_bridge.so - exists because Kodi already decoded the
 * stream and therefore already knows what every channel is. The header says so
 * directly: one label byte per channel, in the order the samples are
 * interleaved.
 *
 * That is the whole reason this is not a WAV header. A WAV header carries a
 * channel count and, at best, a WAVE_FORMAT_EXTENSIBLE dwChannelMask - and a
 * mask is a lossy round trip in both directions. Its bit order is the
 * interleave order, so the host must re-sort its layout to match; positions
 * with no mask bit cannot be expressed; and one position the renderer does not
 * name (TOP_BACK_CENTER) has nowhere to go at all. Six channels through a plain
 * WAV header are labelled the same whether the source used side or back
 * surrounds, which is audibly wrong and silent about it.
 *
 * \verbatim
 * offset  size  field
 * 0       4     magic "OPCM"
 * 4       2     version, currently 1                    (u16 LE)
 * 6       2     channel count N, 1..=OMNI_PCM_MAX_CHANNELS  (u16 LE)
 * 8       4     sample rate in Hz                       (u32 LE)
 * 12      1     sample encoding, see OmniphonyPcmEncoding
 * 13      1     reserved, must be zero
 * 14      N     one channel label per channel, interleave order
 * \endverbatim
 *
 * No length field follows the header: the stream runs until the bridge is
 * reset, which returns it to awaiting a fresh header. That is also how a format
 * change is announced - reset, then describe the new geometry.
 *
 * These values are the bridge's ABI. They mirror `RChannelLabel` in
 * omniphony-renderer/bridge_api and the parser in pcm_bridge/src/header.rs;
 * neither side may be changed alone.
 */

//! \brief How the samples following the header are encoded.
enum class OmniphonyPcmEncoding : uint8_t
{
  /*!
   * \brief Signed 24-bit scaled into an int32, the renderer's own convention.
   *
   * Passed through untouched by the bridge. Converting here rather than there
   * is what a host does when it wants to decide for itself what happens above
   * 0 dBFS, while it still has the float to decide with.
   */
  I32Scaled24 = 0,
  //! \brief 32-bit float, nominally -1.0 to 1.0. Clamped by the bridge.
  Float32 = 1,
};

//! \brief Bytes before the label array.
constexpr size_t OMNI_PCM_FIXED_HEADER_LEN = 14;

//! \brief The only header version the bridge understands.
constexpr uint16_t OMNI_PCM_VERSION = 1;

//! \brief Upper bound on the channel count, matching the bridge's own.
constexpr int OMNI_PCM_MAX_CHANNELS = 64;

/*!
 * \brief Label every channel of an FFmpeg native channel-layout mask.
 *
 * The bits of a native mask are the interleave order - FFmpeg orders channels
 * by ascending bit - so walking the mask upwards and emitting one label per set
 * bit produces exactly the order the samples arrive in. Nothing is sorted and
 * nothing is inferred from the count.
 *
 * \param mask      the layout mask, valid only for AV_CHANNEL_ORDER_NATIVE
 * \param channels  the decoded channel count, which must equal the number of
 *                  set bits - a disagreement means the mask does not describe
 *                  this stream and nothing can be said about its channels
 * \param labels    filled with \p channels label bytes on success, untouched
 *                  otherwise
 * \return false if the count disagrees, the count is out of range, or any set
 *         bit names a position the renderer cannot place. Every failure is a
 *         reason to decode the stream the ordinary way instead: a channel put
 *         in the wrong place is worse than one not spatialised at all.
 */
bool OmniphonyPcmChannelLabels(uint64_t mask, int channels, std::vector<uint8_t>& labels);

/*!
 * \brief Build the header that introduces a stream of \p labels channels.
 *
 * Returns an empty vector if \p labels is empty or longer than the bridge
 * accepts, or if \p sampleRate is zero - each of which the bridge would refuse.
 */
std::vector<uint8_t> OmniphonyPcmHeader(const std::vector<uint8_t>& labels,
                                        uint32_t sampleRate,
                                        OmniphonyPcmEncoding encoding);
