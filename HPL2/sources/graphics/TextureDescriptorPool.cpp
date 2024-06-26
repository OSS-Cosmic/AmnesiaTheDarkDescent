#include "graphics/TextureDescriptorPool.h"
#include "graphics/ForgeHandles.h"

namespace hpl {

    TextureDescriptorPool::TextureDescriptorPool(uint32_t ringsize, uint32_t poolSize):
        m_pool(poolSize) {
        m_ring.resize(ringsize);
        m_slot.resize(poolSize);
    }
    void TextureDescriptorPool::reset(DescriptorHandler handler) {
        m_actionHandler = handler;
        m_index = (m_index + 1) % m_ring.size();
        for(auto& ringEntry: m_ring[m_index]) {
            handler(ringEntry.m_action, ringEntry.slot, m_slot[ringEntry.slot]);
            switch(ringEntry.m_action) {
                case Action::UpdateSlot: {
                    if((++ringEntry.m_count) < m_ring.size()) {
                        m_ring[(m_index + 1) % m_ring.size()].push_back(ringEntry);
                    }
                    break;
                }
                case Action::RemoveSlot: {
                    if((++ringEntry.m_count) < m_ring.size()) {
                        m_ring[(m_index + 1) % m_ring.size()].push_back(ringEntry);
                    } else {
                        m_pool.returnId(ringEntry.slot);
                        m_slot[ringEntry.slot] = SharedTexture();
                    }
                    break;
                }
            }

        }
        m_ring[m_index].clear();

    }
    uint32_t TextureDescriptorPool::request(SharedTexture& texture) {
        uint32_t slot = m_pool.requestId();
        m_slot[slot] = texture;
        m_actionHandler(Action::UpdateSlot, slot, m_slot[slot]);
        if(m_ring.size() > 1) {
            m_ring[(m_index + 1) % m_ring.size()].push_back(RingEntry {Action::UpdateSlot, slot, 1});
        }
        return slot;
    }
    void TextureDescriptorPool::dispose(uint32_t slot) {
        m_ring[(m_index + 1) % m_ring.size()].push_back(RingEntry {Action::RemoveSlot, slot, 0});
    }
} // namespace hpl
