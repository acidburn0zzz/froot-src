#include <iostream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>

#include <arpa/inet.h>
#include <netinet/ip6.h>

#include "ipv6.h"
#include "checksum.h"

static std::ostream& operator<<(std::ostream& os, const in6_addr& addr)
{
	thread_local char buf[INET6_ADDRSTRLEN];
	return os << inet_ntop(AF_INET6, &addr, buf, sizeof buf);
}

static size_t payload_length(const std::vector<iovec>& iov)
{
	return std::accumulate(iov.cbegin() + 1, iov.cend(), 0U,
		[](size_t a, const iovec& b) {
			return a + b.iov_len;
		}
	);
}

void Netserver_IPv6::send_fragment(NetserverPacket& p,
	uint16_t offset, uint16_t chunk,
	const std::vector<iovec>& iovs, size_t iovlen, bool mf) const
{
	// calculate offsets and populate headers
	auto& ip6 = *reinterpret_cast<ip6_hdr*>(iovs[0].iov_base);	// FIXME
	auto& frag = *reinterpret_cast<ip6_frag*>(iovs[1].iov_base);	// FIXME

	ip6.ip6_plen = htons(chunk + sizeof frag);
	frag.ip6f_offlg = htons((offset & 0xfff8) | mf);

	send_up(p, iovs, iovlen);
}

void Netserver_IPv6::send(NetserverPacket& p, const std::vector<iovec>& iovs_in, size_t iovlen) const
{
	// thread local RNG for generating IPv6 IDs
	thread_local auto rnd = std::mt19937(std::chrono::system_clock::now().time_since_epoch().count() + 1);

	// determine maximum payload fragment size
	auto mtu = 1500U;			// TODO: s.getmtu();
	auto len = payload_length(iovs_in);

	auto& ip6_out = *reinterpret_cast<ip6_hdr*>(iovs_in[0].iov_base);	// FIXME

	// if it fits don't fragment it
	if (len + sizeof(ip6_hdr) < mtu) {
		ip6_out.ip6_plen = htons(len);
		send_up(p, iovs_in, iovlen);
		return;
	}

	// copy vectors because we're going to modify them
	auto iovs = iovs_in;

	// create fragment header and add to output
	uint32_t id = rnd();
	ip6_frag frag = { ip6_out.ip6_nxt, 0, 0, id };
	iovs.insert(iovs.begin() + 1, iovec { &frag, sizeof frag });

	// the original IPv6 nxt field has to change now
	ip6_out.ip6_nxt = IPPROTO_FRAGMENT;

	// state variables
	auto max_frag = mtu - sizeof(ip6_hdr) - sizeof(ip6_frag);
	auto chunk = 0U;
	auto offset = 0U;

	auto iter = iovs.begin() + 2;
	while (iter != iovs.end()) {

		auto& vec = *iter++;
		auto base = reinterpret_cast<uint8_t*>(vec.iov_base);
		auto len = vec.iov_len;
		chunk += len;

		// did we take too much?
		if (chunk > max_frag) {

			// how much too much?
			auto excess = chunk - max_frag;
			chunk -= excess;

			// frags need to be a multiple of 8 in length
			auto round = chunk % 8;
			chunk -= round;
			excess += round;

			// adjust this iovec's len to the new total length
			vec.iov_len -= excess;

			// and insert a new iovec after this one that holds the left-overs
			// assignment necessary in case old iterator is invalidated
			iter = iovs.insert(iter, iovec { base + vec.iov_len, len - vec.iov_len});

			// send fragment (with MF bit), remembering which layer we're on
			auto tmp = p.current;
			send_fragment(p, offset, chunk, iovs, iter - iovs.cbegin(), true);
			p.current = tmp;;

			// remove the already transmitted iovecs (excluding the IPv6 header and Fragment EH)
			iter = iovs.erase(iovs.begin() + 2, iter);

			// start collecting the next chunk
			offset += chunk;
			chunk = 0;
		}
	}

	// send final fragment
	send_fragment(p, offset, chunk, iovs, iovs.size(), false);
}

static bool match_solicited(const std::vector<in6_addr>& list, const in6_addr& addr)
{
	static const in6_addr solicit_prefix = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xff, 0, 0, 0 };

	if (::memcmp(&addr, &solicit_prefix, 13) != 0) return false;

	return std::any_of(list.cbegin(), list.cend(), [&](const in6_addr& cmp) {
		auto* p = reinterpret_cast<const uint8_t*>(&addr);
		auto* q = reinterpret_cast<const uint8_t*>(&cmp);
		return ::memcmp(p + 13, q + 13, 3) == 0;
	});
}

