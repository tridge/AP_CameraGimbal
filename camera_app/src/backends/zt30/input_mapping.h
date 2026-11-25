#pragma once
#include "mi/buffer_abi.h"
#include <vector>
#include <cstdio>
namespace ca_zt30 {
struct InputMapping { uint64_t first,last; bool shared_zero; };
// Bounded proc parser, never reads memory at the recorded addresses.
bool read_input_mappings(FILE*,std::vector<InputMapping>&);
// Require a newly introduced exact shared writable SDK mapping and consistent
// virtual/physical plane offsets. Physical slots are checked, never remapped.
// Other threads map and unmap between the two snapshots, so the SDK may
// reuse an address that held another mapping in the first.
bool new_input_mapping(const std::vector<InputMapping>& before,
                       const std::vector<InputMapping>& leased,
                       const InputBufferInfo&);
}
