// ie-dot11s-prune.cc
#include "ie-dot11s-prune.h"

#include "ns3/address-utils.h"
#include "ns3/log.h"

namespace ns3
{
namespace dot11s
{

NS_LOG_COMPONENT_DEFINE("IePrune");

IePrune::IePrune()
{
}

IePrune::~IePrune()
{
}

WifiInformationElementId
IePrune::ElementId() const
{
    return IE_PRUNE;
}

void
IePrune::Print(std::ostream& os) const
{
    os << "PRUNE IE: ";
    for (const auto& unit : m_pruneUnits)
    {
        os << "[Dest: " << unit.first << ", Reason: " << unit.second << "] ";
    }
}

void
IePrune::AddPruneUnit(Mac48Address destination, uint32_t reasonCode)
{
    m_pruneUnits.emplace_back(destination, reasonCode);
}

std::vector<std::pair<Mac48Address, uint32_t>>
IePrune::GetPruneUnits() const
{
    return m_pruneUnits;
}

void
IePrune::SerializeInformationField(Buffer::Iterator i) const
{
    for (const auto& unit : m_pruneUnits)
    {
        WriteTo(i, unit.first);
        i.WriteHtonU32(unit.second);
    }
}

uint16_t
IePrune::DeserializeInformationField(Buffer::Iterator start, uint16_t length)
{
    Buffer::Iterator i = start;
    m_pruneUnits.clear();
    while (i.GetDistanceFrom(start) < length)
    {
        Mac48Address address;
        ReadFrom(i, address);
        uint32_t reason = i.ReadNtohU32();
        m_pruneUnits.emplace_back(address, reason);
    }
    return length;
}

uint16_t
IePrune::GetInformationFieldSize() const
{
    return m_pruneUnits.size() * (6 + 4); // 6 bytes for MAC, 4 for reasonCode
}

} // namespace dot11s

} // namespace ns3