static bool match_exact(const std::vector<in6_addr>& list, const in6_addr& addr)
{
	return std::any_of(list.cbegin(), list.cend(), [&](const in6_addr& cmp) {
		return ::memcmp(&addr, &cmp, sizeof addr) == 0;
	});
}

bool Netserver_IPv6::match(const in6_addr& in) const
{
	return match_exact(addr, in) || match_solicited(addr, in);
}

static uint8_t skip_extension_headers(NetserverPacket& p, uint8_t next)
{
	// we don't know how to handle receipt of IPv6 fragments
	if (next == IPPROTO_FRAGMENT) {
		return IPPROTO_NONE;
	}

	// recognized IPv6 extension headers
	if (next != IPPROTO_HOPOPTS &&
	    next != IPPROTO_DSTOPTS &&
	    next != IPPROTO_ROUTING &&
	    next != IPPROTO_MH)
	{
		return next;
	}

	// read the extension header and skip the contents
	ReadBuffer& in = p.readbuf;
	if (in.available() < 2) {
		return IPPROTO_NONE;
	}
	next = in.read<uint8_t>();
	auto optlen = in.read<uint8_t>() * 8U + 6U;
	if (in.available() < optlen) {
		return IPPROTO_NONE;
	}
	(void) in.read<uint8_t>(optlen);

	// pass back to the main parser
	return next;
}

void Netserver_IPv6::recv(NetserverPacket& p) const
{
	ReadBuffer& in = p.readbuf;

	// extract L3 header
	auto version = (in[0] >> 4) & 0x0f;
	if (version != 6) return;

	// check IPv6 header length
	auto ihl = sizeof(ip6_hdr);
	if (in.available() < ihl) return;

	// read IPv6 header
	auto& ip6_in = in.read<ip6_hdr>();

	// hack for broken AF_PACKET size - recreate the buffer
	// based on the IP header specified length instead of what
	// was returned by the AF_PACKET layer
	if (in.size() == 46) {
		size_t pos = in.position();
		size_t len = ntohs(ip6_in.ip6_plen);
		if (len < 46) {
			in = ReadBuffer(&in[0], len);
			(void) in.read<uint8_t>(pos);
		}
	}

	// check if it's for us
	if (!match(ip6_in.ip6_dst)) return;

	// skip over any extension headers
	auto next = skip_extension_headers(p, ip6_in.ip6_nxt);
	if (next == IPPROTO_NONE) {
		return;
	}

	// ignore if the next protocol isn't registered
	if (!registered(next)) return;

	// IPv6 header creation
	ip6_hdr ip6_out;
	ip6_out.ip6_flow = ip6_in.ip6_flow;
	ip6_out.ip6_plen = 0;
	ip6_out.ip6_nxt = next;
	ip6_out.ip6_hlim = 255;

	// don't send from multicast addresses
	if (ip6_in.ip6_dst.s6_addr[0] == 0xff) {
		ip6_out.ip6_src = addr[0];
	} else {
		ip6_out.ip6_src = ip6_in.ip6_dst;
	}
	ip6_out.ip6_dst = ip6_in.ip6_src;
	p.push(iovec { &ip6_out, sizeof ip6_out } );

	// IPv6 pseudo-header
	p.crc.add(&ip6_out.ip6_src, sizeof(in6_addr));
	p.crc.add(&ip6_out.ip6_dst, sizeof(in6_addr));
	p.crc.add(next);

	// dispatch to layer four handling
	dispatch(p, next);
}

Netserver_IPv6::Netserver_IPv6(const ether_addr& ether)
{
	in6_addr link_local = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0, 0 };
	link_local.s6_addr[8] = ether.ether_addr_octet[0] ^ 0x02;
	link_local.s6_addr[9] = ether.ether_addr_octet[1];
	link_local.s6_addr[10] = ether.ether_addr_octet[2];
	link_local.s6_addr[13] = ether.ether_addr_octet[3];
	link_local.s6_addr[14] = ether.ether_addr_octet[4];
	link_local.s6_addr[15] = ether.ether_addr_octet[5];
	std::cerr << link_local << std::endl;
	addr.push_back(link_local);
}
