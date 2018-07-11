#pragma once

#include <netinet/in.h>
#include <netinet/udp.h>

#include "netserver.h"

class Netserver_UDP : public NetserverLayer {

public:
	Netserver_UDP() {};

	void attach(NetserverLayer& parent) {
		NetserverLayer::attach(parent, IPPROTO_UDP);
	}

public:
	void recv(NetserverPacket &p) const;

};
