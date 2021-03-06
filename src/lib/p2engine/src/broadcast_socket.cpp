/*

Copyright (c) 2007, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the distribution.
* Neither the name of the author nor the names of its
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/
#include "p2engine/broadcast_socket.hpp"
#include "p2engine/enum_net.hpp"

#include "p2engine/push_warning_option.hpp"
#include <boost/bind.hpp>
#include <boost/asio/ip/multicast.hpp>
#include "p2engine/pop_warning_option.hpp"

namespace p2engine{

	bool is_local(address const& a)
	{
		if (a.is_v6()) 
			return a.to_v6().is_link_local();
		address_v4 a4 = a.to_v4();
		unsigned long ip = a4.to_ulong();
		return ((ip & 0xff000000) == 0x0a000000
			|| (ip & 0xfff00000) == 0xac100000
			|| (ip & 0xffff0000) == 0xc0a80000);
	}

	bool is_loopback(address const& addr)
	{
		if (addr.is_v4())
			return addr.to_v4() == address_v4::loopback();
		else
			return addr.to_v6() == address_v6::loopback();
	}

	bool is_multicast(address const& addr)
	{
		if (addr.is_v4())
			return addr.to_v4().is_multicast();
		else
			return addr.to_v6().is_multicast();
	}

	bool is_any(address const& addr)
	{
		if (addr.is_v4())
			return addr.to_v4() == address_v4::any();
		else if (addr.to_v6().is_v4_mapped())
			return (addr.to_v6().to_v4() == address_v4::any());
		else
			return addr.to_v6() == address_v6::any();
	}

	bool is_teredo(address const& addr)
	{
		try {
			if (!addr.is_v6()) return false;
			boost::uint8_t teredo_prefix[] = {0x20, 0x01, 0, 0};
			address_v6::bytes_type b = addr.to_v6().to_bytes();
			return memcmp(&b[0], teredo_prefix, 4) == 0;
		} catch(std::exception& e) { 
			DEBUG_SCOPE(
				printf("broadcast_socket is_teredo %s\n", e.what());
			);
			(void)(e);
			return false; 
		}
		return false;
	}

	bool supports_ipv6()
	{
		error_code ec;
		address::from_string("::1", ec);
		return !ec;
	}

	address guess_local_address(io_service& ios)
	{
		// make a best guess of the interface we're using and its IP
		error_code ec;
		std::vector<ip_interface> const& interfaces = enum_net_interfaces(ios, ec);
		address ret = address_v4::any();
		for (std::vector<ip_interface>::const_iterator i = interfaces.begin()
			, end(interfaces.end()); i != end; ++i)
		{
			address const& a = i->interface_address;
			if (is_loopback(a)
				|| is_multicast(a)
				|| is_any(a)) continue;

			// prefer a v4 address, but return a v6 if
			// there are no v4
			if (a.is_v4()) 
			{
				if (!is_local(a))
					return a;
				ret=a;
			}
			else if (ret != address_v4::any())
				ret = a;
		}
		if (ret==address_v4::any())
			ret=address_v4::loopback();
		return ret;
	}

	// count the length of the common bit prefix
	int common_bits(unsigned char const* b1
		, unsigned char const* b2, size_t n)
	{
		for (int i = 0; i < (int)n; ++i, ++b1, ++b2)
		{
			unsigned char a = *b1 ^ *b2;
			if (a == 0) continue;
			int ret = i * 8 + 8;
			for (; a > 0; a >>= 1) --ret;
			return ret;
		}
		return (int)n * 8;
	}

	// returns the number of bits in that differ from the right
	// between the addresses.
	int cidr_distance(address const& a1, address const& a2)
	{
		if (a1.is_v4() && a2.is_v4())
		{
			// both are v4
			address_v4::bytes_type b1 = a1.to_v4().to_bytes();
			address_v4::bytes_type b2 = a2.to_v4().to_bytes();
			return int(b1.size() * 8) - common_bits(&b1[0], &b2[0], b1.size());
		}

		address_v6::bytes_type b1;
		address_v6::bytes_type b2;
		if (a1.is_v4()) b1 = address_v6::v4_mapped(a1.to_v4()).to_bytes();
		else b1 = a1.to_v6().to_bytes();
		if (a2.is_v4()) b2 = address_v6::v4_mapped(a2.to_v4()).to_bytes();
		else b2 = a2.to_v6().to_bytes();
		return  int(b1.size() * 8)- common_bits(&b1[0], &b2[0], b1.size());
	}

	broadcast_socket::broadcast_socket(io_service& ios
		, udp::endpoint const& multicast_endpoint
		, receive_handler_t const& handler
		, bool loopback, bool joingroup
		)
		: m_multicast_endpoint(multicast_endpoint)
		, m_on_receive(handler)
	{
		BOOST_ASSERT(is_multicast(m_multicast_endpoint.address()));

		using namespace asio::ip::multicast;

		error_code ec;
		std::vector<ip_interface> interfaces= enum_net_interfaces(ios, ec);

		bool isV4=multicast_endpoint.address().is_v4();
		if (joingroup)
		{
			if (isV4)
				open_multicast_socket(ios, address_v4::any(), loopback);
			else
				open_multicast_socket(ios, address_v6::any(), loopback);
		}

		for (std::vector<ip_interface>::const_iterator i = interfaces.begin()
			, end(interfaces.end()); i != end; ++i)
		{
			// only multicast on compatible networks
			if (i->interface_address.is_v4() != isV4) continue;
			// ignore any loopback interface
			if (is_loopback(i->interface_address)) continue;
			// ignore address 0 interface
			if (is_any(i->interface_address)) continue;
			open_unicast_socket(ios, i->interface_address);
		}
	}

	void broadcast_socket::open_multicast_socket(io_service& ios
		, address const& addr, bool loopback)
	{
		using namespace asio::ip::multicast;

		error_code ec;
		boost::shared_ptr<datagram_socket> s(new datagram_socket(ios));
		s->open(addr.is_v4() ? udp::v4() : udp::v6(), ec);
		if (ec) return;
		s->set_option(datagram_socket::reuse_address(true), ec);
		if (ec) return;
		s->bind(udp::endpoint(addr, m_multicast_endpoint.port()), ec);
		if (ec) return;
		s->set_option(join_group(m_multicast_endpoint.address()), ec);
		if (ec) return;
		s->set_option(hops(255), ec);
		if (ec) return;
		s->set_option(enable_loopback(loopback), ec);
		if (ec) return;
		m_sockets.push_back(socket_entry(s));
		socket_entry& se = m_sockets.back();
		s->async_receive_from(asio::buffer(se.buffer, sizeof(se.buffer))
			, se.remote, bind(&broadcast_socket::on_receive, this, &se, _1, _2));
	}

	void broadcast_socket::open_unicast_socket(io_service& ios, address const& addr)
	{
		using namespace asio::ip::multicast;
		error_code ec;
		boost::shared_ptr<datagram_socket> s(new datagram_socket(ios));
		s->open(addr.is_v4() ? udp::v4() : udp::v6(), ec);
		if (ec) return;
		s->bind(udp::endpoint(addr, 0), ec);
		if (ec) return;
		m_unicast_sockets.push_back(socket_entry(s));
		socket_entry& se = m_unicast_sockets.back();
		s->async_receive_from(asio::buffer(se.buffer, sizeof(se.buffer))
			, se.remote, bind(&broadcast_socket::on_receive, this, &se, _1, _2));
	}

	void broadcast_socket::send(const safe_buffer& buf, error_code& ec)
	{
		struct dummy  
		{
			static void dummy_handler(error_code, size_t){}
		};
		
		BOOST_AUTO(asiobuf, buf.to_asio_const_buffers_1());
		for (std::list<socket_entry>::iterator i = m_unicast_sockets.begin()
			, end(m_unicast_sockets.end()); i != end; ++i)
		{
			if (!i->socket) continue;
			i->socket->async_send_to(asiobuf, m_multicast_endpoint, 0, 
				boost::bind(&dummy::dummy_handler, _1, _2));
		}
	}

	void broadcast_socket::on_receive(socket_entry* s, error_code const& ec
		, size_t bytes_transferred)
	{
		if (ec || bytes_transferred == 0 || !m_on_receive) return;
		m_on_receive(s->remote, s->buffer, (int)bytes_transferred);
		if (!s->socket) return;
		s->socket->async_receive_from(asio::buffer(s->buffer, sizeof(s->buffer))
			, s->remote, bind(&broadcast_socket::on_receive, this, s, _1, _2));
	}

	void broadcast_socket::close()
	{
		std::for_each(m_sockets.begin(), m_sockets.end(), bind(&socket_entry::close, _1));
		std::for_each(m_unicast_sockets.begin(), m_unicast_sockets.end(), bind(&socket_entry::close, _1));

		m_on_receive.clear();
	}
}


