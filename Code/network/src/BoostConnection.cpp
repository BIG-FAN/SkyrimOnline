#include "BoostConnection.h"
#include "BoostManager.h"
#include "BoostServer.h"

#include <easylogging++.h>

#include "salsa.h"
#include "sha.h"
#include "ripemd.h"
#include "Packet.h"

using namespace CryptoPP;

class KeyExchangePacket : public Packet
{
public:
	void Deserialize(ReadBuffer* pBuffer)
	{
		buffer.resize(128);
		pBuffer->ReadRaw((uint8_t*)&buffer[0], (uint32_t)buffer.size());
	}

	void Serialize(WriteBuffer* pBuffer)
	{
		pBuffer->WriteRaw((const uint8_t*)buffer.data(), buffer.size());
	}

	std::string buffer;
};

BoostConnection::BoostConnection(boost::asio::io_service& pIoService, uint16_t aId)
    :m_socket(pIoService),
	 m_resolver(pIoService),
     m_id(aId),
     m_connected(false),
     m_markedForDelete(false),
	 m_writeInProgress(false),
	 m_encrypted(false),
     m_outstandingTasks(0),
	 m_pEncryption(nullptr),
	 m_pDecryption(nullptr)
{
}

BoostConnection::~BoostConnection()
{
	delete (Salsa20::Encryption*)m_pEncryption; m_pEncryption = nullptr;
	delete (Salsa20::Decryption*)m_pDecryption; m_pDecryption = nullptr;

	while (!m_incomingQueue.empty())
	{
		delete m_incomingQueue.front();
		m_incomingQueue.pop();
	}
}

void BoostConnection::Connect(const std::string& aIp, uint16_t aPort)
{
	boost::asio::ip::tcp::resolver::query query(boost::asio::ip::tcp::v4(), aIp, std::to_string(aPort));
	m_resolver.async_resolve(query,
		boost::bind(&BoostConnection::HandleResolve,
		this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::iterator));
}

void BoostConnection::Write(const std::string& apBuffer)
{
    if(!m_connected)
        return;
	
	boost::recursive_mutex::scoped_lock _(m_outgoingQueueLock);
    bool empty = m_outgoingQueue.empty();
	m_outgoingQueue.push(apBuffer);
	if (empty && m_writeInProgress == false)
    {
		m_writeInProgress = true;
        ++m_outstandingTasks;
        
		m_socket.get_io_service().post(boost::bind(&BoostConnection::DoWrite, this));
    }
}

void BoostConnection::HandleWrite(const boost::system::error_code& aError)
{
    --m_outstandingTasks;
    if (!aError)
    {
		boost::recursive_mutex::scoped_lock _(m_outgoingQueueLock);
        m_outgoingQueue.pop();
        if (!m_outgoingQueue.empty())
        {
            ++m_outstandingTasks;

			m_socket.get_io_service().post(boost::bind(&BoostConnection::DoWrite, this));
        }
		else
		{
			m_writeInProgress = false;
		}
    }
    else
    {
         Close();
    }
}

void BoostConnection::DoWrite()
{
	boost::recursive_mutex::scoped_lock _(m_outgoingQueueLock);

	Salsa20::Encryption* pEncryptor = (Salsa20::Encryption*)m_pEncryption;
	if (pEncryptor && IsEncrypted())
	{
		pEncryptor->ProcessData((uint8_t*)&m_outgoingQueue.front()[4], (uint8_t*)&m_outgoingQueue.front()[4], m_outgoingQueue.front().size() - 4);
	}

	boost::asio::async_write(m_socket,
		boost::asio::buffer(m_outgoingQueue.front().data(), m_outgoingQueue.front().size()),
		boost::bind(&BoostConnection::HandleWrite, this, boost::asio::placeholders::error));
}

bool BoostConnection::Consume(ReadBuffer*& apBuffer)
{
	apBuffer = nullptr;
    boost::mutex::scoped_lock _(m_incomingQueueLock);
    if(!m_incomingQueue.empty())
    {
		m_lastEvent = boost::chrono::steady_clock::now();
        apBuffer = m_incomingQueue.front();
        m_incomingQueue.pop();

		return true;
    }
    return false;
}

