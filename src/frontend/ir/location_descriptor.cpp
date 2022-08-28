/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "frontend/ir/location_descriptor.h"

namespace Dynarmic::IR {

std::string ToString(const LocationDescriptor& descriptor) {
    return fmt::format("{{{:016x}}}", descriptor.Value());
}

} // namespace Dynarmic::IR
