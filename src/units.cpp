// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "summon/capacity_reconciliation/units.hpp"

#include <array>

namespace summon::capacity_reconciliation {
namespace {

struct UnitToken {
  Unit unit;
  const char* token;
};

constexpr std::array<UnitToken, 15> kUnitTokens{{
    {Unit::None, "none"},
    {Unit::RackUnit, "rack_unit"},
    {Unit::SquareMilliMetre, "square_millimetre"},
    {Unit::MilliWatt, "milliwatt"},
    {Unit::MilliWattThermal, "milliwatt_thermal"},
    {Unit::Gram, "gram"},
    {Unit::Port, "port"},
    {Unit::Slot, "slot"},
    {Unit::Accelerator, "accelerator"},
    {Unit::Byte, "byte"},
    {Unit::BitPerSecond, "bit_per_second"},
    {Unit::MilliAmpere, "milliampere"},
    {Unit::MilliVolt, "millivolt"},
    {Unit::Millisecond, "millisecond"},
    {Unit::Count, "count"},
}};

struct DimensionToken {
  CapacityDimension dimension;
  const char* token;
};

constexpr std::array<DimensionToken, 15> kDimensionTokens{{
    {CapacityDimension::None, "none"},
    {CapacityDimension::Space, "space"},
    {CapacityDimension::Power, "power"},
    {CapacityDimension::Cooling, "cooling"},
    {CapacityDimension::Mass, "mass"},
    {CapacityDimension::Ports, "ports"},
    {CapacityDimension::Slots, "slots"},
    {CapacityDimension::Accelerators, "accelerators"},
    {CapacityDimension::Memory, "memory"},
    {CapacityDimension::Storage, "storage"},
    {CapacityDimension::Bandwidth, "bandwidth"},
    {CapacityDimension::Current, "current"},
    {CapacityDimension::Voltage, "voltage"},
    {CapacityDimension::Time, "time"},
    {CapacityDimension::Count, "count"},
}};

}  // namespace

const char* to_string(Unit unit) noexcept {
  for (const UnitToken& entry : kUnitTokens) {
    if (entry.unit == unit) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<Unit> unit_from_string(std::string_view token) noexcept {
  for (const UnitToken& entry : kUnitTokens) {
    if (token == entry.token) {
      return entry.unit;
    }
  }
  return std::nullopt;
}

const char* to_string(CapacityDimension dimension) noexcept {
  for (const DimensionToken& entry : kDimensionTokens) {
    if (entry.dimension == dimension) {
      return entry.token;
    }
  }
  return "unrecognised";
}

std::optional<CapacityDimension> dimension_from_string(std::string_view token) noexcept {
  for (const DimensionToken& entry : kDimensionTokens) {
    if (token == entry.token) {
      return entry.dimension;
    }
  }
  return std::nullopt;
}

bool unit_admissible_for(CapacityDimension dimension, Unit unit) noexcept {
  switch (dimension) {
    case CapacityDimension::Space:
      return unit == Unit::RackUnit || unit == Unit::SquareMilliMetre;
    case CapacityDimension::Power:
      return unit == Unit::MilliWatt;
    case CapacityDimension::Cooling:
      return unit == Unit::MilliWattThermal || unit == Unit::MilliWatt;
    case CapacityDimension::Mass:
      return unit == Unit::Gram;
    case CapacityDimension::Ports:
      return unit == Unit::Port;
    case CapacityDimension::Slots:
      return unit == Unit::Slot;
    case CapacityDimension::Accelerators:
      return unit == Unit::Accelerator;
    case CapacityDimension::Memory:
      return unit == Unit::Byte;
    case CapacityDimension::Storage:
      return unit == Unit::Byte;
    case CapacityDimension::Bandwidth:
      return unit == Unit::BitPerSecond;
    case CapacityDimension::Current:
      return unit == Unit::MilliAmpere;
    case CapacityDimension::Voltage:
      return unit == Unit::MilliVolt;
    case CapacityDimension::Time:
      return unit == Unit::Millisecond;
    case CapacityDimension::Count:
      return unit == Unit::Count;
    case CapacityDimension::None:
      return false;
  }
  return false;
}

Result<DimensionKey> DimensionKey::create(CapacityDimension dimension, Unit unit) {
  if (dimension == CapacityDimension::None) {
    return make_error(ErrorCode::InvalidArgument,
                      "a dimension key requires a concrete capacity dimension", "dimension");
  }
  if (unit == Unit::None) {
    return make_error(ErrorCode::InvalidArgument,
                      "a dimension key requires a concrete unit: an unqualified capacity has no "
                      "meaning",
                      "unit");
  }
  if (!unit_admissible_for(dimension, unit)) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string("unit '") + summon::capacity_reconciliation::to_string(unit) +
                          "' is not admissible for dimension '" +
                          summon::capacity_reconciliation::to_string(dimension) + "'",
                      "unit_admissibility");
  }
  DimensionKey key;
  key.dimension_ = dimension;
  key.unit_ = unit;
  return key;
}

std::string DimensionKey::to_string() const {
  std::string out;
  out += summon::capacity_reconciliation::to_string(dimension_);
  out += ':';
  out += summon::capacity_reconciliation::to_string(unit_);
  return out;
}

std::optional<DimensionKey> DimensionKey::parse(std::string_view text) noexcept {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  auto dimension = dimension_from_string(text.substr(0, separator));
  auto unit = unit_from_string(text.substr(separator + 1));
  if (!dimension.has_value() || !unit.has_value()) {
    return std::nullopt;
  }
  auto key = DimensionKey::create(*dimension, *unit);
  if (!key.ok()) {
    return std::nullopt;
  }
  return key.value();
}

}  // namespace summon::capacity_reconciliation