void BoostConnection::Close()
{
    if(m_connected)
    {
        m_connected = false;
        m_socket.close();
    }
}

bool BoostConnection::IsConnected()
{
	boost::chrono::duration<double> sec = boost::chrono::steady_clock::now() - m_lastEvent;
	if (sec.count() > 500)
	{
		Close();
	}
    return m_connected;
}

bool BoostConnection::IsEncrypted() const
{
	return m_encrypted;
}

void BoostConnection::HandleConnect(const boost::system::error_code& pError)
{
    --m_outstandingTasks;
    if(!pError)
    {
        m_connected = true;
        StartEncryptionHandshake();
    }
    else
    {
		LOG(ERROR) << "Connect Failed with : " << pError.message();
        Close();
    }
}

void BoostConnection::HandleResolve(const boost::system::error_code& aError, boost::asio::ip::tcp::resolver::iterator aEndpointItor)
{
	
	--m_outstandingTasks;
	if (!aError)
	{
		++m_outstandingTasks;
		boost::asio::async_connect(m_socket, aEndpointItor,
			boost::bind(&BoostConnection::HandleConnect, this,
			boost::asio::placeholders::error));
	}
	else
	{
		LOG(ERROR) << "Resolve Failed with : " << aError.message();
		Close();
	}
}

void BoostConnection::Accept()
{
    m_connected = true;
	m_lastEvent = boost::chrono::steady_clock::now();
    Read();
}

void BoostConnection::SetSharedSecret(const std::string& acSharedSecret, bool aClient)
{
	int salsaKeyLength = SHA256::BLOCKSIZE; 

	// Calculate a SHA-256 hash over the Diffie-Hellman session key
	SecByteBlock key(SHA256::DIGESTSIZE);
	SHA256().CalculateDigest(key, (const uint8_t*)acSharedSecret.data(), acSharedSecret.size());

	if (aClient)
	{
		m_pEncryption = (void*)new Salsa20::Encryption(key, key.SizeInBytes(), (const uint8_t*)acSharedSecret.data());
		m_pDecryption = (void*)new Salsa20::Encryption(key, key.SizeInBytes(), (const uint8_t*)acSharedSecret.data() + 8);
	}
	else
	{
		m_pEncryption = (void*)new Salsa20::Encryption(key, key.SizeInBytes(), (const uint8_t*)acSharedSecret.data() + 8);
		m_pDecryption = (void*)new Salsa20::Encryption(key, key.SizeInBytes(), (const uint8_t*)acSharedSecret.data());
	}
	
}

void BoostConnection::EnableEncryption()
{
	m_encrypted = true;
}

void BoostConnection::Read()
{
	++m_outstandingTasks;
	boost::asio::async_read(m_socket, boost::asio::buffer((void*)&m_incomingLength, sizeof(m_incomingLength)),
		boost::asio::transfer_all(),
		boost::bind(&BoostConnection::HandleReadHeader, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred)
		);
}

void BoostConnection::HandleReadHeader(const boost::system::error_code& aError, size_t aBytesRead)
{
	--m_outstandingTasks;
	if (aError || aBytesRead != sizeof(m_incomingLength) || m_incomingLength >= 0x2000)
	{
		Close();
		return;
	}

	++m_outstandingTasks;
	m_buffer.resize((size_t)m_incomingLength);
	boost::asio::async_read(m_socket, boost::asio::buffer((void*)&m_buffer[0], (size_t)m_incomingLength),
		boost::asio::transfer_all(),
		boost::bind(&BoostConnection::HandleReadBody, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred)
		);
}

