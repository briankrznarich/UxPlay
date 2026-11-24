# The pinned IXWebSocket exposes the peer address, but not the destination
# address of an accepted connection. Keep this small additive patch explicit
# and fail on an incompatible upstream change rather than guess an interface.
set(header "${ixwebsocket_SOURCE_DIR}/ixwebsocket/IXConnectionState.h")
file(READ "${header}" source)
if(NOT source MATCHES "getLocalIp")
  if(NOT source MATCHES "int getRemotePort\\(\\);" OR NOT source MATCHES "std::string _remoteIp;")
    message(FATAL_ERROR "IXWebSocket local-address patch needs updating (ConnectionState)")
  endif()
  string(REPLACE "int getRemotePort();" "int getRemotePort();\n        const std::string& getLocalIp() const { return _localIp; }" source "${source}")
  string(REPLACE "std::string _remoteIp;" "std::string _remoteIp;\n        std::string _localIp;" source "${source}")
  file(WRITE "${header}" "${source}")
endif()
set(implementation "${ixwebsocket_SOURCE_DIR}/ixwebsocket/IXSocketServer.cpp")
file(READ "${implementation}" source)
if(NOT source MATCHES "connectionState->_localIp")
  set(anchor "connectionState->setRemoteIp(remoteIp);")
  string(FIND "${source}" "${anchor}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "IXWebSocket local-address patch needs updating (SocketServer)")
  endif()
  string(REPLACE "${anchor}" "${anchor}
            // UxPlay: retain the actual IPv4 destination of this accepted socket.
            sockaddr_in localAddress{};
            socklen_t localSize = sizeof(localAddress);
            if (getsockname(clientFd, reinterpret_cast<sockaddr*>(&localAddress), &localSize) == 0 &&
                localAddress.sin_family == AF_INET)
            {
                char address[INET_ADDRSTRLEN];
                if (ix::inet_ntop(AF_INET, &localAddress.sin_addr, address, sizeof(address)))
                    connectionState->_localIp = address;
            }" source "${source}")
  file(WRITE "${implementation}" "${source}")
endif()
