#include "NextHopRouter.h"

NextHopRouter::NextHopRouter() {}

PendingPacket::PendingPacket(meshtastic_MeshPacket *p, uint8_t numRetransmissions)
{
    packet = p;
    this->numRetransmissions = numRetransmissions - 1; // We subtract one, because we assume the user just did the first send
}

/**
 * Send a packet
 */
ErrorCode NextHopRouter::send(meshtastic_MeshPacket *p)
{
    // Add any messages _we_ send to the seen message list (so we will ignore all retransmissions we see)
    p->relay_node = nodeDB->getLastByteOfNodeNum(getNodeNum()); // First set the relayer to us
    wasSeenRecently(p);                                         // FIXME, move this to a sniffSent method

    p->next_hop = getNextHop(p->to, p->relay_node); // set the next hop
    LOG_DEBUG("Setting next hop for packet with dest %x to %x", p->to, p->next_hop);

    // If it's from us, ReliableRouter already handles retransmissions. If a next hop is set and hop limit is not 0, start
    // retransmissions
    if (!isFromUs(p) && p->next_hop != NO_NEXT_HOP_PREFERENCE && p->hop_limit > 0)
        startRetransmission(packetPool.allocCopy(*p)); // start retransmission for relayed packet

    return Router::send(p);
}

bool NextHopRouter::shouldFilterReceived(const meshtastic_MeshPacket *p)
{
    if (wasSeenRecently(p)) { // Note: this will return false for a fallback to flooding
        printPacket("Already seen, try stop re-Tx and cancel sending", p);
        rxDupe++;
        stopRetransmission(p->from, p->id);
        if (config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER &&
            config.device.role != meshtastic_Config_DeviceConfig_Role_REPEATER) {
            // cancel rebroadcast of this message *if* there was already one, unless we're a router/repeater!
            if (Router::cancelSending(p->from, p->id))
                txRelayCanceled++;
        }
        return true;
    }

    return Router::shouldFilterReceived(p);
}

void NextHopRouter::sniffReceived(const meshtastic_MeshPacket *p, const meshtastic_Routing *c)
{
    NodeNum ourNodeNum = getNodeNum();
    bool isAckorReply = (p->which_payload_variant == meshtastic_MeshPacket_decoded_tag) && (p->decoded.request_id != 0);
    if (isAckorReply) {
        // Update next-hop for the original transmitter of this successful transmission to the relay node, but ONLY if "from" is
        // not 0 (means implicit ACK) and original packet was also relayed by this node, or we sent it directly to the destination
        if (p->from != 0 && p->relay_node) {
            uint8_t ourRelayID = nodeDB->getLastByteOfNodeNum(ourNodeNum);
            meshtastic_NodeInfoLite *origTx = nodeDB->getMeshNode(p->from);
            if (origTx) {
                // Either relayer of ACK was also a relayer of the packet, or we were the relayer and the ACK came directly from
                // the destination
                if (wasRelayer(p->relay_node, p->decoded.request_id, p->to) ||
                    (wasRelayer(ourRelayID, p->decoded.request_id, p->to) &&
                     p->relay_node == nodeDB->getLastByteOfNodeNum(p->from))) {
                    if (origTx->next_hop != p->relay_node) {
                        LOG_DEBUG("Update next hop of 0x%x to 0x%x based on ACK/reply", p->from, p->relay_node);
                        origTx->next_hop = p->relay_node;
                    }
                }
            }
            if (!isToUs(p)) {
                Router::cancelSending(p->to, p->decoded.request_id); // cancel rebroadcast for this DM
                stopRetransmission(p->from, p->decoded.request_id);  // stop retransmission for this packet
            }
        }
    }

    if (isRebroadcaster()) {
        if (!isToUs(p) && !isFromUs(p)) {
            if (p->next_hop == NO_NEXT_HOP_PREFERENCE || p->next_hop == nodeDB->getLastByteOfNodeNum(ourNodeNum)) {
                meshtastic_MeshPacket *tosend = packetPool.allocCopy(*p); // keep a copy because we will be sending it
                LOG_INFO("Relaying received message coming from %x", p->relay_node);

                tosend->hop_limit--; // bump down the hop count
                NextHopRouter::send(tosend);
            } // else don't relay
        }
    } else {
        LOG_DEBUG("Not rebroadcasting: Role = CLIENT_MUTE or Rebroadcast Mode = NONE");
    }
    // handle the packet as normal
    Router::sniffReceived(p, c);
}

/**
 * Get the next hop for a destination, given the relay node
 * @return the node number of the next hop, 0 if no preference (fallback to FloodingRouter)
 */
uint8_t NextHopRouter::getNextHop(NodeNum to, uint8_t relay_node)
{
    meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(to);
    if (node) {
        // We are careful not to return the relay node as the next hop
        if (node->next_hop && node->next_hop != relay_node) {
            LOG_DEBUG("Next hop for 0x%x is 0x%x", to, node->next_hop);
            return node->next_hop;
        } else {
            if (node->next_hop)
                LOG_WARN("Next hop for 0x%x is 0x%x, same as relayer; set no pref", to, node->next_hop);
        }
    }
    return NO_NEXT_HOP_PREFERENCE;
}

