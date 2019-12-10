#ifndef MS_TCP_CONNECTION_HPP
#define MS_TCP_CONNECTION_HPP

#include "common.hpp"
#include <uv.h>
#include <string>

// Avoid cyclic #include problem by declaring classes instead of including
// the corresponding header files.
class TcpServer;

class TcpConnection
{
public:
	class Listener
	{
	public:
		virtual ~Listener() = default;

	public:
		virtual void OnTcpConnectionClosed(TcpConnection* connection) = 0;
	};

public:
	/* Struct for the data field of uv_req_t when writing into the connection. */
	struct UvWriteData
	{
		uv_write_t req;
		uint8_t store[1];
	};

	// Let the TcpServer class directly call the destructor of TcpConnection.
	friend class TcpServer;

public:
	explicit TcpConnection(size_t bufferSize);
	TcpConnection& operator=(const TcpConnection&) = delete;
	TcpConnection(const TcpConnection&)            = delete;
	virtual ~TcpConnection();

public:
	void Close();
	virtual void Dump() const;
	void Setup(
	  Listener* listener,
	  struct sockaddr_storage* localAddr,
	  const std::string& localIp,
	  uint16_t localPort);
	bool IsClosed() const;
	uv_tcp_t* GetUvHandle() const;
	void Start();
	void Write(const uint8_t* data, size_t len);
	void Write(const uint8_t* data1, size_t len1, const uint8_t* data2, size_t len2);
	void Write(const std::string& data);
	void ErrorReceiving();
	const struct sockaddr* GetLocalAddress() const;
	int GetLocalFamily() const;
	const std::string& GetLocalIp() const;
	uint16_t GetLocalPort() const;
	const struct sockaddr* GetPeerAddress() const;
	const std::string& GetPeerIp() const;
	uint16_t GetPeerPort() const;

private:
	bool SetPeerAddress();

	/* Callbacks fired by UV events. */
public:
	void OnUvReadAlloc(size_t suggestedSize, uv_buf_t* buf);
	void OnUvRead(ssize_t nread, const uv_buf_t* buf);
	void OnUvWriteError(int error);

	/* Pure virtual methods that must be implemented by the subclass. */
protected:
	virtual void UserOnTcpConnectionRead() = 0;

protected:
	// Passed by argument.
	size_t bufferSize{ 0 };
	// Allocated by this.
	uint8_t* buffer{ nullptr };
	// Others.
	size_t bufferDataLen{ 0 };
	std::string localIp;
	uint16_t localPort{ 0 };
	struct sockaddr_storage peerAddr;
	std::string peerIp;
	uint16_t peerPort{ 0 };

private:
	// Passed by argument.
	Listener* listener{ nullptr };
	// Allocated by this.
	uv_tcp_t* uvHandle{ nullptr };
	// Others.
	struct sockaddr_storage* localAddr{ nullptr };
	bool closed{ false };
	bool isClosedByPeer{ false };
	bool hasError{ false };
};

/* Inline methods. */

inline bool TcpConnection::IsClosed() const
{
	return this->closed;
}

inline uv_tcp_t* TcpConnection::GetUvHandle() const
{
	return this->uvHandle;
}

inline void TcpConnection::Write(const std::string& data)
{
	Write(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
}

inline const struct sockaddr* TcpConnection::GetLocalAddress() const
{
	return reinterpret_cast<const struct sockaddr*>(this->localAddr);
}

inline int TcpConnection::GetLocalFamily() const
{
	return reinterpret_cast<const struct sockaddr*>(this->localAddr)->sa_family;
}

inline const std::string& TcpConnection::GetLocalIp() const
{
	return this->localIp;
}

inline uint16_t TcpConnection::GetLocalPort() const
{
	return this->localPort;
}

inline const struct sockaddr* TcpConnection::GetPeerAddress() const
{
	return reinterpret_cast<const struct sockaddr*>(&this->peerAddr);
}

inline const std::string& TcpConnection::GetPeerIp() const
{
	return this->peerIp;
}

inline uint16_t TcpConnection::GetPeerPort() const
{
	return this->peerPort;
}

#endif
