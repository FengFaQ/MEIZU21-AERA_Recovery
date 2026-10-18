/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <string>
#include "update_engine/update_metadata.pb.h"
namespace aeraui::payload {
// Empty means supported. Includes extent coverage, bounds, hashes and codec checks.
std::string ExtractionError(const chromeos_update_engine::PartitionUpdate &partition,
                            uint64_t block_size, uint64_t data_size);
}
