#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/thread/thread.hpp>

#include <string>
#include <memory>
#include <iostream>
#include <mutex>

static std::mutex iostreamMutex;

namespace ip = boost::asio::ip;
namespace http = boost::beast::http;

class HTTPServer {
public:
    static inline const std::map<std::string, std::string> mimeMap = {
            {".html", "text/html"},
            {".htm",  "text/html"},
            {".css",  "text/css"},
            {".js",   "application/javascript"},
            {".json", "application/json"},
            {".png",  "image/png"},
            {".jpg",  "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".gif",  "image/gif"},
            {".ico",  "image/x-icon"},
            {".txt",  "text/plain"}
    };

private:
    boost::asio::io_context m_ioContext;
    ip::tcp::acceptor m_acceptor;
    uint16_t m_port;
    boost::thread m_ioLoopThread;
    std::atomic<bool> m_isRunning = false;

public:
    // Constructor
    HTTPServer(uint16_t port)
        : m_ioContext(),
        m_acceptor(m_ioContext, ip::tcp::endpoint(ip::address_v4::any(), port)),
        m_port(port),
        m_isRunning(false){
    }

    ~HTTPServer() {
        try {
            stop();
        }
        catch (...) {
            // Best effort cleanup, ignore errors
        }
    }

    void stop();

    // Start accepting connections
    void startNonBlocking();

    // Start accepting connections blocking
    void startBlocking();

private:
    void accept();

    void processRequest(ip::tcp::socket socket);

    std::string mimeType(const std::string& path);

    void serveFile(const std::string& filepath,
        const http::request<http::string_body>& req,
        ip::tcp::socket& socket);

    void sendErrorResponse(const http::request<http::string_body>& req,
        ip::tcp::socket& socket,
        http::status status,
        const std::string& message);

    void handleRequest(http::request<http::string_body>& req, ip::tcp::socket& socket);
};

