// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Units and capacity dimensions.
//
// A capacity dimension without its unit is not a quantity. Dimension and unit
// are therefore bound together in DimensionKey, and the unit is validated
// against the dimension at construction. Two capacities expressed in different
// units are never added, subtracted or compared: they are separate keys, and a
// cross-unit join is reported as UNIT_SEPARATED rather than silently coerced.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "summon/capacity_reconciliation/status.hpp"

namespace summon::capacity_reconciliation {

/// Exact, integral, non-scaled units. Every unit is an exact count or an exact
/// integer multiple of a base unit (milli-watts, milli-amperes, grams, bytes).
/// No unit is a floating point quantity.
enum class Unit : std::uint8_t {
  None = 0,

  RackUnit,           ///< Whole rack units of vertical space (U).
  SquareMilliMetre,   ///< Footprint area in mm^2.
  MilliWatt,          ///< Electrical power in milliwatts.
  MilliWattThermal,   ///< Heat rejection in milliwatts thermal.
  Gram,               ///< Mass in grams.
  Port,               ///< Countable physical/logical ports.
  Slot,               ///< Countable equipment slots.
  Accelerator,        ///< Countable accelerator positions.
  Byte,               ///< Storage / memory capacity in bytes.
  BitPerSecond,       ///< Network bandwidth in bits per second.
  MilliAmpere,        ///< Current in milliamperes.
  MilliVolt,          ///< Voltage in millivolts.
  Millisecond,        ///< Time budget in milliseconds.
  Count,              ///< Generic dimensionless count.
};

/// What kind of facility capacity a key describes. The dimension determines
/// which units are permissible.
enum class CapacityDimension : std::uint8_t {
  None = 0,

  Space,        ///< Physical space: rack units or footprint.
  Power,        ///< Provisioned electrical capacity.
  Cooling,      ///< Heat rejection capacity.
  Mass,         ///< Structural load capacity.
  Ports,        ///< Network port capacity.
  Slots,        ///< Equipment slot capacity.
  Accelerators, ///< Accelerator position capacity.
  Memory,       ///< Memory capacity.
  Storage,      ///< Storage capacity.
  Bandwidth,    ///< Network bandwidth capacity.
  Current,      ///< Amperage capacity (feeds / breakers).
  Voltage,      ///< Voltage capacity.
  Time,         ///< Maintenance / reservation time budget.
  Count,        ///< Generic countable capacity.
};

/// Stable lowercase token for a unit. Used in canonical serialization, JSON and
/// CLI output. Never localised.
[[nodiscard]] const char* to_string(Unit unit) noexcept;
[[nodiscard]] std::optional<Unit> unit_from_string(std::string_view token) noexcept;

/// Stable lowercase token for a dimension.
[[nodiscard]] const char* to_string(CapacityDimension dimension) noexcept;
[[nodiscard]] std::optional<CapacityDimension> dimension_from_string(
    std::string_view token) noexcept;

/// The units that may be used to express a given dimension. A dimension may
/// admit several units; they remain distinct keys.
[[nodiscard]] bool unit_admissible_for(CapacityDimension dimension, Unit unit) noexcept;

/// A dimension paired with the exact unit it is expressed in.
class DimensionKey {
 public:
  DimensionKey() = default;

  /// Constructs a key. Fails with INVALID_ARGUMENT when the unit is not
  /// admissible for the dimension and with UNSUPPORTED when the dimension is
  /// outside the runtime's boundary.
  [[nodiscard]] static Result<DimensionKey> create(CapacityDimension dimension, Unit unit);

  [[nodiscard]] CapacityDimension dimension() const noexcept { return dimension_; }
  [[nodiscard]] Unit unit() const noexcept { return unit_; }

  /// Canonical token form: `<dimension>:<unit>`.
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] static std::optional<DimensionKey> parse(std::string_view text) noexcept;

  friend bool operator==(const DimensionKey& a, const DimensionKey& b) noexcept {
    return a.dimension_ == b.dimension_ && a.unit_ == b.unit_;
  }
  friend bool operator!=(const DimensionKey& a, const DimensionKey& b) noexcept {
    return !(a == b);
  }
  /// Total order used for canonical ordering. Orders by dimension then unit.
  friend bool operator<(const DimensionKey& a, const DimensionKey& b) noexcept {
    if (a.dimension_ != b.dimension_) {
      return static_cast<std::uint8_t>(a.dimension_) < static_cast<std::uint8_t>(b.dimension_);
    }
    return static_cast<std::uint8_t>(a.unit_) < static_cast<std::uint8_t>(b.unit_);
  }

 private:
  CapacityDimension dimension_ = CapacityDimension::None;
  Unit unit_ = Unit::None;
};

/// True when both keys describe the same dimension but in different units. Such
/// keys are never combined; the engine reports UNIT_SEPARATED for the cell.
[[nodiscard]] inline bool same_dimension_different_unit(const DimensionKey& a,
                                                        const DimensionKey& b) noexcept {
  return a.dimension() == b.dimension() && a.unit() != b.unit();
}

}  // namespace summon::capacity_reconciliation
