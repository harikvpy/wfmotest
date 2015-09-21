// wfmotest.cpp : Test program that demonstrates the usage of WFMOHandler
//                class.
//
// Copyright (c) 2013 Hariharan Mahadevan, hari@smallpearl.com
//
// 

#include "stdafx.h"
#include "wfmohandler.h"

/*
 * A simple class that implements an asynchronous 'recv' UDP socket.
 * Socket binds to loopback address!
 */
class AsyncSocket {
    USHORT m_port;
    WSAEVENT m_event;
    SOCKET m_socket;
    AsyncSocket();
    AsyncSocket(const AsyncSocket&);
public:
    AsyncSocket(USHORT port)
        : m_port(port)
        , m_event(::WSACreateEvent())
        , m_socket(::WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, 0))
    {
        // bind the socket
        struct sockaddr_in sin = {0};
        sin.sin_family = AF_INET;
        sin.sin_port = ::htons(port);
        sin.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        if (m_event != NULL 
            && m_socket != INVALID_SOCKET 
            && ::bind(m_socket, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) == 0) {
            // put it in 'async' mode
            if (::WSAEventSelect(m_socket, m_event, FD_READ) == 0)
                return;
        }

        std::cerr << "Error initializing AsyncSocket, error code: " << WSAGetLastError() << std::endl;

        // something went wrong, release resources and raise an exception
        if (m_event != NULL) ::WSACloseEvent(m_event);
        if (m_socket != INVALID_SOCKET) ::closesocket(m_socket);
        throw std::exception("socket creation error");
    }
    ~AsyncSocket()
    {
        ::closesocket(m_socket);
        ::WSACloseEvent(m_event);
    }
    /* for direct access to the embedded event handle */
    operator HANDLE() { return m_event; }
    /*
     * Read all incoming packets in the socket's recv buffer. When all the packets 
     * in the buffer have been read, resets the associated Win32 event preparing it
     * for subsequent signalling when a new packet is copied into the buffer.
     */
    void ReadIncomingPacket()
    {
        std::vector<char> buf(64*1024);
        struct sockaddr_in from = {0};
        int fromlen = sizeof(from);
        int cbRecd = ::recvfrom(m_socket, 
            &buf[0], 
            buf.size(), 
            0, 
            reinterpret_cast<sockaddr*>(&from), 
            &fromlen);
        if (cbRecd > 0) {
            std::cerr << cbRecd << " bytes received on port " << m_port << std::endl;
        } else {
            int rc = ::WSAGetLastError();
            if (rc == WSAEWOULDBLOCK) {
                // no more data, reset the event so that WaitForMult..will block on it
                ::WSAResetEvent(m_event);
            } else {
                // something else went wrong
                std::cerr << "Error receiving data from port " << m_port 
                      << ", error code: " << ::WSAGetLastError() << std::endl;
            }
        }
    }
};

/*
    A sample daemon that uses WFMO to process its internal events.

    The daemon creates two UDP sockets on ports 5000 and 6000 and reads
    any incoming packets.

    Though only sockets are shown, the same can be extended to include
    any other types of object to which a Win32 waitable handle can be
    associated.
 */
class MyDaemon : public WFMOHandler {
    AsyncSocket m_socket1;
    AsyncSocket m_socket2;
    unsigned m_timerid;
    unsigned m_oneofftimerid;
public:
    MyDaemon() 
        : WFMOHandler()
        , m_socket1(5000)
        , m_socket2(6000)
        , m_timerid(0)
        , m_oneofftimerid(0)
    {
        // setup two handlers on the two AsyncSockets that we created
        WFMOHandler::AddWaitHandle(m_socket1, 
            std::bind(&AsyncSocket::ReadIncomingPacket, &m_socket1));
        WFMOHandler::AddWaitHandle(m_socket2, 
            std::bind(&AsyncSocket::ReadIncomingPacket, &m_socket2));
        m_timerid = WFMOHandler::AddTimer(1000, true, std::bind(&MyDaemon::RoutineTimer, this, &m_socket1));
        m_oneofftimerid = WFMOHandler::AddTimer(3000, false, std::bind(&MyDaemon::OneOffTimer, this));
    }
    virtual ~MyDaemon()
    {
        Stop();
        // just being graceful, WFMOHandler dtor will cleanup anyways 
        WFMOHandler::RemoveWaitHandle(m_socket2);
        WFMOHandler::RemoveWaitHandle(m_socket1);
    }
    void RoutineTimer(AsyncSocket* pSock)
    {
        pSock;
        std::cout << "Routine timer has expired!" << std::endl;
    }
    void OneOffTimer()
    {
        std::cout << "One off tmer has expired!" << std::endl;
        RemoveTimer(m_oneofftimerid);
        m_oneofftimerid = 0;
    }
};

HANDLE __hStopEvent = NULL;
// Ctrl+C/Ctrl+Break handler function
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCode)
{
  switch (dwCode)
  {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_SHUTDOWN_EVENT:
      ::SetEvent(__hStopEvent);
    return TRUE;
  default:
    return FALSE;
  }
}

int _tmain(int argc, _TCHAR* argv[])
{
    // setup Ctrl+Break handler
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    WSADATA wsad = {0};
    ::WSAStartup(MAKEWORD(2, 2), &wsad); // ought to succeed

    __hStopEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);

    try {
        MyDaemon md;
        md.Start();

        std::cout << "Daemon started, press Ctrl+C to stop." << std::endl;

        ::WaitForSingleObject(__hStopEvent, INFINITE);

    } catch (std::exception e) {
        std::cerr << "std::exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown exception" << std::endl;
    }

    ::CloseHandle(__hStopEvent);

    ::WSACleanup();

    return 0;
}
