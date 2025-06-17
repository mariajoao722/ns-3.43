// ie-dot11s-prune.h
#ifndef IE_DOT11S_PRUNE_H
#define IE_DOT11S_PRUNE_H

#include "ns3/mac48-address.h"
#include "ns3/wifi-information-element.h"

#include <vector>

namespace ns3
{
namespace dot11s
{
  
/**
 * \ingroup dot11s
 * \brief PRUNE information element for mesh path management
 *
 * This class represents a custom PRUNE IE used to inform nodes to stop forwarding
 * packets to certain destinations. This implementation is compatible with the
 * NS-3 WifiInformationElement framework.
 */
class IePrune : public WifiInformationElement
{
  public:
    IePrune();
    ~IePrune() override;
    /**
     * \brief Add a (destination, reasonCode) unit to the PRUNE message
     * \param destination MAC address to prune
     * \param reasonCode A 32-bit reason code
     */
    void AddPruneUnit(Mac48Address destination, uint32_t reasonCode);
    /**
     * \brief Get all destination/reason pairs contained in this PRUNE element
     * \returns vector of (Mac48Address, reasonCode)
     */
    std::vector<std::pair<Mac48Address, uint32_t>> GetPruneUnits() const;

    // Inherited from WifiInformationElement
    WifiInformationElementId ElementId() const override;
    void SerializeInformationField(Buffer::Iterator i) const override;
    uint16_t DeserializeInformationField(Buffer::Iterator start, uint16_t length) override;
    uint16_t GetInformationFieldSize() const override;
    void Print(std::ostream& os) const override;

  private:
    // List of pruned destinations and their reason codes
    std::vector<std::pair<Mac48Address, uint32_t>> m_pruneUnits;
};
} // namespace dot11s
} // namespace ns3

#endif // IE_DOT11S_PRUNE_H