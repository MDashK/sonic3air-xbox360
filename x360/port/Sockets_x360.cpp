/*
*	Xbox 360 port of Sonic 3 A.I.R.
*
*	Replacement for Oxygen/oxygenengine/source/oxygen_netcore/network/Sockets.cpp on Xbox 360,
*	which is excluded from the X360 build (see x360/build/oxygen_netcore.vcxproj).
*
*	Networking (update check, online ghosts, netplay, Crowd Control) is not supported on the
*	Xbox 360 port: the console's network stack (XNet) would need its own implementation and the
*	online features are not needed to play. Every socket operation fails cleanly, which the
*	engine already handles like "no connection".
*/

#include "oxygen_netcore/pch.h"
#include "oxygen_netcore/network/Sockets.h"


/* ----- Sockets ----- */

void Sockets::startupSockets()
{
	mIsInitialized = true;
}

void Sockets::shutdownSockets()
{
	mIsInitialized = false;
}

bool Sockets::resolveToIP(const std::string& hostName, std::string& outIP, bool useIPv6)
{
	outIP.clear();
	return false;
}


/* ----- SocketAddress ----- */

std::string SocketAddress::toLoggedString() const
{
	return mPreventIPLogging ? std::string("<hidden>") : toString();
}

void SocketAddress::assureSockAddr() const
{
	// No native sockaddr representation without a network stack
}

void SocketAddress::assureIpPort() const
{
	if (!mHasIpPort)
	{
		mIP.clear();
		mPort = 0;
		mHasIpPort = true;
	}
}

void SocketAddress::assureAddrHash() const
{
	if (!mHasAddrHash)
	{
		assureIpPort();
		mAddrHash = rmx::getMurmur2_64(String(mIP.c_str())) ^ (uint64)mPort;
		mHasAddrHash = true;
	}
}

bool SocketAddress::operator==(const SocketAddress& other) const
{
	return (getIP() == other.getIP() && getPort() == other.getPort());
}


/* ----- TCPSocket ----- */

TCPSocket::TCPSocket()
{
}

TCPSocket::~TCPSocket()
{
}

bool TCPSocket::isValid() const
{
	return false;
}

void TCPSocket::close()
{
}

const SocketAddress& TCPSocket::getRemoteAddress()
{
	static const SocketAddress EMPTY_ADDRESS;
	return EMPTY_ADDRESS;
}

void TCPSocket::swapWith(TCPSocket& other)
{
	std::swap(mInternal, other.mInternal);
}

bool TCPSocket::setupServer(uint16 serverPort, Sockets::ProtocolFamily protocolFamily)
{
	return false;
}

bool TCPSocket::acceptConnection(TCPSocket& outSocket)
{
	return false;
}

bool TCPSocket::connectTo(const std::string& serverAddress, uint16 serverPort, Sockets::ProtocolFamily protocolFamily)
{
	return false;
}

bool TCPSocket::sendData(const uint8* data, size_t length)
{
	return false;
}

bool TCPSocket::sendData(const std::vector<uint8>& data)
{
	return false;
}

bool TCPSocket::receiveBlocking(ReceiveResult& outReceiveResult)
{
	outReceiveResult.mBuffer.clear();
	return false;
}

bool TCPSocket::receiveNonBlocking(ReceiveResult& outReceiveResult)
{
	outReceiveResult.mBuffer.clear();
	return false;
}

bool TCPSocket::receiveInternal(ReceiveResult& outReceiveResult)
{
	outReceiveResult.mBuffer.clear();
	return false;
}


/* ----- UDPSocket ----- */

UDPSocket::~UDPSocket()
{
}

bool UDPSocket::isValid() const
{
	return false;
}

void UDPSocket::close()
{
}

bool UDPSocket::bindToPort(uint16 port, Sockets::ProtocolFamily protocolFamily)
{
	return false;
}

bool UDPSocket::bindToAnyPort(Sockets::ProtocolFamily protocolFamily)
{
	return false;
}

bool UDPSocket::sendData(const uint8* data, size_t length, const SocketAddress& destinationAddress)
{
	return false;
}

bool UDPSocket::sendData(const std::vector<uint8>& data, const SocketAddress& destinationAddress)
{
	return false;
}

bool UDPSocket::receiveBlocking(ReceiveResult& outReceiveResult)
{
	outReceiveResult.mBuffer.clear();
	return false;
}

bool UDPSocket::receiveNonBlocking(ReceiveResult& outReceiveResult)
{
	outReceiveResult.mBuffer.clear();
	return false;
}

bool UDPSocket::receiveInternal(ReceiveResult& outReceiveResult)
{
	outReceiveResult.mBuffer.clear();
	return false;
}