void BoostConnection::HandleReadBody(const boost::system::error_code& aError, size_t aBytesRead)
{
	--m_outstandingTasks;
	if (aError || aBytesRead != (size_t)m_incomingLength)
	{
		Close();
		return;
	}

	CryptoPP::Salsa20::Decryption* pDecryptor = (CryptoPP::Salsa20::Decryption*)m_pDecryption;

	// Non compressed version
	if (pDecryptor && IsEncrypted())
	{
		pDecryptor->ProcessData((uint8_t*)&m_buffer[0], (uint8_t*)&m_buffer[0], m_buffer.size());
	}

	ReadBuffer* pBuffer = new ReadBuffer((uint8_t*)m_buffer.data(), m_buffer.size());
	if (pDecryptor && IsEncrypted())
	{
		boost::mutex::scoped_lock _(m_incomingQueueLock);
		m_incomingQueue.push(pBuffer);
	}
	else
	{
		HandleHandshake(pBuffer);
		delete pBuffer;
	}

	Read();
}

void BoostConnection::StartEncryptionHandshake()
{
	std::string buffer;
	WriteBuffer wbuffer;
	BoostManager::GetInstance().GetKeyAgreement()->Serialize(buffer);

	wbuffer.Write_uint8(0);
	wbuffer.WriteRaw((const uint8_t*)buffer.data(), buffer.size());

	Write(BoostServer::Serialize(&wbuffer));

	++m_outstandingTasks;
	boost::asio::async_read(m_socket, boost::asio::buffer((void*)&m_incomingLength, sizeof(m_incomingLength)),
		boost::asio::transfer_all(),
		boost::bind(&BoostConnection::HandleReadEncryptionHeader, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred)
		);
}

void BoostConnection::HandleReadEncryptionHeader(const boost::system::error_code& aError, size_t aBytesRead)
{
	--m_outstandingTasks;
	if (aError || aBytesRead != sizeof(m_incomingLength) || m_incomingLength != 128)
	{
		Close();
		return;
	}

	++m_outstandingTasks;
	m_buffer.resize((size_t)m_incomingLength);
	boost::asio::async_read(m_socket, boost::asio::buffer((void*)&m_buffer[0], (size_t)m_incomingLength),
		boost::asio::transfer_all(),
		boost::bind(&BoostConnection::HandleReadEncryptionBody, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred)
		);
}

void BoostConnection::HandleReadEncryptionBody(const boost::system::error_code& aError, size_t aBytesRead)
{
	--m_outstandingTasks;
	if (aError || aBytesRead != (size_t)m_incomingLength)
	{
		Close();
		return;
	}

	ReadBuffer buffer((const uint8_t*)m_buffer.data(), m_buffer.size());
	std::string sharedSecret = BoostManager::GetInstance().GetKeyAgreement()->Agree(&buffer);
	if (sharedSecret.size() == 0)
	{
		Close();
		return;
	}
	SetSharedSecret(sharedSecret, true);

	WriteBuffer wbuffer;
	wbuffer.Write_uint8(1);
	wbuffer.Write_uint32(0x12345678);

	m_outgoingQueue.push(BoostServer::Serialize(&wbuffer));

	m_writeInProgress = true;
	boost::asio::async_write(m_socket,
		boost::asio::buffer(m_outgoingQueue.front().data(), m_outgoingQueue.front().size()),
		boost::bind(&BoostConnection::HandleWrite, this, boost::asio::placeholders::error));

	

	EnableEncryption();
	boost::mutex::scoped_lock _(m_incomingQueueLock);
	m_incomingQueue.push(nullptr);

	Read();
}

void BoostConnection::HandleHandshake(ReadBuffer* apBuffer)
{
	uint8_t type = 0;
	apBuffer->Read_uint8(type);
	switch (type)
	{
	case 0:
	{
		std::string sharedSecret = BoostManager::GetInstance().GetKeyAgreement()->Agree(apBuffer);
		if (sharedSecret.size() == 0)
		{
			Close();
			return;
		}
		SetSharedSecret(sharedSecret, false);

		std::string buffer;
		BoostManager::GetInstance().GetKeyAgreement()->Serialize(buffer);

		Write(BoostServer::Serialize(buffer));
	}
		break;
	case 1:
	{
		uint32_t magic;
		apBuffer->Read_uint32(magic);
		if (magic == 0x12345678)
		{
			EnableEncryption();
			boost::mutex::scoped_lock _(m_incomingQueueLock);
			m_incomingQueue.push(nullptr);
			return;
		}
	}
	default:

		Close();
		break;
	}
}