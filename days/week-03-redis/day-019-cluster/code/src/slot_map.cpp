#include "slot_map.hpp"

#include <algorithm>
#include <stdexcept>

namespace archdesign {

SlotMap::SlotMap(std::size_t slot_count) : owners_(slot_count) {}

void SlotMap::assign(Slot slot, NodeId owner) {
    if (slot >= owners_.size()) throw std::out_of_range("slot out of range");
    owners_[slot] = std::move(owner);
}

void SlotMap::assign_range(Slot begin, Slot end, NodeId owner) {
    if (begin > end || end >= owners_.size()) throw std::out_of_range("slot range out of range");
    for (std::size_t slot = begin; slot <= end; ++slot) {
        owners_[slot] = owner;
    }
}

const NodeId& SlotMap::owner(Slot slot) const {
    if (slot >= owners_.size()) throw std::out_of_range("slot out of range");
    return owners_[slot];
}

bool SlotMap::assigned(Slot slot) const { return !owner(slot).empty(); }

std::size_t SlotMap::assigned_count() const {
    return static_cast<std::size_t>(
        std::count_if(owners_.begin(), owners_.end(), [](const NodeId& id) { return !id.empty(); }));
}

bool SlotMap::all_assigned() const { return assigned_count() == owners_.size(); }

std::size_t SlotMap::slot_count() const { return owners_.size(); }

}  // namespace archdesign
