// netsend.cpp : Program to send a string to a specific UDP port 
//               on the localhost. The string and the port number
//               are to be supplied as commandline arguments.
// 

#include "stdafx.h"

int _tmain(int argc, _TCHAR* argv[])
{
    if (argc < 3) {
        std::cerr << "Usage:-\n\n\tnetsend <message> <port>" << std::endl;
        return 1;
    }

    wchar_t* ep = 0;
    int port = ::wcstol(argv[2], &ep, 10);
    if (port == 0 || port == -1 || port > 65535) {
        std::cerr << "Invalid port number specified." << std::endl;
        return 1;
    }

    WSADATA wsad = {0};
    ::WSAStartup(MAKEWORD(2, 2), &wsad);

    SOCKET socket = ::WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0, 0);
    if (socket != INVALID_SOCKET) {
        struct sockaddr_in to = {0};
        to.sin_family = AF_INET;
        to.sin_port = ::htons(port);
        to.sin_addr.s_addr = ::inet_addr("127.0.0.1");
        int cbSent = ::sendto(socket, reinterpret_cast<const char*>(argv[1]),
            ::wcslen(argv[1])*sizeof(_TCHAR), 
            0,
            reinterpret_cast<const sockaddr*>(&to),
            sizeof(to));
        if (cbSent != SOCKET_ERROR) {
            std::cerr << "Sent " << cbSent << " bytes to port " << port << std::endl;
        } else {
            std::cerr << "Error sending data to port " << port << ", error code: " 
                      << ::WSAGetLastError() << std::endl;
        }

        ::closesocket(socket);
    }

    ::WSACleanup();

    return 0;
}

