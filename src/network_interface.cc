#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

using namespace std;

// ethernet_address: Ethernet (what ARP calls "hardware") address of the interface
// ip_address: IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface( const EthernetAddress& ethernet_address, const Address& ip_address )
  : ethernet_address_( ethernet_address ), ip_address_( ip_address )
{
  cerr << "DEBUG: Network interface has Ethernet address " << to_string( ethernet_address_ ) << " and IP address "
       << ip_address.ip() << "\n";
}

// dgram: the IPv4 datagram to be sent
// next_hop: the IP address of the interface to send it to (typically a router or default gateway, but
// may also be another host if directly connected to the same network as the destination)

// Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) by using the
// Address::ipv4_numeric() method.
/**
 * 当调用者（例如你的 TCPConnection 或路由器）想要向下一跳发送一个出站的互联网（IP）数据报时，会调用这个方法。
 * 这是你的接口的工作，将这个数据报转换成一个以太网帧并（最终）发送它。
 * - 如果目标以太网地址已知，立即发送。创建一个以太网帧（类型为 EthernetHeader::TYPE IPv4），将有效载荷设置为序列化后的数据报，并设置源地址和目标地址。
 * - 如果目标以太网地址未知，广播一个用于下一跳以太网地址的 ARP 请求，并将 IP 数据报排队，以便在收到 ARP 回复后发送。
 * 例外：你不想用 ARP 请求淹没网络。如果网络接口在过去的五秒内已经发送过关于相同 IP 地址的 ARP 请求，不要发送第二个请求，只需等待第一个请求的回复。
 * 同样，排队数据报，直到你了解到目标以太网地址。
*/
void NetworkInterface::send_datagram( const InternetDatagram& dgram, const Address& next_hop )
{
  auto const& target_ip = next_hop.ipv4_numeric();
  if ( ip2ether_.contains( target_ip ) ) {
    EthernetFrame frame { { ip2ether_[target_ip].first, ethernet_address_, EthernetHeader::TYPE_IPv4 },
                          serialize( dgram ) };
    out_frames_.push( std::move( frame ) );
  } else {
    if ( !arp_timer_.contains( target_ip ) ) {
      ARPMessage request_msg;
      request_msg.opcode = ARPMessage::OPCODE_REQUEST;
      request_msg.sender_ethernet_address = ethernet_address_;
      request_msg.sender_ip_address = ip_address_.ipv4_numeric();
      request_msg.target_ip_address = target_ip;
      EthernetFrame frame { { ETHERNET_BROADCAST, ethernet_address_, EthernetHeader::TYPE_ARP },
                            serialize( request_msg ) };
      out_frames_.push( std::move( frame ) );
      arp_timer_.emplace( next_hop.ipv4_numeric(), 0 );
      waited_dgrams_.insert( { target_ip, { dgram } } );
    } else {
      waited_dgrams_[target_ip].push_back( dgram );
    }
  }
}

// frame: the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame( const EthernetFrame& frame )
{
  if ( frame.header.dst != ethernet_address_ && frame.header.dst != ETHERNET_BROADCAST ) {
    return {};
  }

  if ( frame.header.type == EthernetHeader::TYPE_IPv4 ) {
    InternetDatagram dgram;
    if ( parse( dgram, frame.payload ) ) {
      return dgram;
    }
  } else if ( frame.header.type == EthernetHeader::TYPE_ARP ) {
    ARPMessage msg;
    if ( parse( msg, frame.payload ) ) {
      ip2ether_.insert( { msg.sender_ip_address, { msg.sender_ethernet_address, 0 } } );
      if ( msg.opcode == ARPMessage::OPCODE_REQUEST ) {
        if ( msg.target_ip_address == ip_address_.ipv4_numeric() ) {
          ARPMessage reply_msg;
          reply_msg.opcode = ARPMessage::OPCODE_REPLY;
          reply_msg.sender_ethernet_address = ethernet_address_;
          reply_msg.sender_ip_address = ip_address_.ipv4_numeric();
          reply_msg.target_ethernet_address = msg.sender_ethernet_address;
          reply_msg.target_ip_address = msg.sender_ip_address;
          EthernetFrame reply_frame { { msg.sender_ethernet_address, ethernet_address_, EthernetHeader::TYPE_ARP },
                                      serialize( reply_msg ) };
          out_frames_.push( std::move( reply_frame ) );
        }
      } else if ( msg.opcode == ARPMessage::OPCODE_REPLY ) {
        ip2ether_.insert( { msg.sender_ip_address, { msg.sender_ethernet_address, 0 } } );
        auto const& dgrams = waited_dgrams_[msg.sender_ip_address];
        for ( auto const& dgram : dgrams ) {
          send_datagram( dgram, Address::from_ipv4_numeric( msg.sender_ip_address ) );
        }
        waited_dgrams_.erase( msg.sender_ip_address );
      }
    }
  }
  return {};
}

// ms_since_last_tick: the number of milliseconds since the last call to this method
void NetworkInterface::tick( const size_t ms_since_last_tick )
{
  static constexpr size_t IP_MAP_TTL = 30000;     // 30s
  static constexpr size_t ARP_REQUEST_TTL = 5000; // 5s

  for ( auto it = ip2ether_.begin(); it != ip2ether_.end(); ) {
    it->second.second += ms_since_last_tick;
    if ( it->second.second >= IP_MAP_TTL ) {
      it = ip2ether_.erase( it );
    } else {
      ++it;
    }
  }

  for ( auto it = arp_timer_.begin(); it != arp_timer_.end(); ) {
    it->second += ms_since_last_tick;
    if ( it->second >= ARP_REQUEST_TTL ) {
      it = arp_timer_.erase( it );
    } else {
      ++it;
    }
  }
}

optional<EthernetFrame> NetworkInterface::maybe_send()
{
  if ( out_frames_.empty() ) {
    return {};
  }
  auto frame = out_frames_.front();
  out_frames_.pop();
  return frame;
}