PendingPacket *NextHopRouter::findPendingPacket(GlobalPacketId key)
{
    auto old = pending.find(key); // If we have an old record, someone messed up because id got reused
    if (old != pending.end()) {
        return &old->second;
    } else
        return NULL;
}
/**
 * Stop any retransmissions we are doing of the specified node/packet ID pair
 */
bool NextHopRouter::stopRetransmission(NodeNum from, PacketId id)
{
    auto key = GlobalPacketId(from, id);
    return stopRetransmission(key);
}

bool NextHopRouter::stopRetransmission(GlobalPacketId key)
{
    auto old = findPendingPacket(key);
    if (old) {
        auto p = old->packet;
        /* Only when we already transmitted a packet via LoRa, we will cancel the packet in the Tx queue
          to avoid canceling a transmission if it was ACKed super fast via MQTT */
        if (old->numRetransmissions < NUM_RETRANSMISSIONS - 1) {
            // remove the 'original' (identified by originator and packet->id) from the txqueue and free it
            cancelSending(getFrom(p), p->id);
            // now free the pooled copy for retransmission too
            packetPool.release(p);
        }
        auto numErased = pending.erase(key);
        assert(numErased == 1);
        return true;
    } else
        return false;
}

/**
 * Add p to the list of packets to retransmit occasionally.  We will free it once we stop retransmitting.
 */
PendingPacket *NextHopRouter::startRetransmission(meshtastic_MeshPacket *p)
{
    auto id = GlobalPacketId(p);
    auto rec = PendingPacket(p, this->NUM_RETRANSMISSIONS);

    stopRetransmission(getFrom(p), p->id);

    setNextTx(&rec);
    pending[id] = rec;

    return &pending[id];
}

/**
 * Do any retransmissions that are scheduled (FIXME - for the time being called from loop)
 */
int32_t NextHopRouter::doRetransmissions()
{
    uint32_t now = millis();
    int32_t d = INT32_MAX;

    // FIXME, we should use a better datastructure rather than walking through this map.
    // for(auto el: pending) {
    for (auto it = pending.begin(), nextIt = it; it != pending.end(); it = nextIt) {
        ++nextIt; // we use this odd pattern because we might be deleting it...
        auto &p = it->second;

        bool stillValid = true; // assume we'll keep this record around

        // FIXME, handle 51 day rolloever here!!!
        if (p.nextTxMsec <= now) {
            if (p.numRetransmissions == 0) {
                if (isFromUs(p.packet)) {
                    LOG_DEBUG("Reliable send failed, returning a nak for fr=0x%x,to=0x%x,id=0x%x", p.packet->from, p.packet->to,
                              p.packet->id);
                    sendAckNak(meshtastic_Routing_Error_MAX_RETRANSMIT, getFrom(p.packet), p.packet->id, p.packet->channel);
                }
                // Note: we don't stop retransmission here, instead the Nak packet gets processed in sniffReceived
                stopRetransmission(it->first);
                stillValid = false; // just deleted it
            } else {
                LOG_DEBUG("Sending retransmission fr=0x%x,to=0x%x,id=0x%x, tries left=%d", p.packet->from, p.packet->to,
                          p.packet->id, p.numRetransmissions);

                if (!isBroadcast(p.packet->to)) {
                    if (p.numRetransmissions == 1) {
                        // Last retransmission, reset next_hop (fallback to FloodingRouter)
                        p.packet->next_hop = NO_NEXT_HOP_PREFERENCE;
                        // Also reset it in the nodeDB
                        meshtastic_NodeInfoLite *sentTo = nodeDB->getMeshNode(p.packet->to);
                        if (sentTo) {
                            LOG_WARN("Resetting next hop for packet with dest 0x%x\n", p.packet->to);
                            sentTo->next_hop = NO_NEXT_HOP_PREFERENCE;
                        }
                        FloodingRouter::send(packetPool.allocCopy(*p.packet));
                    } else {
                        NextHopRouter::send(packetPool.allocCopy(*p.packet));
                    }
                } else {
                    // Note: we call the superclass version because we don't want to have our version of send() add a new
                    // retransmission record
                    FloodingRouter::send(packetPool.allocCopy(*p.packet));
                }

                // Queue again
                --p.numRetransmissions;
                setNextTx(&p);
            }
        }

        if (stillValid) {
            // Update our desired sleep delay
            int32_t t = p.nextTxMsec - now;

            d = min(t, d);
        }
    }

    return d;
}

void NextHopRouter::setNextTx(PendingPacket *pending)
{
    assert(iface);
    auto d = iface->getRetransmissionMsec(pending->packet);
    pending->nextTxMsec = millis() + d;
    LOG_DEBUG("Setting next retransmission in %u msecs: ", d);
    printPacket("", pending->packet);
    setReceivedMessage(); // Run ASAP, so we can figure out our correct sleep time
}